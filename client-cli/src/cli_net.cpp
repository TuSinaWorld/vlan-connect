#include "cli_net.h"
#include "cli_log.h"
#include <cstring>
#include <algorithm>

extern "C" {
#include "monocypher.h"
}

namespace VLan {

// ======================== CliSignalClient ========================

CliSignalClient::CliSignalClient()
    : m_myPeerId(0), m_pingSentTime(0), m_connectStartTime(0), m_hasPendingAuth(false),
      m_serverAuthRequired(false), m_secureReady(false), m_secureSessionId(0)
{
    memset(m_pendingAuthHash, 0, CIPHER_KEY_SIZE);
    memset(m_clientNonce, 0, sizeof(m_clientNonce));
    memset(m_serverNonce, 0, sizeof(m_serverNonce));
    memset(m_clientPrivKey, 0, sizeof(m_clientPrivKey));
    memset(m_clientPubKey, 0, sizeof(m_clientPubKey));
    memset(m_serverPubKey, 0, sizeof(m_serverPubKey));
}

CliSignalClient::~CliSignalClient() { disconnect(); }

bool CliSignalClient::connectTo(const std::string& ip, uint16_t port) {
    m_myPeerId = 0;
    m_serverAuthRequired = false;
    m_secureReady = false;
    m_secureSessionId = 0;
    m_secureMaster.clear();
    m_connectStartTime = currentTimeMs();
    return m_conn.connectTo(ip, port);
}

void CliSignalClient::disconnect() {
    m_conn.reset();
    m_myPeerId = 0;
}

void CliSignalClient::setServerPassword(const std::string& password) {
    m_serverPassword = password;
}

void CliSignalClient::continueServerAuth() {
    if (m_serverAuthRequired && !m_secureReady)
        sendServerAuth();
}

void CliSignalClient::login(const std::string& name,
                            bool hasResume,
                            uint32_t resumeRoomId,
                            uint32_t resumePeerId,
                            const uint8_t* resumeToken) {
    ByteBuffer bb;
    bb.writeString(name);
    bb.writeU16(PROTOCOL_VERSION);
    bool sendResume = hasResume && resumeToken != nullptr;
    bb.writeU8(sendResume ? 1 : 0);
    if (sendResume) {
        bb.writeU32(resumeRoomId);
        bb.writeU32(resumePeerId);
        bb.writeBytes(resumeToken, RECONNECT_TOKEN_SIZE);
    }
    sendMsg(MSG_LOGIN, bb);
}

void CliSignalClient::createRoom(const std::string& roomName, uint8_t maxPlayers,
                                 RoomTrafficPolicy tcpPolicy,
                                 RoomTrafficPolicy udpPolicy,
                                 uint16_t mtu,
                                 bool passwordProtected, const uint8_t* passwordHash) {
    ByteBuffer bb;
    bb.writeString(roomName);
    bb.writeU8(maxPlayers);
    bb.writeU8(static_cast<uint8_t>(tcpPolicy.transportMode));
    bb.writeU8(static_cast<uint8_t>(tcpPolicy.fecMode));
    bb.writeU8(static_cast<uint8_t>(tcpPolicy.kcpProfile));
    bb.writeU8(static_cast<uint8_t>(udpPolicy.transportMode));
    bb.writeU8(static_cast<uint8_t>(udpPolicy.fecMode));
    bb.writeU8(static_cast<uint8_t>(udpPolicy.kcpProfile));
    bb.writeU8(passwordProtected ? 1 : 0);
    if (passwordProtected && passwordHash)
        bb.writeBytes(passwordHash, 32);
    bb.writeU16(normalizeRoomMtu(mtu));
    sendMsg(MSG_CREATE_ROOM, bb);
}

void CliSignalClient::joinRoom(uint32_t roomId, const uint8_t* authHash) {
    if (authHash) {
        memcpy(m_pendingAuthHash, authHash, CIPHER_KEY_SIZE);
        m_hasPendingAuth = true;
    } else {
        m_hasPendingAuth = false;
    }
    ByteBuffer bb;
    bb.writeU32(roomId);
    sendMsg(MSG_JOIN_ROOM, bb);
}

void CliSignalClient::resumeRoom(uint32_t roomId, uint32_t peerId, const uint8_t* resumeToken) {
    if (!resumeToken) return;
    ByteBuffer bb;
    bb.writeU32(roomId);
    bb.writeU32(peerId);
    bb.writeBytes(resumeToken, RECONNECT_TOKEN_SIZE);
    sendMsg(MSG_RESUME_ROOM, bb);
}

void CliSignalClient::leaveRoom() { sendMsg(MSG_LEAVE_ROOM); }
void CliSignalClient::logout() { sendMsg(MSG_LOGOUT); }
void CliSignalClient::listRooms() { sendMsg(MSG_LIST_ROOMS); }

void CliSignalClient::requestRelay(uint32_t targetPeerId) {
    ByteBuffer bb;
    bb.writeU32(targetPeerId);
    sendMsg(MSG_REQUEST_RELAY, bb);
}

void CliSignalClient::sendPing() {
    m_pingSentTime = currentTimeMs();
    sendMsg(MSG_PING);
}

void CliSignalClient::sendAuthResponse(const uint8_t* response) {
    ByteBuffer bb;
    bb.writeBytes(response, 32);
    sendMsg(MSG_AUTH_RESPONSE, bb);
}

void CliSignalClient::sendClientHello() {
    secureRandomBytes(m_clientNonce, 16);
    secureRandomBytes(m_clientPrivKey, 32);
    crypto_x25519_public_key(m_clientPubKey, m_clientPrivKey);

    ByteBuffer bb;
    bb.writeU16(PROTOCOL_VERSION);
    bb.writeBytes(m_clientNonce, 16);
    bb.writeBytes(m_clientPubKey, 32);
    m_conn.sendTcpMsg(MSG_CLIENT_HELLO, bb);
}

void CliSignalClient::sendServerAuth() {
    if (m_serverPassword.empty()) {
        if (onServerPasswordRequired) onServerPasswordRequired();
        return;
    }

    uint8_t intermediate[CIPHER_KEY_SIZE];
    uint8_t authHash[CIPHER_KEY_SIZE];
    computeIntermediate(reinterpret_cast<const uint8_t*>(m_serverPassword.data()),
                        m_serverPassword.size(), intermediate);
    authHashFromIntermediate(intermediate, authHash);

    uint8_t shared[32];
    uint8_t master[32];
    crypto_x25519(shared, m_clientPrivKey, m_serverPubKey);
    deriveSecureMaster(master, shared, authHash, m_clientNonce, m_serverNonce,
                       m_clientPubKey, m_serverPubKey);
    uint8_t proof[32];
    computeClientAuthProof(proof, master, authHash);

    m_secureMaster.assign(master, master + 32);

    ByteBuffer bb;
    bb.writeBytes(proof, 32);
    m_conn.sendTcpMsg(MSG_SERVER_AUTH, bb);

    crypto_wipe(intermediate, sizeof(intermediate));
    crypto_wipe(authHash, sizeof(authHash));
    crypto_wipe(shared, sizeof(shared));
    crypto_wipe(master, sizeof(master));
    crypto_wipe(proof, sizeof(proof));
}

void CliSignalClient::sendMsg(uint8_t msgType, const ByteBuffer& body) {
    if (m_secureReady &&
        msgType != MSG_CLIENT_HELLO &&
        msgType != MSG_SERVER_AUTH &&
        msgType != MSG_SERVER_AUTH_OK &&
        msgType != MSG_SERVER_HELLO) {
        std::vector<uint8_t> plain;
        plain.reserve(1 + body.size());
        plain.push_back(msgType);
        if (body.size() > 0)
            plain.insert(plain.end(), body.data(), body.data() + body.size());
        std::vector<uint8_t> enc = m_secureCipher.encrypt(plain.data(), plain.size());
        ByteBuffer wrapped;
        if (!enc.empty())
            wrapped.writeBytes(enc.data(), enc.size());
        m_conn.sendTcpMsg(MSG_ENCRYPTED, wrapped);
        return;
    }
    m_conn.sendTcpMsg(msgType, body);
}

void CliSignalClient::sendMsg(uint8_t msgType) {
    ByteBuffer empty;
    sendMsg(msgType, empty);
}

void CliSignalClient::onWritable() {
    if (m_conn.connecting) {
        int err = 0;
        socklen_t errLen = sizeof(err);
        getsockopt(m_conn.fd, SOL_SOCKET, SO_ERROR,
                   reinterpret_cast<char*>(&err), &errLen);
        if (err == 0) {
            m_conn.connecting = false;
            m_conn.connected = true;
            m_conn.lastRecvTime = currentTimeMs();
            LOG_INFO("Signal TCP connected");
            sendClientHello();
        } else {
            LOG_ERR("Signal connect failed: error %d", err);
            if (onConnectFailed) onConnectFailed("connect error");
            m_conn.reset();
        }
        return;
    }
    m_conn.flushSend();
}

void CliSignalClient::onReadable() {
    int n = m_conn.readData();
    if (n <= 0) {
        LOG_INFO("Signal TCP disconnected");
        m_conn.reset();
        if (onDisconnected) onDisconnected();
        return;
    }

    if (m_conn.recvBuf.size() > 1024 * 1024) {
        LOG_ERR("Signal TCP stream corrupted");
        m_conn.reset();
        if (onDisconnected) onDisconnected();
        return;
    }

    while (m_conn.recvBuf.size() >= sizeof(TcpMsgHeader)) {
        const TcpMsgHeader* hdr =
            reinterpret_cast<const TcpMsgHeader*>(m_conn.recvBuf.data());
        uint16_t payloadLen = ntohs(hdr->length);
        if (payloadLen > MAX_TCP_MSG_PAYLOAD) {
            LOG_ERR("Signal TCP stream corrupted (payload too large)");
            m_conn.reset();
            if (onDisconnected) onDisconnected();
            return;
        }
        size_t frameLen = sizeof(TcpMsgHeader) + payloadLen;
        if (m_conn.recvBuf.size() < frameLen) break;

        processMessage(hdr->msgType,
                       m_conn.recvBuf.data() + sizeof(TcpMsgHeader), payloadLen);
        m_conn.recvBuf.erase(m_conn.recvBuf.begin(),
                             m_conn.recvBuf.begin() + frameLen);
    }
}

void CliSignalClient::checkTimeouts() {
    if (!m_conn.connected && m_conn.connecting) {
        if (currentTimeMs() - m_connectStartTime > 8000) {
            LOG_ERR("Signal connect timeout");
            if (onConnectFailed) onConnectFailed("connect timeout");
            m_conn.reset();
        }
        return;
    }
    if (m_conn.connected) {
        uint32_t elapsed = currentTimeMs() - m_conn.lastRecvTime;
        if (elapsed > static_cast<uint32_t>(TCP_RECV_TIMEOUT_MS)) {
            LOG_ERR("Signal TCP recv timeout (%u ms)", elapsed);
            m_conn.reset();
            if (onDisconnected) onDisconnected();
        }
    }
}

void CliSignalClient::processMessage(uint8_t msgType, const uint8_t* payload, size_t len) {
    ByteBuffer bb(payload, len);
    LOG_DBG("[signal] msg type=0x%02x len=%zu", msgType, len);

    if (msgType == MSG_SERVER_HELLO) {
        uint16_t serverVersion = bb.readU16();
        bool authRequired = bb.remaining() > 0 ? (bb.readU8() != 0) : false;
        if (serverVersion != PROTOCOL_VERSION) {
            if (onServerError) onServerError("protocol version mismatch");
            m_conn.reset();
            return;
        }
        m_serverAuthRequired = authRequired;
        if (!authRequired) {
            if (onConnected) onConnected();
            return;
        }
        if (bb.remaining() < 48) {
            if (onServerError) onServerError("invalid server auth hello");
            m_conn.reset();
            return;
        }
        bb.readBytes(m_serverNonce, 16);
        bb.readBytes(m_serverPubKey, 32);
        if (m_serverPassword.empty()) {
            if (onServerPasswordRequired) onServerPasswordRequired();
        } else {
            sendServerAuth();
        }
        return;
    }

    if (msgType == MSG_SERVER_AUTH_OK) {
        if (m_secureMaster.size() != 32 || bb.remaining() < 36) {
            if (onServerError) onServerError("invalid server auth response");
            m_conn.reset();
            return;
        }
        m_secureSessionId = bb.readU32();
        uint8_t serverProof[32];
        bb.readBytes(serverProof, 32);

        uint8_t intermediate[CIPHER_KEY_SIZE];
        uint8_t authHash[CIPHER_KEY_SIZE];
        computeIntermediate(reinterpret_cast<const uint8_t*>(m_serverPassword.data()),
                            m_serverPassword.size(), intermediate);
        authHashFromIntermediate(intermediate, authHash);
        uint8_t expected[32];
        computeServerAuthProof(expected, m_secureMaster.data(), authHash);
        if (crypto_verify32(expected, serverProof) != 0) {
            if (onServerError) onServerError("server auth proof failed");
            m_conn.reset();
            crypto_wipe(intermediate, sizeof(intermediate));
            crypto_wipe(authHash, sizeof(authHash));
            crypto_wipe(expected, sizeof(expected));
            return;
        }
        m_secureCipher.init(m_secureMaster.data(), true, "signal");
        m_secureReady = true;
        if (onSecureSessionEstablished)
            onSecureSessionEstablished(m_secureSessionId, m_secureMaster);
        if (onConnected) onConnected();
        crypto_wipe(intermediate, sizeof(intermediate));
        crypto_wipe(authHash, sizeof(authHash));
        crypto_wipe(expected, sizeof(expected));
        return;
    }

    std::vector<uint8_t> plain;
    if (msgType == MSG_ENCRYPTED) {
        if (!m_secureReady ||
            !m_secureCipher.decrypt(payload, len, &plain) || plain.empty()) {
            m_conn.reset();
            if (onDisconnected) onDisconnected();
            return;
        }
        msgType = plain[0];
        payload = plain.size() > 1 ? plain.data() + 1 : nullptr;
        len = plain.size() > 1 ? plain.size() - 1 : 0;
        bb = len > 0 ? ByteBuffer(payload, len) : ByteBuffer();
    } else if (m_secureReady) {
        m_conn.reset();
        if (onDisconnected) onDisconnected();
        return;
    }

    switch (msgType) {
    case MSG_LOGIN_RESP: {
        m_myPeerId = bb.readU32();
        uint16_t ver = bb.remaining() >= 2 ? bb.readU16() : 1;
        bool resumeAccepted = bb.remaining() > 0 ? (bb.readU8() != 0) : false;
        LOG_INFO("Login OK, peerId=%u serverVersion=%u resume=%u",
                 m_myPeerId, ver, resumeAccepted ? 1 : 0);
        if (ver != PROTOCOL_VERSION)
            LOG_ERR("Protocol version mismatch: client=%u server=%u", PROTOCOL_VERSION, ver);
        if (onLoginResponse) onLoginResponse(m_myPeerId, resumeAccepted);
        break;
    }
    case MSG_ROOM_CREATED: {
        uint32_t roomId = bb.readU32();
        uint32_t vip    = bb.readU32();
        RoomTrafficPolicy tcpPolicy = normalizeTrafficPolicy(
            bb.readU8(), bb.readU8(), bb.readU8(), makeDefaultTcpPolicy());
        RoomTrafficPolicy udpPolicy = normalizeTrafficPolicy(
            bb.readU8(), bb.readU8(), bb.readU8(), makeDefaultUdpPolicy());
        bool passwordProtected = bb.remaining() > 0 ? (bb.readU8() != 0) : false;
        uint16_t mtu = bb.remaining() >= 2 ? normalizeRoomMtu(bb.readU16()) : ROOM_MTU_DEFAULT;
        Buffer leaseToken;
        if (bb.remaining() >= RECONNECT_TOKEN_SIZE) {
            leaseToken.resize(RECONNECT_TOKEN_SIZE);
            bb.readBytes(leaseToken.data(), RECONNECT_TOKEN_SIZE);
        }
        if (onRoomCreated) onRoomCreated(roomId, vip, tcpPolicy, udpPolicy, mtu, passwordProtected, leaseToken);
        break;
    }
    case MSG_JOIN_RESP: {
        uint32_t roomId = bb.readU32();
        uint32_t vip    = bb.readU32();
        RoomTrafficPolicy tcpPolicy = normalizeTrafficPolicy(
            bb.readU8(), bb.readU8(), bb.readU8(), makeDefaultTcpPolicy());
        RoomTrafficPolicy udpPolicy = normalizeTrafficPolicy(
            bb.readU8(), bb.readU8(), bb.readU8(), makeDefaultUdpPolicy());
        bool passwordProtected = bb.remaining() > 0 ? (bb.readU8() != 0) : false;
        uint16_t mtu = bb.remaining() >= 2 ? normalizeRoomMtu(bb.readU16()) : ROOM_MTU_DEFAULT;
        uint8_t count = bb.readU8();
        std::vector<PeerInfo> members;
        for (uint8_t i = 0; i < count; ++i) {
            PeerInfo pi;
            pi.peerId     = bb.readU32();
            pi.virtualIP  = bb.readU32();
            pi.name       = bb.readString();
            pi.transport  = TRANSPORT_NONE;
            members.push_back(pi);
        }
        Buffer leaseToken;
        if (bb.remaining() >= RECONNECT_TOKEN_SIZE) {
            leaseToken.resize(RECONNECT_TOKEN_SIZE);
            bb.readBytes(leaseToken.data(), RECONNECT_TOKEN_SIZE);
        }
        m_hasPendingAuth = false;
        if (onJoinResponse) onJoinResponse(roomId, vip, tcpPolicy, udpPolicy, mtu, passwordProtected, members, leaseToken);
        break;
    }
    case MSG_PEER_JOINED: {
        PeerInfo pi;
        pi.peerId     = bb.readU32();
        pi.virtualIP  = bb.readU32();
        pi.name       = bb.readString();
        pi.transport  = TRANSPORT_NONE;
        if (onPeerJoined) onPeerJoined(pi);
        break;
    }
    case MSG_PEER_RESUMED: {
        PeerInfo pi;
        pi.peerId     = bb.readU32();
        pi.virtualIP  = bb.readU32();
        pi.name       = bb.readString();
        pi.transport  = TRANSPORT_NONE;
        LOG_INFO("[signal] PEER_RESUMED peerId=%u ip=%s name=%s",
                 pi.peerId, ipToString(pi.virtualIP).c_str(), pi.name.c_str());
        if (onPeerResumed) onPeerResumed(pi);
        break;
    }
    case MSG_PEER_LEFT: {
        uint32_t peerId = bb.readU32();
        if (onPeerLeft) onPeerLeft(peerId);
        break;
    }
    case MSG_LOGOUT_ACK: {
        m_myPeerId = 0;
        if (onLogoutAck) onLogoutAck();
        break;
    }
    case MSG_ROOM_LIST:
    case MSG_ROOM_LIST_PUSH: {
        uint16_t count = bb.readU16();
        std::vector<CliRoomListItem> rooms;
        for (uint16_t i = 0; i < count; ++i) {
            CliRoomListItem ri;
            ri.roomId        = bb.readU32();
            ri.roomName      = bb.readString();
            ri.playerCount   = bb.readU8();
            ri.maxPlayers    = bb.readU8();
            ri.tcpPolicy = normalizeTrafficPolicy(
                bb.readU8(), bb.readU8(), bb.readU8(), makeDefaultTcpPolicy());
            ri.udpPolicy = normalizeTrafficPolicy(
                bb.readU8(), bb.readU8(), bb.readU8(), makeDefaultUdpPolicy());
            ri.passwordProtected = bb.remaining() > 0 ? bb.readU8() : 0;
            ri.mtu = bb.remaining() >= 2 ? normalizeRoomMtu(bb.readU16()) : ROOM_MTU_DEFAULT;
            rooms.push_back(ri);
        }
        if (onRoomList) onRoomList(rooms, msgType == MSG_ROOM_LIST_PUSH);
        break;
    }
    case MSG_RELAY_READY: {
        uint32_t peerId = bb.readU32();
        if (onRelayReady) onRelayReady(peerId);
        break;
    }
    case MSG_PONG: {
        if (m_pingSentTime != 0) {
            int rtt = static_cast<int>(currentTimeMs() - m_pingSentTime);
            m_pingSentTime = 0;
            if (onServerRtt) onServerRtt(rtt);
        }
        break;
    }
    case MSG_AUTH_CHALLENGE: {
        if (m_hasPendingAuth && bb.remaining() >= 32) {
            uint8_t challenge[CIPHER_CHALLENGE_SIZE];
            bb.readBytes(challenge, 32);
            uint8_t response[CIPHER_KEY_SIZE];
            computeChallengeResponse(m_pendingAuthHash, challenge, response);
            sendAuthResponse(response);
            LOG_INFO("Auth challenge received, response sent");
        } else {
            LOG_ERR("Auth challenge but no pending auth hash");
            m_hasPendingAuth = false;
        }
        break;
    }
    case MSG_ERROR: {
        std::string errMsg = bb.readString();
        LOG_ERR("Server error: %s", errMsg.c_str());
        m_hasPendingAuth = false;
        if (onServerError) onServerError(errMsg);
        break;
    }
    default:
        LOG_DBG("[signal] Unknown message type: 0x%02x", msgType);
        break;
    }
}

// ======================== CliDataChannel ========================

CliDataChannel::CliDataChannel()
    : m_port(0), m_peerId(0), m_established(false),
      m_needReconnect(false), m_reconnectTime(0),
      m_secureEnabled(false), m_secureSessionId(0)
{}

CliDataChannel::~CliDataChannel() { disconnect(); }

bool CliDataChannel::connectTo(const std::string& ip, uint16_t port, uint32_t peerId) {
    m_host = ip;
    m_port = port;
    m_peerId = peerId;
    m_established = false;
    m_needReconnect = false;
    return m_conn.connectTo(ip, port);
}

void CliDataChannel::disconnect() {
    m_conn.reset();
    m_established = false;
    m_needReconnect = false;
    m_peerId = 0;
    m_port = 0;
}

void CliDataChannel::setSecureSession(uint32_t sessionId, const Buffer& master) {
    m_secureSessionId = sessionId;
    m_secureMaster = master;
    m_secureEnabled = (sessionId != 0 && master.size() == 32);
    if (m_secureEnabled)
        m_cipher.init(m_secureMaster.data(), true, "data");
}

void CliDataChannel::sendRelayData(uint32_t srcPeerId, uint32_t dstPeerId,
                                   TrafficClass cls, const Buffer& data) {
    if (!m_established) return;
    ByteBuffer bb;
    bb.writeU32(srcPeerId);
    bb.writeU32(dstPeerId);
    bb.writeU8(static_cast<uint8_t>(cls));
    if (!data.empty())
        bb.writeBytes(data.data(), data.size());
    sendMsg(MSG_TCP_RELAY_DATA, bb);
}

void CliDataChannel::sendPing() {
    if (!m_established) return;
    sendMsg(MSG_PING);
}

void CliDataChannel::onWritable() {
    if (m_conn.connecting) {
        int err = 0;
        socklen_t errLen = sizeof(err);
        getsockopt(m_conn.fd, SOL_SOCKET, SO_ERROR,
                   reinterpret_cast<char*>(&err), &errLen);
        if (err == 0) {
            m_conn.connecting = false;
            m_conn.connected = true;
            m_conn.lastRecvTime = currentTimeMs();

            ByteBuffer bb;
            if (m_secureEnabled) {
                ByteBuffer inner;
                inner.writeU32(m_peerId);
                std::vector<uint8_t> enc = m_cipher.encrypt(inner.data(), inner.size());
                uint8_t sid[SECURE_SESSION_ID_SIZE];
                writeU32BE(sid, m_secureSessionId);
                bb.writeBytes(sid, SECURE_SESSION_ID_SIZE);
                if (!enc.empty())
                    bb.writeBytes(enc.data(), enc.size());
            } else {
                bb.writeU32(m_peerId);
            }
            m_conn.sendTcpMsg(MSG_DATA_CHANNEL_INIT, bb);
            LOG_DBG("[datachannel] TCP connected, sent INIT for peer %u", m_peerId);
        } else {
            LOG_ERR("[datachannel] connect failed: error %d", err);
            m_conn.reset();
            scheduleReconnect();
        }
        return;
    }
    m_conn.flushSend();
}

void CliDataChannel::onReadable() {
    int n = m_conn.readData();
    if (n <= 0) {
        bool wasEst = m_established;
        m_conn.reset();
        m_established = false;
        if (wasEst && onDisconnectedCb) onDisconnectedCb();
        if (m_peerId != 0) scheduleReconnect();
        return;
    }

    while (m_conn.recvBuf.size() >= sizeof(TcpMsgHeader)) {
        const TcpMsgHeader* hdr =
            reinterpret_cast<const TcpMsgHeader*>(m_conn.recvBuf.data());
        uint16_t payloadLen = ntohs(hdr->length);
        if (payloadLen > MAX_TCP_MSG_PAYLOAD) {
            m_conn.reset();
            m_established = false;
            return;
        }
        size_t frameLen = sizeof(TcpMsgHeader) + payloadLen;
        if (m_conn.recvBuf.size() < frameLen) break;

        processMessage(hdr->msgType,
                       m_conn.recvBuf.data() + sizeof(TcpMsgHeader), payloadLen);
        m_conn.recvBuf.erase(m_conn.recvBuf.begin(),
                             m_conn.recvBuf.begin() + frameLen);
    }
}

void CliDataChannel::processMessage(uint8_t msgType, const uint8_t* payload, size_t len) {
    std::vector<uint8_t> plain;
    if (msgType == MSG_ENCRYPTED) {
        if (!m_secureEnabled ||
            !m_cipher.decrypt(payload, len, &plain) || plain.empty())
            return;
        msgType = plain[0];
        payload = plain.size() > 1 ? plain.data() + 1 : nullptr;
        len = plain.size() > 1 ? plain.size() - 1 : 0;
    } else if (m_secureEnabled) {
        m_conn.reset();
        m_established = false;
        if (onDisconnectedCb) onDisconnectedCb();
        if (m_peerId != 0)
            scheduleReconnect();
        return;
    }

    switch (msgType) {
    case MSG_DATA_CHANNEL_ACK:
        m_established = true;
        m_conn.lastRecvTime = currentTimeMs();
        LOG_INFO("Data channel established");
        if (onConnectedCb) onConnectedCb();
        break;
    case MSG_TCP_RELAY_DATA: {
        if (len < 9) break;
        ByteBuffer bb(payload, len);
        uint32_t srcId = bb.readU32();
        bb.readU32();
        TrafficClass cls = bb.readU8() == TRAFFIC_TCP ? TRAFFIC_TCP : TRAFFIC_UDP;
        size_t remaining = bb.remaining();
        Buffer data(payload + len - remaining, payload + len);
        if (onRelayData) onRelayData(srcId, cls, data);
        break;
    }
    case MSG_PONG:
        break;
    default:
        break;
    }
}

void CliDataChannel::sendMsg(uint8_t msgType, const ByteBuffer& body) {
    if (m_secureEnabled && msgType != MSG_DATA_CHANNEL_INIT) {
        std::vector<uint8_t> plain;
        plain.reserve(1 + body.size());
        plain.push_back(msgType);
        if (body.size() > 0)
            plain.insert(plain.end(), body.data(), body.data() + body.size());
        std::vector<uint8_t> enc = m_cipher.encrypt(plain.data(), plain.size());
        ByteBuffer wrapped;
        if (!enc.empty())
            wrapped.writeBytes(enc.data(), enc.size());
        m_conn.sendTcpMsg(MSG_ENCRYPTED, wrapped);
        return;
    }
    m_conn.sendTcpMsg(msgType, body);
}

void CliDataChannel::sendMsg(uint8_t msgType) {
    ByteBuffer empty;
    sendMsg(msgType, empty);
}

void CliDataChannel::checkTimeouts() {
    if (m_needReconnect && currentTimeMs() >= m_reconnectTime) {
        m_needReconnect = false;
        if (m_peerId != 0 && m_conn.fd == SOCK_INVALID) {
            LOG_DBG("[datachannel] Attempting reconnect");
            connectTo(m_host, m_port, m_peerId);
        }
    }
    if (m_established) {
        uint32_t elapsed = currentTimeMs() - m_conn.lastRecvTime;
        if (elapsed > static_cast<uint32_t>(TCP_RELAY_DEAD_MS)) {
            LOG_ERR("[datachannel] No data for %u ms, reconnecting", elapsed);
            m_conn.reset();
            m_established = false;
            if (onDisconnectedCb) onDisconnectedCb();
            scheduleReconnect();
        }
    }
}

void CliDataChannel::scheduleReconnect() {
    m_needReconnect = true;
    m_reconnectTime = currentTimeMs() + RECONNECT_INTERVAL_MS;
}

} // namespace VLan

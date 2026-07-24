#include "cli_net.h"
#include "cli_log.h"
#include "signal_message_validator.h"
#include "tcp_frame_probe.h"
#include <cstring>
#include <algorithm>

extern "C" {
#include "monocypher.h"
}

namespace VLan {

// ======================== CliSignalClient ========================

CliSignalClient::CliSignalClient()
    : m_myPeerId(0), m_pingSentTime(0), m_connectStartTime(0), m_hasPendingAuth(false),
      m_serverAuthRequired(false), m_secureReady(false),
      m_disconnectNotified(false), m_secureSessionId(0)
{
    memset(m_pendingAuthHash, 0, CIPHER_KEY_SIZE);
    memset(m_clientNonce, 0, sizeof(m_clientNonce));
    memset(m_serverNonce, 0, sizeof(m_serverNonce));
    memset(m_clientPrivKey, 0, sizeof(m_clientPrivKey));
    memset(m_clientPubKey, 0, sizeof(m_clientPubKey));
    memset(m_serverPubKey, 0, sizeof(m_serverPubKey));
}

CliSignalClient::~CliSignalClient() { disconnect(); }

void CliSignalClient::resetSecureState() {
    m_serverAuthRequired = false;
    m_secureReady = false;
    m_secureSessionId = 0;
    m_secureCipher.reset();
    if (!m_secureMaster.empty()) {
        crypto_wipe(m_secureMaster.data(), m_secureMaster.size());
        m_secureMaster.clear();
    }
    crypto_wipe(m_clientNonce, sizeof(m_clientNonce));
    crypto_wipe(m_serverNonce, sizeof(m_serverNonce));
    crypto_wipe(m_clientPrivKey, sizeof(m_clientPrivKey));
    crypto_wipe(m_clientPubKey, sizeof(m_clientPubKey));
    crypto_wipe(m_serverPubKey, sizeof(m_serverPubKey));
}

bool CliSignalClient::connectTo(const std::string& ip, uint16_t port) {
    m_myPeerId = 0;
    resetSecureState();
    m_disconnectNotified = false;
    m_connectStartTime = currentTimeMs();
    return m_conn.connectTo(ip, port);
}

void CliSignalClient::disconnect() {
    m_conn.reset();
    resetSecureState();
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

void CliSignalClient::sendRelayData(uint32_t srcPeerId, uint32_t dstPeerId,
                                    TrafficClass cls, const Buffer& data) {
    ByteBuffer bb;
    bb.writeU32(srcPeerId);
    bb.writeU32(dstPeerId);
    bb.writeU8(static_cast<uint8_t>(cls));
    if (!data.empty())
        bb.writeBytes(data.data(), data.size());
    sendMsg(MSG_TCP_RELAY_DATA, bb);
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
            m_conn.reset();
            if (onConnectFailed) onConnectFailed("connect error");
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
        resetSecureState();
        notifyDisconnectedOnce();
        return;
    }

    if (m_conn.recvBuf.size() > 1024 * 1024) {
        failSignalFrame(0, m_conn.recvBuf.size(),
                        "receive-buffer-too-large", 0);
        return;
    }

    static const size_t FRAME_HEADER_SIZE = 3;
    while (m_conn.recvBuf.size() >= FRAME_HEADER_SIZE) {
        const TcpFrameProbeResult frame = probeTcpFrame(
            m_conn.recvBuf.data(), m_conn.recvBuf.size());
        if (frame.status == TcpFrameProbeStatus::Malformed) {
            failSignalFrame(frame.msgType, frame.payloadLength,
                            "payload-too-large", 0);
            return;
        }
        if (frame.status == TcpFrameProbeStatus::NeedMore)
            break;

        const uint8_t msgType = frame.msgType;
        const uint16_t payloadLen = frame.payloadLength;
        const size_t frameLen = frame.frameLength;
        Buffer payload;
        if (payloadLen > 0) {
            payload.assign(m_conn.recvBuf.begin() + FRAME_HEADER_SIZE,
                           m_conn.recvBuf.begin() + frameLen);
        }
        m_conn.recvBuf.erase(m_conn.recvBuf.begin(),
                             m_conn.recvBuf.begin() + frameLen);
        const uint8_t* payloadData =
            payload.empty() ? nullptr : payload.data();
        if (!processMessage(msgType, payloadData, payload.size()))
            return;
        if (!m_conn.connected)
            return;
    }
}

void CliSignalClient::notifyDisconnectedOnce() {
    if (m_disconnectNotified) return;
    m_disconnectNotified = true;
    if (onDisconnected) onDisconnected();
}

void CliSignalClient::failSignalFrame(uint8_t msgType, size_t len,
                                      const char* error, size_t offset) {
    LOG_ERR("[signal] Invalid frame type=0x%02x len=%zu error=%s offset=%zu",
            msgType, len, error ? error : "unknown", offset);
    m_conn.reset();
    resetSecureState();
    notifyDisconnectedOnce();
}

void CliSignalClient::checkTimeouts() {
    if (!m_conn.connected && m_conn.connecting) {
        if (currentTimeMs() - m_connectStartTime > 8000) {
            LOG_ERR("Signal connect timeout");
            m_conn.reset();
            if (onConnectFailed) onConnectFailed("connect timeout");
        }
        return;
    }
    if (m_conn.connected) {
        uint32_t elapsed = currentTimeMs() - m_conn.lastRecvTime;
        if (elapsed > static_cast<uint32_t>(TCP_RECV_TIMEOUT_MS)) {
            LOG_ERR("Signal TCP recv timeout (%u ms)", elapsed);
            m_conn.reset();
            resetSecureState();
            notifyDisconnectedOnce();
        }
    }
}

bool CliSignalClient::processMessage(uint8_t msgType,
                                     const uint8_t* payload, size_t len) {
    try {
        MessageValidationResult validation =
            validateServerSignalPayload(msgType, payload, len);
        if (validation.status == MessageValidationStatus::Malformed) {
            if (validation.error == MessageValidationError::InvalidVersion &&
                onServerError)
                onServerError("protocol version mismatch");
            failSignalFrame(msgType, len,
                            messageValidationErrorName(validation.error),
                            validation.offset);
            return false;
        }
        if (validation.status == MessageValidationStatus::UnknownType) {
            LOG_DBG("[signal] Ignoring unknown outer type=0x%02x len=%zu",
                    msgType, len);
            return true;
        }

        std::vector<uint8_t> plain;
        if (msgType == MSG_ENCRYPTED) {
            if (!m_secureReady ||
                !m_secureCipher.decrypt(payload, len, &plain) ||
                plain.empty()) {
                failSignalFrame(msgType, len, "decrypt-failed", 0);
                return false;
            }
            msgType = plain[0];
            payload = plain.size() > 1 ? plain.data() + 1 : nullptr;
            len = plain.size() > 1 ? plain.size() - 1 : 0;
            validation = validateServerSignalPayload(msgType, payload, len);
            if (validation.status == MessageValidationStatus::Malformed) {
                if (validation.error ==
                        MessageValidationError::InvalidVersion &&
                    onServerError) {
                    onServerError("protocol version mismatch");
                }
                failSignalFrame(msgType, len,
                                messageValidationErrorName(validation.error),
                                validation.offset);
                return false;
            }
        } else if (m_secureReady) {
            failSignalFrame(msgType, len, "plaintext-in-secure-mode", 0);
            return false;
        }

        LOG_DBG("[signal] Received type=0x%02x len=%zu", msgType, len);
        if (validation.status == MessageValidationStatus::UnknownType)
            return true;

        ByteBuffer bb(payload, len);
        if (msgType == MSG_SERVER_HELLO) {
            const uint16_t serverVersion = bb.readU16();
            const bool authRequired = bb.readU8() != 0;
            if (serverVersion != PROTOCOL_VERSION) {
                if (onServerError) onServerError("protocol version mismatch");
                failSignalFrame(msgType, len, "invalid-version", 0);
                return false;
            }
            m_serverAuthRequired = authRequired;
            if (!authRequired) {
                if (onConnected) onConnected();
                return true;
            }
            bb.readBytes(m_serverNonce, 16);
            bb.readBytes(m_serverPubKey, 32);
            if (m_serverPassword.empty()) {
                if (onServerPasswordRequired) onServerPasswordRequired();
            } else {
                sendServerAuth();
            }
            return true;
        }

        if (msgType == MSG_SERVER_AUTH_OK) {
            if (m_secureMaster.size() != SECURE_KEY_SIZE) {
                if (onServerError) onServerError("invalid server auth response");
                failSignalFrame(msgType, len, "auth-state-invalid", 0);
                return false;
            }
            const uint32_t sessionId = bb.readU32();
            uint8_t serverProof[SECURE_KEY_SIZE];
            bb.readBytes(serverProof, sizeof(serverProof));
            uint8_t intermediate[CIPHER_KEY_SIZE];
            uint8_t authHash[CIPHER_KEY_SIZE];
            uint8_t expected[SECURE_KEY_SIZE];
            computeIntermediate(
                reinterpret_cast<const uint8_t*>(m_serverPassword.data()),
                m_serverPassword.size(), intermediate);
            authHashFromIntermediate(intermediate, authHash);
            computeServerAuthProof(expected, m_secureMaster.data(), authHash);
            const bool proofValid =
                crypto_verify32(expected, serverProof) == 0;
            crypto_wipe(intermediate, sizeof(intermediate));
            crypto_wipe(authHash, sizeof(authHash));
            crypto_wipe(expected, sizeof(expected));
            crypto_wipe(serverProof, sizeof(serverProof));

            if (!proofValid) {
                if (onServerError) onServerError("server auth proof failed");
                failSignalFrame(msgType, len, "invalid-server-proof", 0);
                return false;
            }
            m_secureSessionId = sessionId;
            m_secureCipher.init(m_secureMaster.data(), true, "signal");
            m_secureReady = true;
            if (onSecureSessionEstablished)
                onSecureSessionEstablished(m_secureSessionId, m_secureMaster);
            if (!m_conn.connected) return false;
            if (onConnected) onConnected();
            return true;
        }

        switch (msgType) {
        case MSG_LOGIN_RESP: {
            const uint32_t peerId = bb.readU32();
            const uint16_t serverVersion = bb.readU16();
            const bool resumeAccepted = bb.readU8() != 0;
            if (serverVersion != PROTOCOL_VERSION) {
                if (onServerError) onServerError("protocol version mismatch");
                failSignalFrame(msgType, len, "invalid-version", 4);
                return false;
            }
            m_myPeerId = peerId;
            if (onLoginResponse) onLoginResponse(peerId, resumeAccepted);
            return true;
        }
        case MSG_ROOM_CREATED: {
            const uint32_t roomId = bb.readU32();
            const uint32_t virtualIP = bb.readU32();
            const RoomTrafficPolicy tcpPolicy = normalizeTrafficPolicy(
                bb.readU8(), bb.readU8(), bb.readU8(),
                makeDefaultTcpPolicy());
            const RoomTrafficPolicy udpPolicy = normalizeTrafficPolicy(
                bb.readU8(), bb.readU8(), bb.readU8(),
                makeDefaultUdpPolicy());
            const bool passwordProtected = bb.readU8() != 0;
            const uint16_t mtu = bb.readU16();
            Buffer leaseToken(RECONNECT_TOKEN_SIZE);
            bb.readBytes(leaseToken.data(), leaseToken.size());
            if (onRoomCreated)
                onRoomCreated(roomId, virtualIP, tcpPolicy, udpPolicy, mtu,
                              passwordProtected, leaseToken);
            return true;
        }
        case MSG_JOIN_RESP: {
            const uint32_t roomId = bb.readU32();
            const uint32_t virtualIP = bb.readU32();
            const RoomTrafficPolicy tcpPolicy = normalizeTrafficPolicy(
                bb.readU8(), bb.readU8(), bb.readU8(),
                makeDefaultTcpPolicy());
            const RoomTrafficPolicy udpPolicy = normalizeTrafficPolicy(
                bb.readU8(), bb.readU8(), bb.readU8(),
                makeDefaultUdpPolicy());
            const bool passwordProtected = bb.readU8() != 0;
            const uint16_t mtu = bb.readU16();
            const uint8_t count = bb.readU8();
            std::vector<PeerInfo> members;
            members.reserve(count);
            for (uint8_t i = 0; i < count; ++i) {
                PeerInfo info;
                info.peerId = bb.readU32();
                info.virtualIP = bb.readU32();
                info.name = bb.readString();
                info.transport = TRANSPORT_NONE;
                members.push_back(info);
            }
            Buffer leaseToken(RECONNECT_TOKEN_SIZE);
            bb.readBytes(leaseToken.data(), leaseToken.size());
            m_hasPendingAuth = false;
            if (onJoinResponse)
                onJoinResponse(roomId, virtualIP, tcpPolicy, udpPolicy, mtu,
                               passwordProtected, members, leaseToken);
            return true;
        }
        case MSG_PEER_JOINED:
        case MSG_PEER_RESUMED: {
            PeerInfo info;
            info.peerId = bb.readU32();
            info.virtualIP = bb.readU32();
            info.name = bb.readString();
            info.transport = TRANSPORT_NONE;
            if (msgType == MSG_PEER_JOINED) {
                if (onPeerJoined) onPeerJoined(info);
            } else {
                if (onPeerResumed) onPeerResumed(info);
            }
            return true;
        }
        case MSG_PEER_LEFT: {
            const uint32_t peerId = bb.readU32();
            if (onPeerLeft) onPeerLeft(peerId);
            return true;
        }
        case MSG_LOGOUT_ACK:
            m_myPeerId = 0;
            if (onLogoutAck) onLogoutAck();
            return true;

        case MSG_ROOM_LIST:
        case MSG_ROOM_LIST_PUSH: {
            const uint16_t count = bb.readU16();
            std::vector<CliRoomListItem> rooms;
            rooms.reserve(count);
            for (uint16_t i = 0; i < count; ++i) {
                CliRoomListItem info;
                info.roomId = bb.readU32();
                info.roomName = bb.readString();
                info.playerCount = bb.readU8();
                info.maxPlayers = bb.readU8();
                info.tcpPolicy = normalizeTrafficPolicy(
                    bb.readU8(), bb.readU8(), bb.readU8(),
                    makeDefaultTcpPolicy());
                info.udpPolicy = normalizeTrafficPolicy(
                    bb.readU8(), bb.readU8(), bb.readU8(),
                    makeDefaultUdpPolicy());
                info.passwordProtected = bb.readU8();
                info.mtu = bb.readU16();
                rooms.push_back(info);
            }
            if (onRoomList)
                onRoomList(rooms, msgType == MSG_ROOM_LIST_PUSH);
            return true;
        }
        case MSG_RELAY_READY: {
            const uint32_t peerId = bb.readU32();
            if (onRelayReady) onRelayReady(peerId);
            return true;
        }
        case MSG_TCP_RELAY_DATA: {
            const uint32_t srcPeerId = bb.readU32();
            bb.readU32();
            const TrafficClass cls =
                static_cast<TrafficClass>(bb.readU8());
            const size_t dataLen = bb.remaining();
            Buffer data;
            if (dataLen > 0) {
                const uint8_t* dataStart =
                    payload + len - dataLen;
                data.assign(dataStart, dataStart + dataLen);
            }
            if (onRelayData)
                onRelayData(srcPeerId, cls, data);
            return true;
        }
        case MSG_PONG:
            if (m_pingSentTime != 0) {
                const int rtt =
                    static_cast<int>(currentTimeMs() - m_pingSentTime);
                m_pingSentTime = 0;
                if (onServerRtt) onServerRtt(rtt);
            }
            return true;

        case MSG_AUTH_CHALLENGE:
            if (m_hasPendingAuth) {
                uint8_t challenge[CIPHER_CHALLENGE_SIZE];
                uint8_t response[CIPHER_KEY_SIZE];
                bb.readBytes(challenge, sizeof(challenge));
                computeChallengeResponse(
                    m_pendingAuthHash, challenge, response);
                sendAuthResponse(response);
                crypto_wipe(challenge, sizeof(challenge));
                crypto_wipe(response, sizeof(response));
            } else {
                m_hasPendingAuth = false;
                LOG_ERR("[signal] Unexpected auth challenge type=0x%02x len=%zu",
                        msgType, len);
            }
            return true;

        case MSG_ERROR: {
            const std::string message = bb.readString();
            m_hasPendingAuth = false;
            if (onServerError) onServerError(message);
            return true;
        }
        default:
            return true;
        }
    } catch (const std::exception&) {
        failSignalFrame(msgType, len, "parser-exception", 0);
        return false;
    } catch (...) {
        failSignalFrame(msgType, len, "unknown-parser-exception", 0);
        return false;
    }
}

// ======================== CliDataChannel ========================

CliDataChannel::CliDataChannel()
    : m_port(0), m_peerId(0), m_established(false),
      m_needReconnect(false), m_reconnectTime(0),
      m_securityMode(DataPlaneSecurityMode::Unconfigured),
      m_secureSessionId(0)
{}

CliDataChannel::~CliDataChannel() {
    disconnect();
    clearSecurityContext();
}

bool CliDataChannel::connectTo(const std::string& ip, uint16_t port, uint32_t peerId) {
    if (m_securityMode == DataPlaneSecurityMode::Unconfigured) {
        LOG_ERR("[datachannel] Refusing connection with unconfigured security mode");
        return false;
    }
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

void CliDataChannel::configurePlaintextSession() {
    if (m_conn.fd != SOCK_INVALID) {
        LOG_ERR("[datachannel] Cannot change security mode while connected");
        return;
    }
    if (!m_secureMaster.empty())
        crypto_wipe(m_secureMaster.data(), m_secureMaster.size());
    m_secureMaster.clear();
    m_secureSessionId = 0;
    m_cipher.reset();
    m_securityMode = DataPlaneSecurityMode::Plaintext;
}

bool CliDataChannel::installSecureSession(uint32_t sessionId,
                                          const Buffer& master) {
    if (m_conn.fd != SOCK_INVALID) {
        LOG_ERR("[datachannel] Cannot change security mode while connected");
        return false;
    }
    if (sessionId == 0 || master.size() != SECURE_KEY_SIZE) {
        clearSecurityContext();
        return false;
    }
    if (!m_secureMaster.empty())
        crypto_wipe(m_secureMaster.data(), m_secureMaster.size());
    m_secureSessionId = sessionId;
    m_secureMaster = master;
    m_cipher.init(m_secureMaster.data(), true, "data");
    m_securityMode = DataPlaneSecurityMode::Secure;
    return true;
}

void CliDataChannel::clearSecurityContext() {
    if (m_conn.fd != SOCK_INVALID) {
        LOG_ERR("[datachannel] Refusing to clear security while connected");
        return;
    }
    if (!m_secureMaster.empty())
        crypto_wipe(m_secureMaster.data(), m_secureMaster.size());
    m_secureMaster.clear();
    m_secureSessionId = 0;
    m_cipher.reset();
    m_securityMode = DataPlaneSecurityMode::Unconfigured;
}

void CliDataChannel::sendRelayData(uint32_t srcPeerId, uint32_t dstPeerId,
                                   TrafficClass cls, const Buffer& data) {
    if (!m_established ||
        m_securityMode == DataPlaneSecurityMode::Unconfigured)
        return;
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
            if (m_securityMode == DataPlaneSecurityMode::Secure) {
                ByteBuffer inner;
                inner.writeU32(m_peerId);
                std::vector<uint8_t> enc = m_cipher.encrypt(inner.data(), inner.size());
                uint8_t sid[SECURE_SESSION_ID_SIZE];
                writeU32BE(sid, m_secureSessionId);
                bb.writeBytes(sid, SECURE_SESSION_ID_SIZE);
                if (!enc.empty())
                    bb.writeBytes(enc.data(), enc.size());
            } else if (m_securityMode == DataPlaneSecurityMode::Plaintext) {
                bb.writeU32(m_peerId);
            } else {
                m_conn.reset();
                return;
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

    static const size_t FRAME_HEADER_SIZE = 3;
    while (m_conn.recvBuf.size() >= FRAME_HEADER_SIZE) {
        const TcpFrameProbeResult frame = probeTcpFrame(
            m_conn.recvBuf.data(), m_conn.recvBuf.size());
        if (frame.status == TcpFrameProbeStatus::Malformed) {
            failDataChannelFrame(frame.msgType, frame.payloadLength,
                                 "payload-too-large", 0);
            return;
        }
        if (frame.status == TcpFrameProbeStatus::NeedMore)
            break;

        const uint8_t msgType = frame.msgType;
        const uint16_t payloadLen = frame.payloadLength;
        const size_t frameLen = frame.frameLength;
        Buffer payload;
        if (payloadLen > 0) {
            payload.assign(m_conn.recvBuf.begin() + FRAME_HEADER_SIZE,
                           m_conn.recvBuf.begin() + frameLen);
        }
        m_conn.recvBuf.erase(m_conn.recvBuf.begin(),
                             m_conn.recvBuf.begin() + frameLen);
        const uint8_t* payloadData =
            payload.empty() ? nullptr : payload.data();
        if (!processMessage(msgType, payloadData, payload.size()))
            return;
        if (!m_conn.connected)
            return;
    }
}

bool CliDataChannel::processMessage(uint8_t msgType,
                                    const uint8_t* payload, size_t len) {
    try {
        MessageValidationResult validation =
            validateServerDataPayload(msgType, payload, len);
        if (validation.status == MessageValidationStatus::Malformed) {
            failDataChannelFrame(
                msgType, len, messageValidationErrorName(validation.error),
                validation.offset);
            return false;
        }
        if (validation.status == MessageValidationStatus::UnknownType) {
            LOG_DBG("[datachannel] Ignoring unknown outer type=0x%02x len=%zu",
                    msgType, len);
            return true;
        }

        std::vector<uint8_t> plain;
        if (msgType == MSG_ENCRYPTED) {
            if (m_securityMode != DataPlaneSecurityMode::Secure ||
                !m_cipher.decrypt(payload, len, &plain) ||
                plain.empty()) {
                failDataChannelFrame(msgType, len, "decrypt-failed", 0);
                return false;
            }
            msgType = plain[0];
            payload = plain.size() > 1 ? plain.data() + 1 : nullptr;
            len = plain.size() > 1 ? plain.size() - 1 : 0;
            validation = validateServerDataPayload(msgType, payload, len);
            if (validation.status == MessageValidationStatus::Malformed) {
                failDataChannelFrame(
                    msgType, len,
                    messageValidationErrorName(validation.error),
                    validation.offset);
                return false;
            }
        } else if (m_securityMode == DataPlaneSecurityMode::Secure) {
            failDataChannelFrame(
                msgType, len, "plaintext-in-secure-mode", 0);
            return false;
        } else if (m_securityMode == DataPlaneSecurityMode::Unconfigured) {
            failDataChannelFrame(msgType, len, "security-unconfigured", 0);
            return false;
        }

        LOG_DBG("[datachannel] Received type=0x%02x len=%zu", msgType, len);
        if (validation.status == MessageValidationStatus::UnknownType)
            return true;

        switch (msgType) {
        case MSG_DATA_CHANNEL_ACK:
            m_established = true;
            m_conn.lastRecvTime = currentTimeMs();
            if (onConnectedCb) onConnectedCb();
            return true;

        case MSG_TCP_RELAY_DATA: {
            ByteBuffer bb(payload, len);
            const uint32_t srcId = bb.readU32();
            bb.readU32();
            const TrafficClass trafficClass =
                static_cast<TrafficClass>(bb.readU8());
            const size_t dataLen = bb.remaining();
            Buffer data(payload + len - dataLen, payload + len);
            if (onRelayData) onRelayData(srcId, trafficClass, data);
            return true;
        }
        case MSG_PONG:
            return true;
        default:
            return true;
        }
    } catch (const std::exception&) {
        failDataChannelFrame(msgType, len, "parser-exception", 0);
        return false;
    } catch (...) {
        failDataChannelFrame(
            msgType, len, "unknown-parser-exception", 0);
        return false;
    }
}

void CliDataChannel::failDataChannelFrame(uint8_t msgType, size_t len,
                                          const char* error, size_t offset) {
    LOG_ERR("[datachannel] Invalid frame type=0x%02x len=%zu error=%s offset=%zu",
            msgType, len, error ? error : "unknown", offset);
    const bool wasEstablished = m_established;
    m_conn.reset();
    m_established = false;
    if (wasEstablished && onDisconnectedCb)
        onDisconnectedCb();
    if (m_peerId != 0)
        scheduleReconnect();
}

void CliDataChannel::sendMsg(uint8_t msgType, const ByteBuffer& body) {
    if (!m_conn.connected ||
        m_securityMode == DataPlaneSecurityMode::Unconfigured)
        return;
    if (m_securityMode == DataPlaneSecurityMode::Secure &&
        msgType != MSG_DATA_CHANNEL_INIT) {
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

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
    : m_myPeerId(0), m_pingSentTime(0), m_connectStartTime(0), m_hasPendingAuth(false)
{
    memset(m_pendingAuthHash, 0, CIPHER_KEY_SIZE);
}

CliSignalClient::~CliSignalClient() { disconnect(); }

bool CliSignalClient::connectTo(const std::string& ip, uint16_t port) {
    m_myPeerId = 0;
    m_connectStartTime = currentTimeMs();
    return m_conn.connectTo(ip, port);
}

void CliSignalClient::disconnect() {
    m_conn.reset();
    m_myPeerId = 0;
}

void CliSignalClient::login(const std::string& name) {
    ByteBuffer bb;
    bb.writeString(name);
    bb.writeU16(PROTOCOL_VERSION);
    m_conn.sendTcpMsg(MSG_LOGIN, bb);
}

void CliSignalClient::createRoom(const std::string& roomName, uint8_t maxPlayers,
                                  TransportMode mode, FecMode fecMode,
                                  uint16_t mtu,
                                  bool encrypted, const uint8_t* passwordHash) {
    ByteBuffer bb;
    bb.writeString(roomName);
    bb.writeU8(maxPlayers);
    bb.writeU8(static_cast<uint8_t>(mode));
    bb.writeU8(static_cast<uint8_t>(fecMode));
    bb.writeU8(encrypted ? 1 : 0);
    if (encrypted && passwordHash)
        bb.writeBytes(passwordHash, 32);
    bb.writeU16(normalizeRoomMtu(mtu));
    m_conn.sendTcpMsg(MSG_CREATE_ROOM, bb);
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
    m_conn.sendTcpMsg(MSG_JOIN_ROOM, bb);
}

void CliSignalClient::leaveRoom() { m_conn.sendTcpMsg(MSG_LEAVE_ROOM); }
void CliSignalClient::listRooms() { m_conn.sendTcpMsg(MSG_LIST_ROOMS); }

void CliSignalClient::reportNatType(NatType type) {
    ByteBuffer bb;
    bb.writeU8(type);
    m_conn.sendTcpMsg(MSG_NAT_REPORT, bb);
}

void CliSignalClient::reportPunchResult(uint32_t targetPeerId, bool success) {
    ByteBuffer bb;
    bb.writeU32(targetPeerId);
    bb.writeU8(success ? 1 : 0);
    m_conn.sendTcpMsg(MSG_PUNCH_RESULT, bb);
}

void CliSignalClient::requestRelay(uint32_t targetPeerId) {
    ByteBuffer bb;
    bb.writeU32(targetPeerId);
    m_conn.sendTcpMsg(MSG_REQUEST_RELAY, bb);
}

void CliSignalClient::sendPing() {
    m_pingSentTime = currentTimeMs();
    m_conn.sendTcpMsg(MSG_PING);
}

void CliSignalClient::sendAuthResponse(const uint8_t* response) {
    ByteBuffer bb;
    bb.writeBytes(response, 32);
    m_conn.sendTcpMsg(MSG_AUTH_RESPONSE, bb);
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
            if (onConnected) onConnected();
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

    switch (msgType) {
    case MSG_LOGIN_RESP: {
        m_myPeerId = bb.readU32();
        uint16_t ver = bb.remaining() >= 2 ? bb.readU16() : 1;
        LOG_INFO("Login OK, peerId=%u serverVersion=%u", m_myPeerId, ver);
        if (ver != PROTOCOL_VERSION)
            LOG_ERR("Protocol version mismatch: client=%u server=%u", PROTOCOL_VERSION, ver);
        if (onLoginResponse) onLoginResponse(m_myPeerId);
        break;
    }
    case MSG_ROOM_CREATED: {
        uint32_t roomId = bb.readU32();
        uint32_t vip    = bb.readU32();
        TransportMode tmode = bb.remaining() > 0 ? static_cast<TransportMode>(bb.readU8()) : MODE_RELAY_KCP;
        FecMode fmode = bb.remaining() > 0 ? static_cast<FecMode>(bb.readU8()) : FEC_NONE;
        bool enc = bb.remaining() > 0 ? (bb.readU8() != 0) : false;
        uint8_t salt[16] = {0}, seed[16] = {0};
        if (enc && bb.remaining() >= 32) {
            bb.readBytes(salt, 16);
            bb.readBytes(seed, 16);
        }
        uint16_t mtu = bb.remaining() >= 2 ? normalizeRoomMtu(bb.readU16()) : ROOM_MTU_DEFAULT;
        if (onRoomCreated) onRoomCreated(roomId, vip, tmode, fmode, mtu, enc, salt, seed);
        break;
    }
    case MSG_JOIN_RESP: {
        uint32_t roomId = bb.readU32();
        uint32_t vip    = bb.readU32();
        TransportMode tmode = static_cast<TransportMode>(bb.readU8());
        FecMode fmode = bb.remaining() > 0 ? static_cast<FecMode>(bb.readU8()) : FEC_NONE;
        bool enc = bb.remaining() > 0 ? (bb.readU8() != 0) : false;
        uint8_t salt[16] = {0}, seed[16] = {0};
        if (enc && bb.remaining() >= 32) {
            bb.readBytes(salt, 16);
            bb.readBytes(seed, 16);
        }
        uint16_t mtu = bb.remaining() >= 2 ? normalizeRoomMtu(bb.readU16()) : ROOM_MTU_DEFAULT;
        uint8_t count = bb.readU8();
        std::vector<PeerInfo> members;
        for (uint8_t i = 0; i < count; ++i) {
            PeerInfo pi;
            pi.peerId     = bb.readU32();
            pi.virtualIP  = bb.readU32();
            pi.name       = bb.readString();
            pi.natType    = static_cast<NatType>(bb.readU8());
            pi.publicIP   = bb.readU32();
            pi.publicPort = bb.readU16();
            pi.transport  = TRANSPORT_NONE;
            members.push_back(pi);
        }
        m_hasPendingAuth = false;
        if (onJoinResponse) onJoinResponse(roomId, vip, tmode, fmode, mtu, enc, salt, seed, members);
        break;
    }
    case MSG_PEER_JOINED: {
        PeerInfo pi;
        pi.peerId     = bb.readU32();
        pi.virtualIP  = bb.readU32();
        pi.name       = bb.readString();
        pi.natType    = static_cast<NatType>(bb.readU8());
        pi.publicIP   = 0;
        pi.publicPort = 0;
        pi.transport  = TRANSPORT_NONE;
        if (onPeerJoined) onPeerJoined(pi);
        break;
    }
    case MSG_PEER_LEFT: {
        uint32_t peerId = bb.readU32();
        if (onPeerLeft) onPeerLeft(peerId);
        break;
    }
    case MSG_ROOM_LIST: {
        uint16_t count = bb.readU16();
        std::vector<CliRoomListItem> rooms;
        for (uint16_t i = 0; i < count; ++i) {
            CliRoomListItem ri;
            ri.roomId        = bb.readU32();
            ri.roomName      = bb.readString();
            ri.playerCount   = bb.readU8();
            ri.maxPlayers    = bb.readU8();
            ri.transportMode = static_cast<TransportMode>(bb.readU8());
            ri.fecMode = bb.remaining() > 0 ? static_cast<FecMode>(bb.readU8()) : FEC_NONE;
            ri.encrypted = bb.remaining() > 0 ? bb.readU8() : 0;
            ri.mtu = bb.remaining() >= 2 ? normalizeRoomMtu(bb.readU16()) : ROOM_MTU_DEFAULT;
            rooms.push_back(ri);
        }
        if (onRoomList) onRoomList(rooms);
        break;
    }
    case MSG_PUNCH_NOTIFY: {
        uint32_t peerId = bb.readU32();
        uint32_t vip    = bb.readU32();
        NatType  nat    = static_cast<NatType>(bb.readU8());
        uint32_t pubIP  = bb.readU32();
        uint16_t pubPort= bb.readU16();
        if (onPunchNotify) onPunchNotify(peerId, vip, nat, pubIP, pubPort);
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
      m_needReconnect(false), m_reconnectTime(0)
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
}

void CliDataChannel::sendRelayData(uint32_t srcPeerId, uint32_t dstPeerId, const Buffer& data) {
    if (!m_established) return;
    ByteBuffer bb;
    bb.writeU32(srcPeerId);
    bb.writeU32(dstPeerId);
    if (!data.empty())
        bb.writeBytes(data.data(), data.size());
    m_conn.sendTcpMsg(MSG_TCP_RELAY_DATA, bb);
}

void CliDataChannel::sendPing() {
    if (!m_established) return;
    m_conn.sendTcpMsg(MSG_PING);
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
            bb.writeU32(m_peerId);
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
    switch (msgType) {
    case MSG_DATA_CHANNEL_ACK:
        m_established = true;
        m_conn.lastRecvTime = currentTimeMs();
        LOG_INFO("Data channel established");
        if (onConnectedCb) onConnectedCb();
        break;
    case MSG_TCP_RELAY_DATA: {
        if (len < 8) break;
        ByteBuffer bb(payload, len);
        uint32_t srcId = bb.readU32();
        bb.readU32();
        size_t remaining = bb.remaining();
        Buffer data(payload + len - remaining, payload + len);
        if (onRelayData) onRelayData(srcId, data);
        break;
    }
    case MSG_PONG:
        break;
    default:
        break;
    }
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

// ======================== CliNatDetector ========================

CliNatDetector::CliNatDetector()
    : m_serverIP(0), m_serverPort(0), m_localPort(0), m_myPeerId(0),
      m_token(0), m_observedIP(0), m_observedPort(0),
      m_done(false), m_retryCount(0), m_lastProbeTime(0), m_result(NAT_UNKNOWN)
{}

void CliNatDetector::detect(socket_t udpFd, uint16_t localPort,
                             uint32_t serverIP, uint16_t serverPort, uint32_t myPeerId) {
    m_serverIP   = serverIP;
    m_serverPort = serverPort;
    m_localPort  = localPort;
    m_myPeerId   = myPeerId;
    m_done       = false;
    m_retryCount = 0;
    std::random_device rd;
    m_token = rd();
    sendProbe(udpFd);
}

void CliNatDetector::sendProbe(socket_t udpFd) {
    StunRequest req;
    memset(&req, 0, sizeof(req));
    req.type      = UDP_STUN_REQUEST;
    req.token     = htonl(m_token);
    req.localPort = htons(m_localPort);
    req.peerId    = htonl(m_myPeerId);

    struct sockaddr_in addr = makeAddr(m_serverIP, m_serverPort);
    sendto(udpFd, reinterpret_cast<const char*>(&req), sizeof(req), 0,
           (struct sockaddr*)&addr, sizeof(addr));
    m_lastProbeTime = currentTimeMs();
    LOG_DBG("[NAT] sendProbe token=%u retry=%d", m_token, m_retryCount);
}

void CliNatDetector::handleStunResponse(const uint8_t* data, size_t len) {
    if (len < sizeof(StunResponse)) return;
    const StunResponse* resp = reinterpret_cast<const StunResponse*>(data);
    if (resp->type != UDP_STUN_RESPONSE) return;
    if (ntohl(resp->token) != m_token) return;
    if (m_done) return;

    m_observedIP   = ntohl(resp->observedIP);
    m_observedPort = ntohs(resp->observedPort);
    m_done = true;

    m_result = (m_observedPort == m_localPort) ? NAT_FULL_CONE : NAT_SYMMETRIC;
    LOG_INFO("NAT detected: %s (observed %s:%u)",
             natTypeName(m_result), ipToString(m_observedIP).c_str(), m_observedPort);
    if (onDetected) onDetected(m_result, m_observedIP, m_observedPort);
}

void CliNatDetector::checkTimeout(socket_t udpFd) {
    if (m_done) return;
    if (currentTimeMs() - m_lastProbeTime < 2000) return;
    if (m_retryCount < 3) {
        ++m_retryCount;
        sendProbe(udpFd);
    } else {
        m_done = true;
        m_result = NAT_UNKNOWN;
        LOG_INFO("NAT detection failed after retries");
        if (onDetected) onDetected(NAT_UNKNOWN, 0, 0);
    }
}

// ======================== CliHolePuncher ========================

CliHolePuncher::CliHolePuncher(uint32_t myPeerId)
    : m_myPeerId(myPeerId)
{
    std::random_device rd;
    m_rng.seed(rd());
}

void CliHolePuncher::startPunch(uint32_t targetPeerId, uint32_t targetIP, uint16_t targetPort) {
    PunchAttempt& a = m_attempts[targetPeerId];
    a.targetPeerId = targetPeerId;
    a.targetIP     = targetIP;
    a.targetPort   = targetPort;
    a.token        = m_rng();
    a.attempts     = 0;
    a.ackReceived  = false;
    a.lastSendTime = 0;
    LOG_INFO("Start punching peer %u at %s:%u", targetPeerId,
             ipToString(targetIP).c_str(), targetPort);
}

void CliHolePuncher::cancelPunch(uint32_t targetPeerId) {
    m_attempts.erase(targetPeerId);
}

void CliHolePuncher::sendPunchPacket(socket_t udpFd, const PunchAttempt& a) {
    PunchPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.type   = UDP_PUNCH;
    pkt.peerId = htonl(m_myPeerId);
    pkt.token  = htonl(a.token);

    struct sockaddr_in addr = makeAddr(a.targetIP, a.targetPort);
    sendto(udpFd, reinterpret_cast<const char*>(&pkt), sizeof(pkt), 0,
           (struct sockaddr*)&addr, sizeof(addr));
}

void CliHolePuncher::sendAck(socket_t udpFd, uint32_t token, uint32_t ip, uint16_t port) {
    PunchPacket ack;
    memset(&ack, 0, sizeof(ack));
    ack.type   = UDP_PUNCH_ACK;
    ack.peerId = htonl(m_myPeerId);
    ack.token  = htonl(token);

    struct sockaddr_in addr = makeAddr(ip, port);
    sendto(udpFd, reinterpret_cast<const char*>(&ack), sizeof(ack), 0,
           (struct sockaddr*)&addr, sizeof(addr));
}

void CliHolePuncher::handleIncomingPacket(const uint8_t* data, size_t len,
                                           uint32_t fromIP, uint16_t fromPort) {
    if (len < sizeof(PunchPacket)) return;
    const PunchPacket* pkt = reinterpret_cast<const PunchPacket*>(data);
    uint32_t remotePeerId = ntohl(pkt->peerId);
    uint32_t token        = ntohl(pkt->token);

    /* Need a UDP fd to send ACK; we'll handle this in the caller via update() */
    (void)token;

    if (pkt->type == UDP_PUNCH) {
        auto it = m_attempts.find(remotePeerId);
        if (it != m_attempts.end() && !it->second.ackReceived) {
            it->second.ackReceived = true;
            LOG_INFO("Punch success (received PUNCH from peer %u)", remotePeerId);
            if (onPunchSucceeded) onPunchSucceeded(remotePeerId, fromIP, fromPort);
            m_attempts.erase(it);
        }
    } else if (pkt->type == UDP_PUNCH_ACK) {
        auto it = m_attempts.find(remotePeerId);
        if (it != m_attempts.end() && !it->second.ackReceived) {
            it->second.ackReceived = true;
            LOG_INFO("Punch success (ACK from peer %u)", remotePeerId);
            if (onPunchSucceeded) onPunchSucceeded(remotePeerId, fromIP, fromPort);
            m_attempts.erase(it);
        }
    }
}

void CliHolePuncher::update(socket_t udpFd) {
    uint32_t now = currentTimeMs();
    std::vector<uint32_t> failed;

    for (auto& kv : m_attempts) {
        PunchAttempt& a = kv.second;
        if (a.ackReceived) continue;
        if (now - a.lastSendTime < static_cast<uint32_t>(PUNCH_RETRY_INTERVAL)) continue;

        a.attempts++;
        int maxAttempts = PUNCH_TIMEOUT_MS / PUNCH_RETRY_INTERVAL;
        if (a.attempts >= maxAttempts) {
            failed.push_back(kv.first);
        } else {
            sendPunchPacket(udpFd, a);
            a.lastSendTime = now;
        }
    }

    for (uint32_t pid : failed) {
        LOG_INFO("Punch timeout for peer %u", pid);
        m_attempts.erase(pid);
        if (onPunchFailed) onPunchFailed(pid);
    }
}

} // namespace VLan

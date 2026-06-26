#include "signal_client.h"
#include "net_common.h"
#include "../ui/log_manager.h"
#include <QTimer>
#include <cstring>

namespace VLan {

SignalClient::SignalClient(QObject* parent)
    : QObject(parent), m_myPeerId(0), m_serverPort(0), m_connectTimeoutMs(8000),
      m_lastRecvTime(0), m_pingSentTime(0)
{
    m_socket = new QTcpSocket(this);
    connect(m_socket, &QTcpSocket::connected,    this, &SignalClient::onConnected);
    connect(m_socket, &QTcpSocket::disconnected, this, &SignalClient::onDisconnected);
    connect(m_socket, &QTcpSocket::readyRead,    this, &SignalClient::onReadyRead);
    connect(m_socket, SIGNAL(error(QAbstractSocket::SocketError)),
            this, SLOT(onSocketError(QAbstractSocket::SocketError)));

    m_pingTimer = new QTimer(this);
    connect(m_pingTimer, &QTimer::timeout, this, &SignalClient::onPingTimer);

    m_connectTimer = new QTimer(this);
    m_connectTimer->setSingleShot(true);
    connect(m_connectTimer, &QTimer::timeout, this, &SignalClient::onConnectTimeout);

    m_recvTimeoutTimer = new QTimer(this);
    connect(m_recvTimeoutTimer, &QTimer::timeout, this, &SignalClient::onRecvTimeoutCheck);
}

SignalClient::~SignalClient() {
    disconnect();
}

void SignalClient::connectToServer(const QString& host, quint16 port,
                                   int timeoutMs) {
    if (m_socket->state() != QAbstractSocket::UnconnectedState) {
        m_socket->abort();
    }
    m_serverHost       = host;
    m_serverPort       = port;
    m_connectTimeoutMs = timeoutMs;
    m_myPeerId         = 0;
    m_recvBuf.clear();
    m_socket->connectToHost(host, port);
    m_connectTimer->start(timeoutMs);
}

void SignalClient::disconnect() {
    m_connectTimer->stop();
    m_pingTimer->stop();
    m_recvTimeoutTimer->stop();
    if (m_socket->state() != QAbstractSocket::UnconnectedState)
        m_socket->abort();
}

bool SignalClient::isConnected() const {
    return m_socket->state() == QAbstractSocket::ConnectedState;
}

bool SignalClient::isConnecting() const {
    return m_socket->state() == QAbstractSocket::ConnectingState ||
           m_socket->state() == QAbstractSocket::HostLookupState;
}

// ───────── Outgoing messages ─────────

void SignalClient::login(const QString& name) {
    ByteBuffer bb;
    bb.writeString(name.toStdString());
    bb.writeU16(PROTOCOL_VERSION);
    sendMsg(MSG_LOGIN, bb);
}

void SignalClient::createRoom(const QString& roomName, uint8_t maxPlayers,
                              TransportMode mode, FecMode fecMode,
                              uint16_t mtu,
                              bool encrypted, const QByteArray& passwordHash) {
    ByteBuffer bb;
    bb.writeString(roomName.toStdString());
    bb.writeU8(maxPlayers);
    bb.writeU8(static_cast<uint8_t>(mode));
    bb.writeU8(static_cast<uint8_t>(fecMode));
    bb.writeU8(encrypted ? 1 : 0);
    if (encrypted && passwordHash.size() == 32) {
        bb.writeBytes(passwordHash.constData(), 32);
    }
    bb.writeU16(normalizeRoomMtu(mtu));
    sendMsg(MSG_CREATE_ROOM, bb);
}

void SignalClient::joinRoom(uint32_t roomId, const QByteArray& authHash) {
    m_pendingAuthHash = authHash;
    ByteBuffer bb;
    bb.writeU32(roomId);
    sendMsg(MSG_JOIN_ROOM, bb);
}

void SignalClient::leaveRoom() {
    sendMsg(MSG_LEAVE_ROOM);
}

void SignalClient::listRooms() {
    sendMsg(MSG_LIST_ROOMS);
}

void SignalClient::reportNatType(NatType type) {
    ByteBuffer bb;
    bb.writeU8(type);
    sendMsg(MSG_NAT_REPORT, bb);
}

void SignalClient::reportPunchResult(uint32_t targetPeerId, bool success) {
    ByteBuffer bb;
    bb.writeU32(targetPeerId);
    bb.writeU8(success ? 1 : 0);
    sendMsg(MSG_PUNCH_RESULT, bb);
}

void SignalClient::requestRelay(uint32_t targetPeerId) {
    ByteBuffer bb;
    bb.writeU32(targetPeerId);
    sendMsg(MSG_REQUEST_RELAY, bb);
}

// ───────── Socket events ─────────

void SignalClient::onConnected() {
    m_connectTimer->stop();
    m_socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    m_lastRecvTime = currentTimeMs();
    m_pingTimer->start(KEEPALIVE_INTERVAL_MS);
    m_recvTimeoutTimer->start(5000);
    emit connected();
}

void SignalClient::onDisconnected() {
    m_connectTimer->stop();
    m_pingTimer->stop();
    m_recvTimeoutTimer->stop();
    emit disconnected();
}

void SignalClient::onSocketError(QAbstractSocket::SocketError err) {
    Q_UNUSED(err);
    m_connectTimer->stop();
    m_pingTimer->stop();
    m_recvTimeoutTimer->stop();
    QString reason = m_socket->errorString();
    if (m_socket->state() != QAbstractSocket::ConnectedState) {
        m_socket->abort();
        emit connectFailed(reason);
    }
}

void SignalClient::onConnectTimeout() {
    if (m_socket->state() != QAbstractSocket::ConnectedState) {
        m_socket->abort();
        emit connectFailed(QString::fromUtf8("连接超时 (%1秒)")
                           .arg(m_connectTimeoutMs / 1000));
    }
}

void SignalClient::onReadyRead() {
    m_recvBuf.append(m_socket->readAll());
    m_lastRecvTime = currentTimeMs();

    static const int MAX_RECV_BUF = 1024 * 1024;
    if (m_recvBuf.size() > MAX_RECV_BUF) {
        handleStreamCorruption();
        return;
    }

    while (m_recvBuf.size() >= static_cast<int>(sizeof(TcpMsgHeader))) {
        const TcpMsgHeader* hdr =
            reinterpret_cast<const TcpMsgHeader*>(m_recvBuf.constData());
        uint16_t payloadLen = ntohs(hdr->length);

        if (payloadLen > MAX_TCP_MSG_PAYLOAD) {
            handleStreamCorruption();
            return;
        }

        int frameLen = static_cast<int>(sizeof(TcpMsgHeader)) + payloadLen;
        if (m_recvBuf.size() < frameLen) break;

        processMessage(hdr->msgType,
                       reinterpret_cast<const uint8_t*>(m_recvBuf.constData()) + sizeof(TcpMsgHeader),
                       payloadLen);
        m_recvBuf.remove(0, frameLen);
    }
}

void SignalClient::handleStreamCorruption() {
    LogManager::instance().logError(QString("[signal] TCP stream corrupted, disconnecting"));
    m_recvBuf.clear();
    m_socket->abort();
}

void SignalClient::onRecvTimeoutCheck() {
    if (!isConnected()) return;
    uint32_t now = currentTimeMs();
    uint32_t elapsed = now - m_lastRecvTime;
    if (elapsed > static_cast<uint32_t>(TCP_RECV_TIMEOUT_MS)) {
        LogManager::instance().logError(QString("[signal] No data received for %1 ms, connection dead").arg(elapsed));
        m_recvBuf.clear();
        m_socket->abort();
    }
}

void SignalClient::onPingTimer() {
    m_pingSentTime = currentTimeMs();
    sendMsg(MSG_PING);
}

// ───────── Send helper ─────────

void SignalClient::sendMsg(uint8_t msgType, const ByteBuffer& body) {
    if (m_socket->state() != QAbstractSocket::ConnectedState) return;
    TcpMsgHeader hdr;
    hdr.msgType = msgType;
    hdr.length  = htons(static_cast<uint16_t>(body.size()));
    m_socket->write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    if (body.size() > 0)
        m_socket->write(reinterpret_cast<const char*>(body.data()), body.size());
}

void SignalClient::sendMsg(uint8_t msgType) {
    if (m_socket->state() != QAbstractSocket::ConnectedState) return;
    TcpMsgHeader hdr;
    hdr.msgType = msgType;
    hdr.length  = 0;
    m_socket->write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
}

// ───────── Incoming message dispatch ─────────

void SignalClient::processMessage(uint8_t msgType,
                                  const uint8_t* payload, size_t len)
{
    ByteBuffer bb(payload, len);

    if (g_verboseLog)
        LogManager::instance().logDetail(QString("[signal] processMessage type=0x%1 len=%2").arg(msgType, 0, 16).arg(len));

    switch (msgType) {
    case MSG_LOGIN_RESP: {
        m_myPeerId = bb.readU32();
        uint16_t serverVersion = bb.remaining() >= 2 ? bb.readU16() : 1;
        LogManager::instance().logDetail(QString("[signal] LOGIN_RESP peerId=%1 serverVersion=%2").arg(m_myPeerId).arg(serverVersion));
        if (serverVersion != PROTOCOL_VERSION) {
            LogManager::instance().logError(QString("[signal] Protocol version mismatch: client=%1 server=%2").arg(PROTOCOL_VERSION).arg(serverVersion));
        }
        emit loginResponse(m_myPeerId);
        break;
    }
    case MSG_ROOM_CREATED: {
        uint32_t roomId = bb.readU32();
        uint32_t vip    = bb.readU32();
        TransportMode tmode = bb.remaining() > 0
            ? static_cast<TransportMode>(bb.readU8()) : MODE_RELAY_KCP;
        FecMode fmode = bb.remaining() > 0
            ? static_cast<FecMode>(bb.readU8()) : FEC_NONE;
        bool enc = bb.remaining() > 0 ? (bb.readU8() != 0) : false;
        QByteArray salt;
        QByteArray sessionSeed;
        if (enc && bb.remaining() >= 32) {
            salt.resize(16);
            bb.readBytes(salt.data(), 16);
            sessionSeed.resize(16);
            bb.readBytes(sessionSeed.data(), 16);
        }
        uint16_t mtu = bb.remaining() >= 2 ? normalizeRoomMtu(bb.readU16()) : ROOM_MTU_DEFAULT;
        LogManager::instance().logDetail(QString("[signal] ROOM_CREATED roomId=%1 vip=%2 mode=%3 fec=%4 mtu=%5 encrypted=%6").arg(roomId).arg(virtualIPToString(vip)).arg(tmode).arg(fmode).arg(mtu).arg(enc));
        emit roomCreated(roomId, vip, tmode, fmode, mtu, enc, salt, sessionSeed);
        break;
    }
    case MSG_JOIN_RESP: {
        uint32_t roomId = bb.readU32();
        uint32_t vip    = bb.readU32();
        TransportMode tmode = static_cast<TransportMode>(bb.readU8());
        FecMode fmode = bb.remaining() > 0
            ? static_cast<FecMode>(bb.readU8()) : FEC_NONE;
        bool enc = bb.remaining() > 0 ? (bb.readU8() != 0) : false;
        QByteArray salt;
        QByteArray sessionSeed;
        if (enc && bb.remaining() >= 32) {
            salt.resize(16);
            bb.readBytes(salt.data(), 16);
            sessionSeed.resize(16);
            bb.readBytes(sessionSeed.data(), 16);
        }
        uint16_t mtu = bb.remaining() >= 2 ? normalizeRoomMtu(bb.readU16()) : ROOM_MTU_DEFAULT;
        uint8_t  count  = bb.readU8();
        QList<PeerInfo> members;
        for (uint8_t i = 0; i < count; ++i) {
            PeerInfo pi;
            pi.peerId     = bb.readU32();
            pi.virtualIP  = bb.readU32();
            pi.name       = bb.readString();
            pi.natType    = static_cast<NatType>(bb.readU8());
            pi.publicIP   = bb.readU32();
            pi.publicPort = bb.readU16();
            pi.transport  = TRANSPORT_NONE;
            members.append(pi);
        }
        LogManager::instance().logDetail(QString("[signal] JOIN_RESP roomId=%1 vip=%2 mode=%3 fec=%4 mtu=%5 encrypted=%6 members=%7").arg(roomId).arg(virtualIPToString(vip)).arg(tmode).arg(fmode).arg(mtu).arg(enc).arg(count));
        m_pendingAuthHash.clear();
        emit joinResponse(roomId, vip, tmode, fmode, mtu, enc, salt, sessionSeed, members);
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
        LogManager::instance().logDetail(QString("[signal] PEER_JOINED peerId=%1 vip=%2 name=%3 nat=%4").arg(pi.peerId).arg(virtualIPToString(pi.virtualIP)).arg(QString::fromStdString(pi.name)).arg(natTypeName(pi.natType)));
        emit peerJoined(pi);
        break;
    }
    case MSG_PEER_LEFT: {
        uint32_t peerId = bb.readU32();
        LogManager::instance().logDetail(QString("[signal] PEER_LEFT peerId=%1").arg(peerId));
        emit peerLeft(peerId);
        break;
    }
    case MSG_ROOM_LIST: {
        uint16_t count = bb.readU16();
        QList<RoomListItem> rooms;
        for (uint16_t i = 0; i < count; ++i) {
            RoomListItem ri;
            ri.roomId      = bb.readU32();
            std::string nm = bb.readString();
            strncpy(ri.roomName, nm.c_str(), MAX_ROOM_NAME_LEN);
            ri.roomName[MAX_ROOM_NAME_LEN] = '\0';
            ri.playerCount = bb.readU8();
            ri.maxPlayers  = bb.readU8();
            ri.transportMode = static_cast<TransportMode>(bb.readU8());
            ri.fecMode = bb.remaining() > 0
                ? static_cast<FecMode>(bb.readU8()) : FEC_NONE;
            ri.encrypted = bb.remaining() > 0 ? bb.readU8() : 0;
            ri.mtu = bb.remaining() >= 2 ? normalizeRoomMtu(bb.readU16()) : ROOM_MTU_DEFAULT;
            rooms.append(ri);
        }
        LogManager::instance().logDetail(QString("[signal] ROOM_LIST count=%1").arg(count));
        emit roomList(rooms);
        break;
    }
    case MSG_PUNCH_NOTIFY: {
        uint32_t peerId = bb.readU32();
        uint32_t vip    = bb.readU32();
        NatType  nat    = static_cast<NatType>(bb.readU8());
        uint32_t pubIP  = bb.readU32();
        uint16_t pubPort= bb.readU16();
        LogManager::instance().logDetail(QString("[signal] PUNCH_NOTIFY peerId=%1 vip=%2 nat=%3 pubIP=%4 pubPort=%5").arg(peerId).arg(virtualIPToString(vip)).arg(natTypeName(nat)).arg(QHostAddress(pubIP).toString()).arg(pubPort));
        emit punchNotify(peerId, vip, nat, pubIP, pubPort);
        break;
    }
    case MSG_RELAY_READY: {
        uint32_t peerId = bb.readU32();
        LogManager::instance().logDetail(QString("[signal] RELAY_READY peerId=%1").arg(peerId));
        emit relayReady(peerId);
        break;
    }
    case MSG_PONG: {
        if (m_pingSentTime != 0) {
            int rtt = static_cast<int>(currentTimeMs() - m_pingSentTime);
            m_pingSentTime = 0;
            LogManager::instance().logDetail(QString("[signal] PONG rtt=%1 ms").arg(rtt));
            emit serverRttUpdated(rtt);
        }
        break;
    }
    case MSG_AUTH_CHALLENGE: {
        if (m_pendingAuthHash.size() == 32 && bb.remaining() >= 32) {
            QByteArray challenge(32, '\0');
            bb.readBytes(challenge.data(), 32);
            QByteArray response = PayloadCipher::challengeResponse(
                m_pendingAuthHash, challenge);
            ByteBuffer respBuf;
            respBuf.writeBytes(response.constData(), 32);
            sendMsg(MSG_AUTH_RESPONSE, respBuf);
            LogManager::instance().logDetail(QString("[signal] AUTH_CHALLENGE received, response sent"));
        } else {
            LogManager::instance().logError(QString("[signal] AUTH_CHALLENGE but no pending auth hash"));
            m_pendingAuthHash.clear();
        }
        break;
    }
    case MSG_ERROR: {
        std::string errMsg = bb.readString();
        LogManager::instance().logDetail(QString("[signal] ERROR: %1").arg(QString::fromStdString(errMsg)));
        m_pendingAuthHash.clear();
        emit serverError(QString::fromStdString(errMsg));
        break;
    }
    default:
        LogManager::instance().logDetail(QString("[signal] Unknown message type: 0x%1").arg(msgType, 0, 16));
        break;
    }
}

} // namespace VLan

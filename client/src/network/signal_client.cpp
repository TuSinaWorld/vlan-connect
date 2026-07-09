#include "signal_client.h"
#include "net_common.h"
#include "../ui/log_manager.h"
#include "../ui/ui_strings.h"
#include <QTimer>
#include <cstring>

namespace VLan {

SignalClient::SignalClient(QObject* parent)
    : QObject(parent), m_myPeerId(0), m_serverPort(0), m_connectTimeoutMs(8000),
      m_lastRecvTime(0), m_pingSentTime(0),
      m_serverAuthRequired(false), m_secureReady(false), m_secureSessionId(0)
{
    memset(m_clientNonce, 0, sizeof(m_clientNonce));
    memset(m_serverNonce, 0, sizeof(m_serverNonce));
    memset(m_clientPrivKey, 0, sizeof(m_clientPrivKey));
    memset(m_clientPubKey, 0, sizeof(m_clientPubKey));
    memset(m_serverPubKey, 0, sizeof(m_serverPubKey));
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
    m_serverAuthRequired = false;
    m_secureReady = false;
    m_secureSessionId = 0;
    m_secureMaster.clear();
    m_recvBuf.clear();
    m_socket->connectToHost(host, port);
    m_connectTimer->start(timeoutMs);
}

void SignalClient::setServerPassword(const QString& password) {
    m_serverPassword = password;
}

void SignalClient::continueServerAuth() {
    if (m_serverAuthRequired && !m_secureReady)
        sendServerAuth();
}

void SignalClient::disconnect() {
    m_connectTimer->stop();
    m_pingTimer->stop();
    m_recvTimeoutTimer->stop();
    if (m_socket->state() != QAbstractSocket::UnconnectedState)
        m_socket->abort();
    m_myPeerId = 0;
}

bool SignalClient::isConnected() const {
    return m_socket->state() == QAbstractSocket::ConnectedState;
}

bool SignalClient::isConnecting() const {
    return m_socket->state() == QAbstractSocket::ConnectingState ||
           m_socket->state() == QAbstractSocket::HostLookupState;
}

// Outgoing messages

void SignalClient::login(const QString& name,
                         bool hasResume,
                         uint32_t resumeRoomId,
                         uint32_t resumePeerId,
                         const QByteArray& resumeToken) {
    ByteBuffer bb;
    QByteArray nameBytes = name.toUtf8();
    bb.writeString(std::string(nameBytes.constData(), nameBytes.size()));
    bb.writeU16(PROTOCOL_VERSION);
    bool sendResume = hasResume && resumeToken.size() == RECONNECT_TOKEN_SIZE;
    bb.writeU8(sendResume ? 1 : 0);
    if (sendResume) {
        bb.writeU32(resumeRoomId);
        bb.writeU32(resumePeerId);
        bb.writeBytes(resumeToken.constData(), RECONNECT_TOKEN_SIZE);
    }
    sendMsg(MSG_LOGIN, bb);
}

void SignalClient::createRoom(const QString& roomName, uint8_t maxPlayers,
                              RoomTrafficPolicy tcpPolicy,
                              RoomTrafficPolicy udpPolicy,
                              uint16_t mtu,
                              bool passwordProtected, const QByteArray& passwordHash) {
    ByteBuffer bb;
    bb.writeString(roomName.toStdString());
    bb.writeU8(maxPlayers);
    bb.writeU8(static_cast<uint8_t>(tcpPolicy.transportMode));
    bb.writeU8(static_cast<uint8_t>(tcpPolicy.fecMode));
    bb.writeU8(static_cast<uint8_t>(tcpPolicy.kcpProfile));
    bb.writeU8(static_cast<uint8_t>(udpPolicy.transportMode));
    bb.writeU8(static_cast<uint8_t>(udpPolicy.fecMode));
    bb.writeU8(static_cast<uint8_t>(udpPolicy.kcpProfile));
    bb.writeU8(passwordProtected ? 1 : 0);
    if (passwordProtected && passwordHash.size() == 32) {
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

void SignalClient::resumeRoom(uint32_t roomId, uint32_t peerId,
                              const QByteArray& resumeToken) {
    if (resumeToken.size() != RECONNECT_TOKEN_SIZE) return;
    ByteBuffer bb;
    bb.writeU32(roomId);
    bb.writeU32(peerId);
    bb.writeBytes(resumeToken.constData(), RECONNECT_TOKEN_SIZE);
    sendMsg(MSG_RESUME_ROOM, bb);
}

void SignalClient::leaveRoom() {
    sendMsg(MSG_LEAVE_ROOM);
}

void SignalClient::logout() {
    sendMsg(MSG_LOGOUT);
}

void SignalClient::listRooms() {
    sendMsg(MSG_LIST_ROOMS);
}

void SignalClient::requestRelay(uint32_t targetPeerId) {
    ByteBuffer bb;
    bb.writeU32(targetPeerId);
    sendMsg(MSG_REQUEST_RELAY, bb);
}

// Socket events

void SignalClient::onConnected() {
    m_connectTimer->stop();
    m_socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    m_lastRecvTime = currentTimeMs();
    m_recvTimeoutTimer->start(5000);
    sendClientHello();
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
        emit connectFailed(UiStrings::text("error.connectTimeout")
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
        LogManager::instance().logError(QString("[signal] %1").arg(UiStrings::text("error.streamCorrupt")));
    m_recvBuf.clear();
    m_socket->abort();
}

void SignalClient::onRecvTimeoutCheck() {
    if (!isConnected()) return;
    uint32_t now = currentTimeMs();
    uint32_t elapsed = now - m_lastRecvTime;
    if (elapsed > static_cast<uint32_t>(TCP_RECV_TIMEOUT_MS)) {
        LogManager::instance().logError(QString("[signal] %1")
            .arg(UiStrings::text("error.connectionDead").arg(elapsed)));
        m_recvBuf.clear();
        m_socket->abort();
    }
}

void SignalClient::onPingTimer() {
    m_pingSentTime = currentTimeMs();
    sendMsg(MSG_PING);
}

// Send helper

void SignalClient::sendClientHello() {
    secureRandomBytes(m_clientNonce, 16);
    secureRandomBytes(m_clientPrivKey, 32);
    crypto_x25519_public_key(m_clientPubKey, m_clientPrivKey);

    ByteBuffer bb;
    bb.writeU16(PROTOCOL_VERSION);
    bb.writeBytes(m_clientNonce, 16);
    bb.writeBytes(m_clientPubKey, 32);

    TcpMsgHeader hdr;
    hdr.msgType = MSG_CLIENT_HELLO;
    hdr.length  = htons(static_cast<uint16_t>(bb.size()));
    m_socket->write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    m_socket->write(reinterpret_cast<const char*>(bb.data()), bb.size());
}

void SignalClient::sendServerAuth() {
    if (m_serverPassword.isEmpty()) {
        emit serverPasswordRequired();
        return;
    }

    QByteArray inter = PayloadCipher::computeIntermediate(m_serverPassword);
    QByteArray authHash = PayloadCipher::hashFromIntermediate(inter);
    uint8_t shared[32];
    uint8_t master[32];
    crypto_x25519(shared, m_clientPrivKey, m_serverPubKey);
    deriveSecureMaster(master, shared,
                       reinterpret_cast<const uint8_t*>(authHash.constData()),
                       m_clientNonce, m_serverNonce,
                       m_clientPubKey, m_serverPubKey);
    uint8_t proof[32];
    computeClientAuthProof(proof, master,
                           reinterpret_cast<const uint8_t*>(authHash.constData()));

    m_secureMaster = QByteArray(reinterpret_cast<const char*>(master), 32);

    ByteBuffer body;
    body.writeBytes(proof, 32);
    TcpMsgHeader hdr;
    hdr.msgType = MSG_SERVER_AUTH;
    hdr.length  = htons(static_cast<uint16_t>(body.size()));
    m_socket->write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    m_socket->write(reinterpret_cast<const char*>(body.data()), body.size());

    crypto_wipe(reinterpret_cast<uint8_t*>(inter.data()), inter.size());
    crypto_wipe(reinterpret_cast<uint8_t*>(authHash.data()), authHash.size());
    crypto_wipe(shared, sizeof(shared));
    crypto_wipe(master, sizeof(master));
    crypto_wipe(proof, sizeof(proof));
}

void SignalClient::sendMsg(uint8_t msgType, const ByteBuffer& body) {
    if (m_socket->state() != QAbstractSocket::ConnectedState) return;

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
        TcpMsgHeader hdr;
        hdr.msgType = MSG_ENCRYPTED;
        hdr.length  = htons(static_cast<uint16_t>(enc.size()));
        m_socket->write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
        if (!enc.empty())
            m_socket->write(reinterpret_cast<const char*>(enc.data()), enc.size());
        return;
    }

    TcpMsgHeader hdr;
    hdr.msgType = msgType;
    hdr.length  = htons(static_cast<uint16_t>(body.size()));
    m_socket->write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    if (body.size() > 0)
        m_socket->write(reinterpret_cast<const char*>(body.data()), body.size());
}

void SignalClient::sendMsg(uint8_t msgType) {
    if (m_socket->state() != QAbstractSocket::ConnectedState) return;
    ByteBuffer empty;
    sendMsg(msgType, empty);
}

// Incoming message dispatch

void SignalClient::processMessage(uint8_t msgType,
                                  const uint8_t* payload, size_t len)
{
    ByteBuffer bb(payload, len);

    if (g_verboseLog)
        LogManager::instance().logDetail(QString("[signal] processMessage type=0x%1 len=%2").arg(msgType, 0, 16).arg(len));

    if (msgType == MSG_SERVER_HELLO) {
        uint16_t serverVersion = bb.readU16();
        bool authRequired = bb.remaining() > 0 ? (bb.readU8() != 0) : false;
        LogManager::instance().logDetail(QString("[signal] SERVER_HELLO version=%1 authRequired=%2 len=%3")
                                         .arg(serverVersion).arg(authRequired).arg(len));
        if (serverVersion != PROTOCOL_VERSION) {
            emit serverError(UiStrings::text("error.protocolMismatch")
                             .arg(PROTOCOL_VERSION).arg(serverVersion));
            m_socket->abort();
            return;
        }
        m_serverAuthRequired = authRequired;
        if (!authRequired) {
            m_pingTimer->start(KEEPALIVE_INTERVAL_MS);
            emit connected();
            return;
        }
        if (bb.remaining() < 48) {
            emit serverError(UiStrings::text("error.invalidServerHello"));
            m_socket->abort();
            return;
        }
        bb.readBytes(m_serverNonce, 16);
        bb.readBytes(m_serverPubKey, 32);
        if (m_serverPassword.isEmpty()) {
            LogManager::instance().logDetail(QStringLiteral("[signal] Server auth password required"));
            emit serverPasswordRequired();
        } else {
            LogManager::instance().logDetail(QStringLiteral("[signal] Sending server auth proof"));
            sendServerAuth();
        }
        return;
    }

    if (msgType == MSG_SERVER_AUTH_OK) {
        LogManager::instance().logDetail(QString("[signal] SERVER_AUTH_OK len=%1").arg(len));
        if (m_secureMaster.size() != 32 || bb.remaining() < 36) {
            emit serverError(UiStrings::text("error.invalidAuthResponse"));
            m_socket->abort();
            return;
        }
        m_secureSessionId = bb.readU32();
        QByteArray serverProof(32, '\0');
        bb.readBytes(serverProof.data(), 32);

        QByteArray inter = PayloadCipher::computeIntermediate(m_serverPassword);
        QByteArray authHash = PayloadCipher::hashFromIntermediate(inter);
        uint8_t expected[32];
        computeServerAuthProof(expected,
                               reinterpret_cast<const uint8_t*>(m_secureMaster.constData()),
                               reinterpret_cast<const uint8_t*>(authHash.constData()));
        if (crypto_verify32(expected, reinterpret_cast<const uint8_t*>(serverProof.constData())) != 0) {
            emit serverError(UiStrings::text("error.serverProofFailed"));
            m_socket->abort();
            crypto_wipe(expected, sizeof(expected));
            crypto_wipe(reinterpret_cast<uint8_t*>(inter.data()), inter.size());
            crypto_wipe(reinterpret_cast<uint8_t*>(authHash.data()), authHash.size());
            return;
        }
        m_secureCipher.init(reinterpret_cast<const uint8_t*>(m_secureMaster.constData()), true, "signal");
        m_secureReady = true;
        LogManager::instance().logDetail(QString("[signal] Secure session established session=%1")
                                         .arg(m_secureSessionId));
        m_pingTimer->start(KEEPALIVE_INTERVAL_MS);
        emit secureSessionEstablished(m_secureSessionId, m_secureMaster);
        emit connected();
        crypto_wipe(expected, sizeof(expected));
        crypto_wipe(reinterpret_cast<uint8_t*>(inter.data()), inter.size());
        crypto_wipe(reinterpret_cast<uint8_t*>(authHash.data()), authHash.size());
        return;
    }

    if (msgType == MSG_ENCRYPTED) {
        if (!m_secureReady) {
            LogManager::instance().logError(QStringLiteral("[signal] Encrypted frame before secure session"));
            handleStreamCorruption();
            return;
        }
        std::vector<uint8_t> plain;
        if (!m_secureCipher.decrypt(payload, len, &plain) || plain.empty()) {
            LogManager::instance().logError(QString("[signal] Cannot decrypt frame len=%1").arg(len));
            handleStreamCorruption();
            return;
        }
        msgType = plain[0];
        payload = plain.size() > 1 ? plain.data() + 1 : nullptr;
        len = plain.size() > 1 ? plain.size() - 1 : 0;
        LogManager::instance().logDetail(QString("[signal] Decrypted frame type=0x%1 bodyLen=%2")
                                         .arg(msgType, 2, 16, QLatin1Char('0')).arg(len));
        bb = len > 0 ? ByteBuffer(payload, len) : ByteBuffer();
    } else if (m_secureReady) {
        LogManager::instance().logError(QString("[signal] Plaintext frame 0x%1 received in secure mode")
                                        .arg(msgType, 2, 16, QLatin1Char('0')));
        handleStreamCorruption();
        return;
    }

    switch (msgType) {
    case MSG_LOGIN_RESP: {
        m_myPeerId = bb.readU32();
        uint16_t serverVersion = bb.remaining() >= 2 ? bb.readU16() : 1;
        bool resumeAccepted = bb.remaining() > 0 ? (bb.readU8() != 0) : false;
        LogManager::instance().logDetail(QString("[signal] LOGIN_RESP peerId=%1 serverVersion=%2 resume=%3").arg(m_myPeerId).arg(serverVersion).arg(resumeAccepted));
        if (serverVersion != PROTOCOL_VERSION) {
            LogManager::instance().logError(QString("[signal] Protocol version mismatch: client=%1 server=%2").arg(PROTOCOL_VERSION).arg(serverVersion));
        }
        emit loginResponse(m_myPeerId, resumeAccepted);
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
        QByteArray leaseToken;
        if (bb.remaining() >= RECONNECT_TOKEN_SIZE) {
            leaseToken.resize(RECONNECT_TOKEN_SIZE);
            bb.readBytes(leaseToken.data(), RECONNECT_TOKEN_SIZE);
        }
        LogManager::instance().logDetail(QString("[signal] ROOM_CREATED roomId=%1 vip=%2 tcp=%3/%4 udp=%5/%6 mtu=%7 password=%8")
            .arg(roomId).arg(virtualIPToString(vip))
            .arg(tcpPolicy.transportMode).arg(tcpPolicy.fecMode)
            .arg(udpPolicy.transportMode).arg(udpPolicy.fecMode)
            .arg(mtu).arg(passwordProtected));
        emit roomCreated(roomId, vip, tcpPolicy, udpPolicy, mtu, passwordProtected, leaseToken);
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
        uint8_t  count  = bb.readU8();
        QList<PeerInfo> members;
        for (uint8_t i = 0; i < count; ++i) {
            PeerInfo pi;
            pi.peerId     = bb.readU32();
            pi.virtualIP  = bb.readU32();
            pi.name       = bb.readString();
            pi.transport  = TRANSPORT_NONE;
            members.append(pi);
        }
        QByteArray leaseToken;
        if (bb.remaining() >= RECONNECT_TOKEN_SIZE) {
            leaseToken.resize(RECONNECT_TOKEN_SIZE);
            bb.readBytes(leaseToken.data(), RECONNECT_TOKEN_SIZE);
        }
        LogManager::instance().logDetail(QString("[signal] JOIN_RESP roomId=%1 vip=%2 tcp=%3/%4 udp=%5/%6 mtu=%7 password=%8 members=%9")
            .arg(roomId).arg(virtualIPToString(vip))
            .arg(tcpPolicy.transportMode).arg(tcpPolicy.fecMode)
            .arg(udpPolicy.transportMode).arg(udpPolicy.fecMode)
            .arg(mtu).arg(passwordProtected).arg(count));
        m_pendingAuthHash.clear();
        emit joinResponse(roomId, vip, tcpPolicy, udpPolicy, mtu, passwordProtected, members, leaseToken);
        break;
    }
    case MSG_PEER_JOINED: {
        PeerInfo pi;
        pi.peerId     = bb.readU32();
        pi.virtualIP  = bb.readU32();
        pi.name       = bb.readString();
        pi.transport  = TRANSPORT_NONE;
        LogManager::instance().logDetail(QString("[signal] PEER_JOINED peerId=%1 vip=%2 name=%3").arg(pi.peerId).arg(virtualIPToString(pi.virtualIP)).arg(QString::fromStdString(pi.name)));
        emit peerJoined(pi);
        break;
    }
    case MSG_PEER_RESUMED: {
        PeerInfo pi;
        pi.peerId     = bb.readU32();
        pi.virtualIP  = bb.readU32();
        pi.name       = bb.readString();
        pi.transport  = TRANSPORT_NONE;
        LogManager::instance().logDetail(QString("[signal] PEER_RESUMED peerId=%1 vip=%2 name=%3").arg(pi.peerId).arg(virtualIPToString(pi.virtualIP)).arg(QString::fromStdString(pi.name)));
        emit peerResumed(pi);
        break;
    }
    case MSG_PEER_LEFT: {
        uint32_t peerId = bb.readU32();
        LogManager::instance().logDetail(QString("[signal] PEER_LEFT peerId=%1").arg(peerId));
        emit peerLeft(peerId);
        break;
    }
    case MSG_LOGOUT_ACK: {
        LogManager::instance().logDetail(QStringLiteral("[signal] LOGOUT_ACK"));
        m_myPeerId = 0;
        emit logoutAck();
        break;
    }
    case MSG_ROOM_LIST:
    case MSG_ROOM_LIST_PUSH: {
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
            ri.tcpPolicy = normalizeTrafficPolicy(
                bb.readU8(), bb.readU8(), bb.readU8(), makeDefaultTcpPolicy());
            ri.udpPolicy = normalizeTrafficPolicy(
                bb.readU8(), bb.readU8(), bb.readU8(), makeDefaultUdpPolicy());
            ri.passwordProtected = bb.remaining() > 0 ? bb.readU8() : 0;
            ri.mtu = bb.remaining() >= 2 ? normalizeRoomMtu(bb.readU16()) : ROOM_MTU_DEFAULT;
            rooms.append(ri);
        }
        LogManager::instance().logDetail(QString("[signal] %1 count=%2")
            .arg(msgType == MSG_ROOM_LIST_PUSH ? "ROOM_LIST_PUSH" : "ROOM_LIST")
            .arg(count));
        emit roomList(rooms);
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

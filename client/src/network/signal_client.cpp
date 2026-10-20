#include "signal_client.h"
#include "net_common.h"
#include "signal_message_validator.h"
#include "tcp_frame_probe.h"
#include "traffic_policy_codec.h"
#include "../ui/log_manager.h"
#include "../ui/ui_strings.h"
#include <QPointer>
#include <QTimer>
#include <cstring>

namespace VLan {

SignalClient::SignalClient(QObject* parent)
    : QObject(parent), m_socket(nullptr), m_myPeerId(0), m_serverPort(0), m_connectTimeoutMs(8000),
      m_lastRecvTime(0), m_connectStartedMs(0), m_authStartedMs(0), m_pingSentTime(0), m_joinPending(false), m_roomOperationPending(false), m_authOperation(0),
      m_pendingCreateMtu(ROOM_MTU_DEFAULT),
      m_serverAuthRequired(false), m_secureReady(false),
      m_fatalDisconnectPending(false), m_secureSessionId(0),
      m_roomListRevision(0), m_snapshotRevision(0),
      m_snapshotPageCount(0), m_snapshotNextPage(0)
{
    memset(m_clientNonce, 0, sizeof(m_clientNonce));
    memset(m_serverNonce, 0, sizeof(m_serverNonce));
    memset(m_clientPrivKey, 0, sizeof(m_clientPrivKey));
    memset(m_clientPubKey, 0, sizeof(m_clientPubKey));
    memset(m_serverPubKey, 0, sizeof(m_serverPubKey));
    memset(m_authSalt, 0, sizeof(m_authSalt));
    memset(m_expectedServerProof, 0, sizeof(m_expectedServerProof));
    memset(m_roomChallenge, 0, sizeof(m_roomChallenge));
    createSocket();

    QTimer* authPoll = new QTimer(this);
    connect(authPoll, &QTimer::timeout, this, &SignalClient::pollAuthTask);
    authPoll->start(10);
    m_pingTimer = new QTimer(this);
    connect(m_pingTimer, &QTimer::timeout, this, &SignalClient::onPingTimer);

    m_connectTimer = new QTimer(this);
    m_connectTimer->setSingleShot(true);
    connect(m_connectTimer, &QTimer::timeout, this, &SignalClient::onConnectTimeout);

    m_recvTimeoutTimer = new QTimer(this);
    connect(m_recvTimeoutTimer, &QTimer::timeout, this, &SignalClient::onRecvTimeoutCheck);
}

void SignalClient::createSocket() {
    QTcpSocket* socket = new QTcpSocket(this);
    m_socket = socket;
    connect(socket, &QTcpSocket::connected, this, [this, socket]() {
        if (m_socket == socket) onConnected();
    });
    connect(socket, &QTcpSocket::disconnected, this, [this, socket]() {
        if (m_socket == socket) onDisconnected();
    });
    connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
        if (m_socket == socket) onReadyRead();
    });
    connect(socket,
            static_cast<void (QTcpSocket::*)(QAbstractSocket::SocketError)>(
                &QTcpSocket::error),
            this, [this, socket](QAbstractSocket::SocketError error) {
        if (m_socket == socket) onSocketError(error);
    });
}

SignalClient::~SignalClient() {
    disconnect();
}

void SignalClient::cancelRoomOperation() {
    if (m_authOperation == 2 || m_authOperation == 3) {
        m_authTask.cancel(); m_authOperation = 0;
    }
    m_roomOperationPending = false; m_joinPending = false;
    if (!m_pendingRoomPassword.isEmpty()) crypto_wipe(m_pendingRoomPassword.data(), m_pendingRoomPassword.size());
    m_pendingRoomPassword.clear();
    crypto_wipe(m_roomChallenge, sizeof(m_roomChallenge));
    if (m_pendingCreate.size()) crypto_wipe(const_cast<uint8_t*>(m_pendingCreate.data()), m_pendingCreate.size());
    m_pendingCreate.clear();
}

void SignalClient::resetSecureState() {
    cancelRoomOperation();
    m_authTask.cancel();
    m_authOperation = 0;
    m_joinPending = false;
    m_roomOperationPending = false;
    crypto_wipe(m_expectedServerProof, sizeof(m_expectedServerProof));
    crypto_wipe(m_roomChallenge, sizeof(m_roomChallenge));
    crypto_wipe(m_authSalt, sizeof(m_authSalt));
    m_secureReady = false;
    m_serverAuthRequired = false;
    m_secureSessionId = 0;
    m_secureCipher.reset();
    if (!m_secureMaster.isEmpty()) {
        crypto_wipe(
            reinterpret_cast<uint8_t*>(m_secureMaster.data()),
            static_cast<size_t>(m_secureMaster.size()));
        m_secureMaster.clear();
    }
    crypto_wipe(m_clientNonce, sizeof(m_clientNonce));
    crypto_wipe(m_serverNonce, sizeof(m_serverNonce));
    crypto_wipe(m_clientPrivKey, sizeof(m_clientPrivKey));
    crypto_wipe(m_clientPubKey, sizeof(m_clientPubKey));
    crypto_wipe(m_serverPubKey, sizeof(m_serverPubKey));
    if (!m_pendingRoomPassword.isEmpty()) {
        crypto_wipe(reinterpret_cast<uint8_t*>(m_pendingRoomPassword.data()),
                    static_cast<size_t>(m_pendingRoomPassword.size()));
        m_pendingRoomPassword.clear();
    }
    resetRoomListState();
}

void SignalClient::resetRoomListState() {
    m_roomListRevision = 0;
    m_snapshotRevision = 0;
    m_snapshotPageCount = 0;
    m_snapshotNextPage = 0;
    m_roomListCache.clear();
    m_snapshotRooms.clear();
}

void SignalClient::connectToServer(const QString& host, quint16 port,
                                   int timeoutMs) {
    if (m_socket) {
        QTcpSocket* oldSocket = m_socket;
        m_socket = nullptr;
        QObject::disconnect(oldSocket, nullptr, this, nullptr);
        oldSocket->abort();
        oldSocket->deleteLater();
    }
    createSocket();
    m_serverHost       = host;
    m_serverPort       = port;
    m_connectTimeoutMs = timeoutMs;
    m_myPeerId         = 0;
    resetSecureState();
    m_fatalDisconnectPending = false;
    m_recvBuf.clear();
    m_connectStartedMs = currentTimeMs();
    m_socket->connectToHost(host, port);
    m_connectTimer->start(timeoutMs);
}

void SignalClient::setServerPassword(const QString& password) {
    if (m_authOperation == 1) { m_authTask.cancel(); m_authOperation = 0; }
    m_serverPassword = password;
    if (!m_secureReady && !m_secureMaster.isEmpty()) disconnect();
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
    resetSecureState();
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
                              RoomTrafficPolicy udpPolicy, uint16_t mtu,
                              bool passwordProtected, const QString& password) {
    if (m_roomOperationPending || m_authOperation != 0 || !m_secureReady) return;
    m_roomOperationPending = true;
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
    if (!passwordProtected) {
        bb.writeU16(normalizeRoomMtu(mtu)); sendMsg(MSG_CREATE_ROOM, bb); return;
    }
    if (!secureRandomBytes(m_authSalt, sizeof(m_authSalt))) {
        m_roomOperationPending = false; emit serverError(QStringLiteral("Secure random generation failed.")); return;
    }
    m_pendingCreate = bb;
    m_pendingCreateMtu = normalizeRoomMtu(mtu);
    QByteArray utf8 = password.toUtf8();
    m_authOperation = 2;
    m_authTask.start(reinterpret_cast<const uint8_t*>(utf8.constData()), utf8.size(), m_authSalt);
    if (!utf8.isEmpty()) crypto_wipe(utf8.data(), utf8.size());
}

void SignalClient::joinRoom(uint32_t roomId, const QString& password) {
    if (m_roomOperationPending || m_authOperation != 0 || !m_secureReady || roomId == 0) return;
    m_roomOperationPending = true;
    if (!m_pendingRoomPassword.isEmpty()) crypto_wipe(m_pendingRoomPassword.data(), m_pendingRoomPassword.size());
    m_pendingRoomPassword = password.toUtf8(); m_joinPending = true;
    ByteBuffer bb; bb.writeU32(roomId); sendMsg(MSG_JOIN_ROOM, bb);
}

void SignalClient::resumeRoom(uint32_t roomId, uint32_t peerId,
                              const QByteArray& resumeToken) {
    if (m_roomOperationPending || !m_secureReady || resumeToken.size() != RECONNECT_TOKEN_SIZE) return;
    m_roomOperationPending = true;
    ByteBuffer bb;
    bb.writeU32(roomId);
    bb.writeU32(peerId);
    bb.writeBytes(resumeToken.constData(), RECONNECT_TOKEN_SIZE);
    sendMsg(MSG_RESUME_ROOM, bb);
}

void SignalClient::leaveRoom() {
    // Pending requests have no correlation ID: close the session so late
    // challenge/response frames cannot complete a cancelled room operation.
    if (m_roomOperationPending) { disconnect(); return; }
    sendMsg(MSG_LEAVE_ROOM);
}

void SignalClient::logout() {
    cancelRoomOperation();
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

void SignalClient::sendRelayData(uint32_t srcPeerId, uint32_t dstPeerId,
                                 TrafficClass cls, const QByteArray& data) {
    ByteBuffer bb;
    bb.writeU32(srcPeerId);
    bb.writeU32(dstPeerId);
    bb.writeU8(static_cast<uint8_t>(cls));
    if (!data.isEmpty())
        bb.writeBytes(data.constData(), static_cast<size_t>(data.size()));
    sendMsg(MSG_TCP_RELAY_DATA, bb);
}

// Socket events

void SignalClient::onConnected() {
    m_connectTimer->stop();
    m_fatalDisconnectPending = false;
    m_socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    m_lastRecvTime = currentTimeMs(); m_authStartedMs = m_lastRecvTime;
    LogManager::instance().logNormal(QString("[auth] TCP connected elapsedMs=%1").arg(m_authStartedMs - m_connectStartedMs));
    m_recvTimeoutTimer->start(1000);
    sendClientHello();
}

void SignalClient::onDisconnected() {
    m_connectTimer->stop();
    m_pingTimer->stop();
    m_recvTimeoutTimer->stop();
    resetSecureState();
    emit disconnected();
}

void SignalClient::onSocketError(QAbstractSocket::SocketError err) {
    Q_UNUSED(err);
    QTcpSocket* failedSocket = m_socket;
    m_connectTimer->stop();
    m_pingTimer->stop();
    m_recvTimeoutTimer->stop();
    QString reason = m_socket->errorString();
    if (m_socket->state() != QAbstractSocket::ConnectedState) {
        QPointer<SignalClient> guard(this);
        failedSocket->abort();
        if (guard.isNull() || guard->m_socket != failedSocket) return;
        emit connectFailed(reason);
    }
}

void SignalClient::onConnectTimeout() {
    if (m_socket->state() != QAbstractSocket::ConnectedState) {
        QTcpSocket* timedOutSocket = m_socket;
        QPointer<SignalClient> guard(this);
        timedOutSocket->abort();
        if (guard.isNull() || guard->m_socket != timedOutSocket) return;
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

    static const int FRAME_HEADER_SIZE = 3;
    while (m_recvBuf.size() >= FRAME_HEADER_SIZE) {
        const TcpFrameProbeResult frame = probeTcpFrame(
            reinterpret_cast<const uint8_t*>(m_recvBuf.constData()),
            static_cast<size_t>(m_recvBuf.size()));
        if (frame.status == TcpFrameProbeStatus::Malformed) {
            handleStreamCorruption();
            return;
        }
        if (frame.status == TcpFrameProbeStatus::NeedMore)
            break;

        const uint8_t msgType = frame.msgType;
        const uint16_t payloadLen = frame.payloadLength;
        const int frameLen = static_cast<int>(frame.frameLength);
        const QByteArray payload =
            m_recvBuf.mid(FRAME_HEADER_SIZE, payloadLen);
        m_recvBuf.remove(0, frameLen);

        QPointer<SignalClient> guard(this);
        const uint8_t* payloadData = payloadLen > 0
            ? reinterpret_cast<const uint8_t*>(payload.constData()) : nullptr;
        if (!processMessage(msgType, payloadData, payloadLen))
            return;
        if (guard.isNull() ||
            m_socket->state() != QAbstractSocket::ConnectedState)
            return;
    }
}

void SignalClient::handleStreamCorruption() {
    handleMalformedFrame(0, 0, "stream-corruption", 0);
}

void SignalClient::handleMalformedFrame(uint8_t msgType, size_t len,
                                        const char* error, size_t offset) {
    if (m_fatalDisconnectPending) return;
    m_fatalDisconnectPending = true;
    LogManager::instance().logError(
        QString("[signal] Invalid frame type=0x%1 len=%2 error=%3 offset=%4")
            .arg(msgType, 2, 16, QLatin1Char('0'))
            .arg(static_cast<qulonglong>(len))
            .arg(QString::fromLatin1(error ? error : "unknown"))
            .arg(static_cast<qulonglong>(offset)));
    m_recvBuf.clear();
    if (m_socket->state() != QAbstractSocket::UnconnectedState)
        m_socket->abort();
}

void SignalClient::onRecvTimeoutCheck() {
    if (!isConnected()) return;
    uint32_t now = currentTimeMs();
    uint32_t elapsed = now - m_lastRecvTime;
    if (!m_secureReady) {
        if (now - m_authStartedMs > static_cast<uint32_t>(SERVER_AUTH_TIMEOUT_SEC * 1000))
            failAuthentication(AuthErrorCode::AuthTimeout, QStringLiteral("Server authentication timed out (120 seconds)."));
        return;
    }
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
    if (!secureRandomBytes(m_clientNonce, 16) ||
        !secureRandomBytes(m_clientPrivKey, 32)) {
        emit serverError(QStringLiteral("Secure random generation failed."));
        m_socket->abort();
        return;
    }
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

void SignalClient::failAuthentication(AuthErrorCode code, const QString& message) {
    QPointer<SignalClient> guard(this);
    emit fatalAuthError(static_cast<uint16_t>(code), message);
    if (guard.isNull()) return;
    emit serverError(message);
    if (guard.isNull()) return;
    m_socket->abort();
    if (guard.isNull()) return;
    resetSecureState();
}

void SignalClient::sendServerAuth() {
    if (m_authOperation != 0 || m_secureReady || !m_secureMaster.isEmpty()) return;
    if (m_serverPassword.isEmpty()) { emit serverPasswordRequired(); return; }
    QByteArray password = m_serverPassword.toUtf8(); m_authOperation = 1;
    m_authTask.start(reinterpret_cast<const uint8_t*>(password.constData()), password.size(), m_authSalt);
    crypto_wipe(password.data(), password.size());
}

void SignalClient::pollAuthTask() {
    AuthKdfTask::Result result;
    if (!m_authTask.poll(result)) return;
    const int operation = m_authOperation; m_authOperation = 0;
    if (operation == 0 || !isConnected()) return;
    LogManager::instance().logNormal(QString("[auth] KDF operation=%1 elapsedMs=%2 success=%3")
        .arg(operation).arg(static_cast<qulonglong>(result.elapsedMs)).arg(result.success));
    if (!result.success) { failAuthentication(AuthErrorCode::KdfFailed, QStringLiteral("Authentication KDF failed.")); return; }
    if (operation == 2) {
        m_pendingCreate.writeBytes(m_authSalt, AUTH_SALT_SIZE);
        m_pendingCreate.writeBytes(result.hash, 32); m_pendingCreate.writeU16(m_pendingCreateMtu);
        sendMsg(MSG_CREATE_ROOM, m_pendingCreate); m_pendingCreate = ByteBuffer(); return;
    }
    if (operation == 3) {
        uint8_t response[32]; computeChallengeResponse(result.hash, m_roomChallenge, response);
        ByteBuffer body; body.writeBytes(response, sizeof(response)); sendMsg(MSG_AUTH_RESPONSE, body);
        crypto_wipe(response, sizeof(response)); crypto_wipe(m_roomChallenge, sizeof(m_roomChallenge)); return;
    }
    uint8_t shared[32], master[32], proof[32];
    crypto_x25519(shared, m_clientPrivKey, m_serverPubKey); crypto_wipe(m_clientPrivKey, sizeof(m_clientPrivKey));
    if (!validX25519Shared(shared)) {
        crypto_wipe(shared, sizeof(shared));
        failAuthentication(AuthErrorCode::InvalidKey, QStringLiteral("Invalid X25519 server key.")); return;
    }
    deriveSecureMaster(master, shared, result.hash, m_clientNonce, m_serverNonce,
                       m_clientPubKey, m_serverPubKey, m_authSalt);
    computeClientAuthProof(proof, master, result.hash);
    computeServerAuthProof(m_expectedServerProof, master, result.hash);
    m_secureMaster = QByteArray(reinterpret_cast<const char*>(master), sizeof(master));
    ByteBuffer body; body.writeBytes(proof, sizeof(proof));
    TcpMsgHeader hdr; hdr.msgType = MSG_SERVER_AUTH; hdr.length = htons(static_cast<uint16_t>(body.size()));
    m_socket->write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    m_socket->write(reinterpret_cast<const char*>(body.data()), body.size());
    crypto_wipe(shared, sizeof(shared)); crypto_wipe(master, sizeof(master)); crypto_wipe(proof, sizeof(proof));
}

void SignalClient::sendMsg(uint8_t msgType, const ByteBuffer& body) {
    if (m_socket->state() != QAbstractSocket::ConnectedState) return;
    if (body.size() > MAX_TCP_MSG_PAYLOAD) {
        LogManager::instance().logError(
            QString("[signal] Refusing oversized payload type=0x%1 size=%2")
                .arg(msgType, 2, 16, QLatin1Char('0'))
                .arg(static_cast<qulonglong>(body.size())));
        return;
    }

    if (!m_secureReady) {
        LogManager::instance().logError(
            QStringLiteral("[signal] Refusing business message before authentication"));
        return;
    }
    std::vector<uint8_t> plain;
    plain.reserve(1 + body.size());
    plain.push_back(msgType);
    if (body.size() > 0)
        plain.insert(plain.end(), body.data(), body.data() + body.size());
    std::vector<uint8_t> enc = m_secureCipher.encrypt(plain.data(), plain.size());
    if (enc.empty()) {
        QPointer<SignalClient> guard(this);
        emit serverError(QStringLiteral("Secure counter exhausted; reconnect required."));
        if (guard.isNull()) return;
        m_socket->abort(); resetSecureState(); return;
    }
    if (enc.size() > MAX_TCP_MSG_PAYLOAD) {
        LogManager::instance().logError(
            QStringLiteral("[signal] Encrypted payload exceeds protocol limit"));
        return;
    }
    TcpMsgHeader hdr;
    hdr.msgType = MSG_ENCRYPTED;
    hdr.length  = htons(static_cast<uint16_t>(enc.size()));
    m_socket->write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    if (!enc.empty())
        m_socket->write(reinterpret_cast<const char*>(enc.data()), enc.size());
}

void SignalClient::sendMsg(uint8_t msgType) {
    if (m_socket->state() != QAbstractSocket::ConnectedState) return;
    ByteBuffer empty;
    sendMsg(msgType, empty);
}

RoomListItem SignalClient::readRoomListItem(ByteBuffer& bb) {
    RoomListItem info;
    memset(&info, 0, sizeof(info));
    info.roomId = bb.readU32();
    const std::string name = bb.readString();
    strncpy(info.roomName, name.c_str(), MAX_ROOM_NAME_LEN);
    info.roomName[MAX_ROOM_NAME_LEN] = '\0';
    info.playerCount = bb.readU8();
    info.maxPlayers = bb.readU8();
    info.tcpPolicy = readTrafficPolicy(bb, makeDefaultTcpPolicy());
    info.udpPolicy = readTrafficPolicy(bb, makeDefaultUdpPolicy());
    info.passwordProtected = bb.readU8();
    info.mtu = bb.readU16();
    return info;
}

void SignalClient::emitCurrentRoomList() {
    emit roomList(m_roomListCache.values());
}

// Incoming message dispatch

bool SignalClient::processMessage(uint8_t msgType,
                                  const uint8_t* payload, size_t len)
{
    try {
        const bool allowedPlainHandshake = !m_secureReady &&
            (msgType == MSG_ERROR ||
             (msgType == MSG_SERVER_HELLO && !m_serverAuthRequired && m_authOperation == 0 && m_secureMaster.isEmpty()) ||
             (msgType == MSG_SERVER_AUTH_OK && m_serverAuthRequired && m_authOperation == 0 && m_secureMaster.size() == SECURE_KEY_SIZE));
        if ((!m_secureReady && !allowedPlainHandshake) ||
            (m_secureReady && msgType != MSG_ENCRYPTED)) {
            failAuthentication(AuthErrorCode::InvalidServerProof, QStringLiteral("Unexpected signaling message before authentication / handshake order violation.")); return false;
        }

        MessageValidationResult validation =
            validateServerSignalPayload(msgType, payload, len);
        if (validation.status == MessageValidationStatus::Malformed) {
            if (!m_secureReady || validation.error == MessageValidationError::InvalidVersion) {
                const AuthErrorCode code = validation.error == MessageValidationError::InvalidVersion || msgType == MSG_ERROR
                    ? AuthErrorCode::VersionMismatch : AuthErrorCode::InvalidServerProof;
                failAuthentication(code, QStringLiteral("Invalid pre-auth protocol response; this client requires protocol v9. Upgrade GUI/CLI and server together.")); return false;
            }
            handleMalformedFrame(msgType, len, messageValidationErrorName(validation.error), validation.offset);
            return false;
        }
        if (validation.status == MessageValidationStatus::UnknownType) {
            LogManager::instance().logDetail(
                QString("[signal] Ignoring unknown outer type=0x%1 len=%2")
                    .arg(msgType, 2, 16, QLatin1Char('0'))
                    .arg(static_cast<qulonglong>(len)));
            return true;
        }

        std::vector<uint8_t> plain;
        if (msgType == MSG_ENCRYPTED) {
            if (!m_secureReady ||
                !m_secureCipher.decrypt(payload, len, &plain) ||
                plain.empty()) {
                handleMalformedFrame(msgType, len, "decrypt-failed", 0);
                return false;
            }
            msgType = plain[0];
            if (msgType == MSG_SERVER_HELLO || msgType == MSG_SERVER_AUTH_OK || msgType == MSG_ENCRYPTED) {
                failAuthentication(AuthErrorCode::InvalidServerProof, QStringLiteral("Unexpected signaling message before authentication / handshake order violation.")); return false;
            }
            payload = plain.size() > 1 ? plain.data() + 1 : nullptr;
            len = plain.size() > 1 ? plain.size() - 1 : 0;
            validation = validateServerSignalPayload(msgType, payload, len);
            if (validation.status == MessageValidationStatus::Malformed) {
                if (validation.error ==
                    MessageValidationError::InvalidVersion) {
                    uint16_t receivedVersion = 0;
                    if (msgType == MSG_LOGIN_RESP && len >= 6) {
                        receivedVersion = static_cast<uint16_t>(
                            (static_cast<uint16_t>(payload[4]) << 8) |
                             static_cast<uint16_t>(payload[5]));
                    }
                    QPointer<SignalClient> guard(this);
                    emit serverError(
                        UiStrings::text("error.protocolMismatch")
                            .arg(PROTOCOL_VERSION)
                            .arg(receivedVersion));
                    if (guard.isNull()) return false;
                }
                handleMalformedFrame(msgType, len,
                                     messageValidationErrorName(validation.error),
                                     validation.offset);
                return false;
            }
        } else if (m_secureReady) {
            handleMalformedFrame(msgType, len, "plaintext-in-secure-mode", 0);
            return false;
        }

        if (g_verboseLog) {
            LogManager::instance().logDetail(
                QString("[signal] Received type=0x%1 len=%2")
                    .arg(msgType, 2, 16, QLatin1Char('0'))
                    .arg(static_cast<qulonglong>(len)));
        }

        if (validation.status == MessageValidationStatus::UnknownType) {
            LogManager::instance().logDetail(
                QString("[signal] Ignoring unknown type=0x%1 len=%2")
                    .arg(msgType, 2, 16, QLatin1Char('0'))
                    .arg(static_cast<qulonglong>(len)));
            return true;
        }

        ByteBuffer bb(payload, len);

        if (msgType == MSG_SERVER_HELLO) {
            const uint16_t serverVersion = bb.readU16();
            const bool authRequired = bb.readU8() != 0;
            if (serverVersion != PROTOCOL_VERSION) {
                QPointer<SignalClient> guard(this);
                emit serverError(UiStrings::text("error.protocolMismatch")
                                 .arg(PROTOCOL_VERSION).arg(serverVersion));
                if (guard.isNull()) return false;
                handleMalformedFrame(msgType, len, "invalid-version", 0);
                return false;
            }
            m_serverAuthRequired = authRequired;
            if (!authRequired) {
                QPointer<SignalClient> guard(this);
                emit serverError(QStringLiteral("不安全服务端被拒绝：服务端未强制鉴权。"));
                if (guard.isNull()) return false;
                m_socket->abort();
                return false;
            }
            const uint8_t profile = bb.readU8();
            if (profile != AUTH_KDF_PROFILE) {
                failAuthentication(AuthErrorCode::UnsupportedKdf, QStringLiteral("Unsupported authentication KDF profile.")); return false;
            }
            bb.readBytes(m_authSalt, AUTH_SALT_SIZE);
            bb.readBytes(m_serverNonce, 16);
            bb.readBytes(m_serverPubKey, 32);
            if (m_serverPassword.isEmpty())
                emit serverPasswordRequired();
            else
                sendServerAuth();
            return true;
        }

        if (msgType == MSG_SERVER_AUTH_OK) {
            if (m_secureMaster.size() != 32) {
                QPointer<SignalClient> guard(this);
                emit serverError(UiStrings::text("error.invalidAuthResponse"));
                if (guard.isNull()) return false;
                handleMalformedFrame(msgType, len, "auth-state-invalid", 0);
                return false;
            }

            const uint32_t sessionId = bb.readU32();
            QByteArray serverProof(32, '\0');
            bb.readBytes(serverProof.data(), 32);

            const bool proofValid = crypto_verify32(m_expectedServerProof,
                reinterpret_cast<const uint8_t*>(serverProof.constData())) == 0 &&
                sessionId == deriveSessionId(reinterpret_cast<const uint8_t*>(m_secureMaster.constData()));
            crypto_wipe(m_expectedServerProof, sizeof(m_expectedServerProof));
            crypto_wipe(serverProof.data(), serverProof.size());

            if (!proofValid) {
                QPointer<SignalClient> guard(this);
                emit fatalAuthError(static_cast<uint16_t>(AuthErrorCode::InvalidServerProof), UiStrings::text("error.serverProofFailed"));
                if (guard.isNull()) return false;
                emit serverError(UiStrings::text("error.serverProofFailed"));
                if (guard.isNull()) return false;
                handleMalformedFrame(msgType, len, "invalid-server-proof", 0);
                return false;
            }

            m_secureSessionId = sessionId;
            m_secureCipher.init(
                reinterpret_cast<const uint8_t*>(m_secureMaster.constData()),
                true, "signal");
            m_secureReady = true;
            LogManager::instance().logNormal(QString("[auth] Server authenticated protocol=%1 elapsedMs=%2")
                .arg(PROTOCOL_VERSION).arg(currentTimeMs() - m_authStartedMs));
            m_pingTimer->start(KEEPALIVE_INTERVAL_MS);
            QPointer<SignalClient> guard(this);
            emit secureSessionEstablished(m_secureSessionId, m_secureMaster);
            if (guard.isNull() ||
                guard->m_socket->state() != QAbstractSocket::ConnectedState)
                return false;
            emit connected();
            return true;
        }

        switch (msgType) {
        case MSG_LOGIN_RESP: {
            const uint32_t peerId = bb.readU32();
            const uint16_t serverVersion = bb.readU16();
            const bool resumeAccepted = bb.readU8() != 0;
            if (serverVersion != PROTOCOL_VERSION) {
                QPointer<SignalClient> guard(this);
                emit serverError(UiStrings::text("error.protocolMismatch")
                                 .arg(PROTOCOL_VERSION).arg(serverVersion));
                if (guard.isNull()) return false;
                handleMalformedFrame(msgType, len, "invalid-version", 4);
                return false;
            }
            m_myPeerId = peerId;
            emit loginResponse(peerId, resumeAccepted);
            return true;
        }
        case MSG_ROOM_CREATED: {
            m_roomOperationPending = false;
            const uint32_t roomId = bb.readU32();
            const uint32_t vip = bb.readU32();
            const RoomTrafficPolicy tcpPolicy =
                readTrafficPolicy(bb, makeDefaultTcpPolicy());
            const RoomTrafficPolicy udpPolicy =
                readTrafficPolicy(bb, makeDefaultUdpPolicy());
            const bool passwordProtected = bb.readU8() != 0;
            const uint16_t mtu = bb.readU16();
            QByteArray leaseToken(RECONNECT_TOKEN_SIZE, '\0');
            bb.readBytes(leaseToken.data(), RECONNECT_TOKEN_SIZE);
            emit roomCreated(roomId, vip, tcpPolicy, udpPolicy, mtu,
                             passwordProtected, leaseToken);
            return true;
        }
        case MSG_JOIN_RESP: {
            m_roomOperationPending = false;
            m_joinPending = false;
            const uint32_t roomId = bb.readU32();
            const uint32_t vip = bb.readU32();
            const RoomTrafficPolicy tcpPolicy =
                readTrafficPolicy(bb, makeDefaultTcpPolicy());
            const RoomTrafficPolicy udpPolicy =
                readTrafficPolicy(bb, makeDefaultUdpPolicy());
            const bool passwordProtected = bb.readU8() != 0;
            const uint16_t mtu = bb.readU16();
            const uint8_t count = bb.readU8();
            QList<PeerInfo> members;
            for (uint8_t i = 0; i < count; ++i) {
                PeerInfo info;
                info.peerId = bb.readU32();
                info.virtualIP = bb.readU32();
                info.name = bb.readString();
                info.transport = TRANSPORT_NONE;
                members.append(info);
            }
            QByteArray leaseToken(RECONNECT_TOKEN_SIZE, '\0');
            bb.readBytes(leaseToken.data(), RECONNECT_TOKEN_SIZE);
            if (!m_pendingRoomPassword.isEmpty())
                crypto_wipe(reinterpret_cast<uint8_t*>(m_pendingRoomPassword.data()),
                            static_cast<size_t>(m_pendingRoomPassword.size()));
            m_pendingRoomPassword.clear();
            emit joinResponse(roomId, vip, tcpPolicy, udpPolicy, mtu,
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
            if (msgType == MSG_PEER_JOINED)
                emit peerJoined(info);
            else
                emit peerResumed(info);
            return true;
        }
        case MSG_PEER_LEFT:
            emit peerLeft(bb.readU32());
            return true;

        case MSG_LOGOUT_ACK:
            m_myPeerId = 0;
            emit logoutAck();
            return true;

        case MSG_ROOM_LIST: {
            const uint64_t revision = bb.readU64();
            const uint16_t pageIndex = bb.readU16();
            const uint16_t pageCount = bb.readU16();
            const uint16_t count = bb.readU16();
            if (pageIndex == 0) {
                m_snapshotRooms.clear();
                m_snapshotRevision = revision;
                m_snapshotPageCount = pageCount;
                m_snapshotNextPage = 0;
            }
            if (revision != m_snapshotRevision ||
                pageCount != m_snapshotPageCount ||
                pageIndex != m_snapshotNextPage) {
                m_snapshotRooms.clear();
                m_snapshotRevision = 0;
                m_snapshotPageCount = 0;
                m_snapshotNextPage = 0;
                emit roomListResyncRequired();
                return true;
            }
            for (uint16_t i = 0; i < count; ++i) {
                const RoomListItem info = readRoomListItem(bb);
                if (m_snapshotRooms.contains(info.roomId)) {
                    m_snapshotRooms.clear();
                    m_snapshotRevision = 0;
                    m_snapshotPageCount = 0;
                    m_snapshotNextPage = 0;
                    emit roomListResyncRequired();
                    return true;
                }
                m_snapshotRooms.insert(info.roomId, info);
            }
            ++m_snapshotNextPage;
            if (m_snapshotNextPage == m_snapshotPageCount) {
                m_roomListCache = m_snapshotRooms;
                m_roomListRevision = m_snapshotRevision;
                m_snapshotRooms.clear();
                m_snapshotRevision = 0;
                m_snapshotPageCount = 0;
                m_snapshotNextPage = 0;
                emitCurrentRoomList();
            }
            return true;
        }
        case MSG_ROOM_LIST_PUSH: {
            const uint64_t baseRevision = bb.readU64();
            const uint64_t revision = bb.readU64();
            const uint16_t count = bb.readU16();
            if (baseRevision != m_roomListRevision) {
                m_snapshotRooms.clear();
                m_snapshotRevision = 0;
                m_snapshotPageCount = 0;
                m_snapshotNextPage = 0;
                emit roomListResyncRequired();
                return true;
            }
            QMap<uint32_t, RoomListItem> updated = m_roomListCache;
            for (uint16_t i = 0; i < count; ++i) {
                const uint8_t operation = bb.readU8();
                if (operation == ROOM_LIST_UPSERT) {
                    const RoomListItem info = readRoomListItem(bb);
                    updated.insert(info.roomId, info);
                } else {
                    updated.remove(bb.readU32());
                }
            }
            m_roomListCache = updated;
            m_roomListRevision = revision;
            emitCurrentRoomList();
            return true;
        }
        case MSG_RELAY_READY:
            emit relayReady(bb.readU32());
            return true;

        case MSG_TCP_RELAY_DATA: {
            const uint32_t srcPeerId = bb.readU32();
            const uint32_t dstPeerId = bb.readU32();
            const TrafficClass cls =
                static_cast<TrafficClass>(bb.readU8());
            if (dstPeerId != m_myPeerId)
                return true;
            const size_t dataLen = bb.remaining();
            const char* dataStart = reinterpret_cast<const char*>(
                payload + len - dataLen);
            emit relayDataReceived(
                srcPeerId, cls,
                QByteArray(dataStart, static_cast<int>(dataLen)));
            return true;
        }

        case MSG_PONG:
            if (m_pingSentTime != 0) {
                const int rtt =
                    static_cast<int>(currentTimeMs() - m_pingSentTime);
                m_pingSentTime = 0;
                emit serverRttUpdated(rtt);
            }
            return true;

        case MSG_AUTH_CHALLENGE: {
            const uint8_t profile = bb.readU8();
            if (profile != AUTH_KDF_PROFILE) {
                failAuthentication(AuthErrorCode::UnsupportedKdf, QStringLiteral("Unsupported room KDF profile.")); return false;
            }
            if (!m_joinPending) { handleMalformedFrame(msgType, len, "unexpected-room-challenge", 0); return false; }
            m_joinPending = false; bb.readBytes(m_authSalt, AUTH_SALT_SIZE);
            bb.readBytes(m_roomChallenge, sizeof(m_roomChallenge)); m_authOperation = 3;
            m_authTask.start(reinterpret_cast<const uint8_t*>(m_pendingRoomPassword.constData()), m_pendingRoomPassword.size(), m_authSalt);
            if (!m_pendingRoomPassword.isEmpty()) crypto_wipe(m_pendingRoomPassword.data(), m_pendingRoomPassword.size());
            m_pendingRoomPassword.clear(); return true;
        }

        case MSG_ERROR: {
            cancelRoomOperation();
            const uint16_t code = bb.readU16();
            const QString message = QString::fromStdString(bb.readString());
            if (isFatalAuthError(code)) { failAuthentication(static_cast<AuthErrorCode>(code), message); return false; }
            if (code == static_cast<uint16_t>(AuthErrorCode::RoomPassword)) {
                QPointer<SignalClient> guard(this); emit fatalAuthError(code, message);
                if (guard.isNull()) return false;
            }
            if (!m_pendingRoomPassword.isEmpty())
                crypto_wipe(reinterpret_cast<uint8_t*>(m_pendingRoomPassword.data()),
                            static_cast<size_t>(m_pendingRoomPassword.size()));
            m_pendingRoomPassword.clear();
            emit serverError(message);
            return true;
        }
        default:
            return true;
        }
    } catch (const std::exception&) {
        handleMalformedFrame(msgType, len, "parser-exception", 0);
        return false;
    } catch (...) {
        handleMalformedFrame(msgType, len, "unknown-parser-exception", 0);
        return false;
    }
}

} // namespace VLan

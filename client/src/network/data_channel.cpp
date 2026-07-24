#include "data_channel.h"
#include "net_common.h"
#include "signal_message_validator.h"
#include "tcp_frame_probe.h"
#include "../ui/log_manager.h"
#include <QPointer>

namespace VLan {

DataChannel::DataChannel(QObject* parent)
    : QObject(parent), m_port(0), m_peerId(0),
      m_established(false), m_lastRecvTime(0),
      m_securityMode(DataPlaneSecurityMode::Unconfigured),
      m_secureSessionId(0)
{
    m_socket = new QTcpSocket(this);
    connect(m_socket, &QTcpSocket::connected,    this, &DataChannel::onSocketConnected);
    connect(m_socket, &QTcpSocket::disconnected, this, &DataChannel::onSocketDisconnected);
    connect(m_socket, &QTcpSocket::readyRead,    this, &DataChannel::onReadyRead);
    connect(m_socket, SIGNAL(error(QAbstractSocket::SocketError)),
            this, SLOT(onSocketError(QAbstractSocket::SocketError)));

    m_keepaliveTimer = new QTimer(this);
    connect(m_keepaliveTimer, &QTimer::timeout, this, &DataChannel::onKeepaliveTimer);

    m_reconnectTimer = new QTimer(this);
    m_reconnectTimer->setSingleShot(true);
    connect(m_reconnectTimer, &QTimer::timeout, this, &DataChannel::onReconnectTimer);
}

DataChannel::~DataChannel() {
    disconnect();
    clearSecurityContext();
}

void DataChannel::connectToServer(const QString& host, quint16 port, uint32_t peerId) {
    if (m_securityMode == DataPlaneSecurityMode::Unconfigured) {
        LogManager::instance().logError(
            QStringLiteral("[datachannel] Refusing connection with unconfigured security mode"));
        return;
    }
    m_host   = host;
    m_port   = port;
    m_peerId = peerId;
    m_established = false;
    m_recvBuf.clear();
    m_reconnectTimer->stop();

    if (m_socket->state() != QAbstractSocket::UnconnectedState)
        m_socket->abort();

    LogManager::instance().logDetail(QString("[datachannel] Connecting for peer %1").arg(peerId));
    m_socket->connectToHost(host, port);
}

void DataChannel::disconnect() {
    m_keepaliveTimer->stop();
    m_reconnectTimer->stop();
    m_established = false;
    m_peerId = 0;
    m_port = 0;
    m_recvBuf.clear();
    if (m_socket->state() != QAbstractSocket::UnconnectedState)
        m_socket->abort();
}

bool DataChannel::isConnected() const {
    return m_established &&
           m_socket->state() == QAbstractSocket::ConnectedState;
}

void DataChannel::configurePlaintextSession() {
    if (m_socket->state() != QAbstractSocket::UnconnectedState) {
        LogManager::instance().logError(
            QStringLiteral("[datachannel] Cannot change security mode while connected"));
        return;
    }
    if (!m_secureMaster.isEmpty())
        crypto_wipe(reinterpret_cast<uint8_t*>(m_secureMaster.data()),
                    static_cast<size_t>(m_secureMaster.size()));
    m_secureMaster.clear();
    m_secureSessionId = 0;
    m_cipher.reset();
    m_securityMode = DataPlaneSecurityMode::Plaintext;
}

bool DataChannel::installSecureSession(uint32_t sessionId,
                                       const QByteArray& master) {
    if (m_socket->state() != QAbstractSocket::UnconnectedState) {
        LogManager::instance().logError(
            QStringLiteral("[datachannel] Cannot change security mode while connected"));
        return false;
    }
    if (sessionId == 0 || master.size() != SECURE_KEY_SIZE) {
        clearSecurityContext();
        return false;
    }
    if (!m_secureMaster.isEmpty())
        crypto_wipe(reinterpret_cast<uint8_t*>(m_secureMaster.data()),
                    static_cast<size_t>(m_secureMaster.size()));
    m_secureSessionId = sessionId;
    m_secureMaster = master;
    m_cipher.init(reinterpret_cast<const uint8_t*>(m_secureMaster.constData()),
                  true, "data");
    m_securityMode = DataPlaneSecurityMode::Secure;
    return true;
}

void DataChannel::clearSecurityContext() {
    if (m_socket->state() != QAbstractSocket::UnconnectedState) {
        LogManager::instance().logError(
            QStringLiteral("[datachannel] Refusing to clear security while connected"));
        return;
    }
    if (!m_secureMaster.isEmpty())
        crypto_wipe(reinterpret_cast<uint8_t*>(m_secureMaster.data()),
                    static_cast<size_t>(m_secureMaster.size()));
    m_secureMaster.clear();
    m_secureSessionId = 0;
    m_cipher.reset();
    m_securityMode = DataPlaneSecurityMode::Unconfigured;
}

void DataChannel::sendRelayData(uint32_t srcPeerId, uint32_t dstPeerId,
                                TrafficClass cls, const QByteArray& data)
{
    if (!isConnected() ||
        m_securityMode == DataPlaneSecurityMode::Unconfigured)
        return;
    if (g_verboseLog)
        LogManager::instance().logDetail(QString("[datachannel] sendRelayData src=%1 dst=%2 dataSize=%3").arg(srcPeerId).arg(dstPeerId).arg(data.size()));
    ByteBuffer bb;
    bb.writeU32(srcPeerId);
    bb.writeU32(dstPeerId);
    bb.writeU8(static_cast<uint8_t>(cls));
    bb.writeBytes(data.constData(), data.size());
    sendMsg(MSG_TCP_RELAY_DATA, bb);
}

void DataChannel::onSocketConnected() {
    m_socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);

    ByteBuffer bb;
    if (m_securityMode == DataPlaneSecurityMode::Secure) {
        ByteBuffer inner;
        inner.writeU32(m_peerId);
        std::vector<uint8_t> enc = m_cipher.encrypt(inner.data(), inner.size());
        uint8_t sid[4];
        writeU32BE(sid, m_secureSessionId);
        bb.writeBytes(sid, 4);
        if (!enc.empty())
            bb.writeBytes(enc.data(), enc.size());
    } else if (m_securityMode == DataPlaneSecurityMode::Plaintext) {
        bb.writeU32(m_peerId);
    } else {
        m_socket->abort();
        return;
    }
    sendMsg(MSG_DATA_CHANNEL_INIT, bb);

    LogManager::instance().logDetail(QString("[datachannel] TCP connected, sent DATA_CHANNEL_INIT for peer %1").arg(m_peerId));
}

void DataChannel::onSocketDisconnected() {
    m_keepaliveTimer->stop();
    bool wasEstablished = m_established;
    m_established = false;
    m_recvBuf.clear();

    QPointer<DataChannel> guard(this);
    if (wasEstablished) {
        LogManager::instance().logDetail(QString("[datachannel] Disconnected"));
        emit disconnected();
    }

    if (guard.isNull()) return;
    if (m_peerId != 0)
        scheduleReconnect();
}

void DataChannel::onSocketError(QAbstractSocket::SocketError err) {
    Q_UNUSED(err);
    LogManager::instance().logError(QString("[datachannel] Socket error: %1").arg(m_socket->errorString()));

    m_keepaliveTimer->stop();
    bool wasEstablished = m_established;
    m_established = false;
    m_recvBuf.clear();

    QPointer<DataChannel> guard(this);
    if (m_socket->state() != QAbstractSocket::ConnectedState)
        m_socket->abort();

    if (guard.isNull()) return;
    if (wasEstablished)
        emit disconnected();

    if (guard.isNull()) return;
    if (m_peerId != 0)
        scheduleReconnect();
}

void DataChannel::onReadyRead() {
    m_recvBuf.append(m_socket->readAll());
    m_lastRecvTime = currentTimeMs();

    static const int FRAME_HEADER_SIZE = 3;
    while (m_recvBuf.size() >= FRAME_HEADER_SIZE) {
        const TcpFrameProbeResult frame = probeTcpFrame(
            reinterpret_cast<const uint8_t*>(m_recvBuf.constData()),
            static_cast<size_t>(m_recvBuf.size()));
        if (frame.status == TcpFrameProbeStatus::Malformed) {
            LogManager::instance().logError(QString("[datachannel] Stream corrupted, reconnecting"));
            m_recvBuf.clear();
            m_socket->abort();
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

        QPointer<DataChannel> guard(this);
        const uint8_t* payloadData = payloadLen > 0
            ? reinterpret_cast<const uint8_t*>(payload.constData()) : nullptr;
        if (!processMessage(msgType, payloadData, payloadLen))
            return;
        if (guard.isNull() ||
            m_socket->state() != QAbstractSocket::ConnectedState)
            return;
    }
}

bool DataChannel::processMessage(uint8_t msgType,
                                 const uint8_t* payload, size_t len) {
    try {
        MessageValidationResult validation =
            validateServerDataPayload(msgType, payload, len);
        if (validation.status == MessageValidationStatus::Malformed) {
            handleMalformedFrame(msgType, len,
                                 messageValidationErrorName(validation.error),
                                 validation.offset);
            return false;
        }
        if (validation.status == MessageValidationStatus::UnknownType) {
            LogManager::instance().logDetail(
                QString("[datachannel] Ignoring unknown outer type=0x%1 len=%2")
                    .arg(msgType, 2, 16, QLatin1Char('0'))
                    .arg(static_cast<qulonglong>(len)));
            return true;
        }

        std::vector<uint8_t> plain;
        if (msgType == MSG_ENCRYPTED) {
            if (m_securityMode != DataPlaneSecurityMode::Secure ||
                !m_cipher.decrypt(payload, len, &plain) ||
                plain.empty()) {
                handleMalformedFrame(msgType, len, "decrypt-failed", 0);
                return false;
            }
            msgType = plain[0];
            payload = plain.size() > 1 ? plain.data() + 1 : nullptr;
            len = plain.size() > 1 ? plain.size() - 1 : 0;
            validation = validateServerDataPayload(msgType, payload, len);
            if (validation.status == MessageValidationStatus::Malformed) {
                handleMalformedFrame(msgType, len,
                                     messageValidationErrorName(validation.error),
                                     validation.offset);
                return false;
            }
        } else if (m_securityMode == DataPlaneSecurityMode::Secure) {
            handleMalformedFrame(msgType, len,
                                 "plaintext-in-secure-mode", 0);
            return false;
        } else if (m_securityMode == DataPlaneSecurityMode::Unconfigured) {
            handleMalformedFrame(msgType, len,
                                 "security-unconfigured", 0);
            return false;
        }

        if (g_verboseLog) {
            LogManager::instance().logDetail(
                QString("[datachannel] Received type=0x%1 len=%2")
                    .arg(msgType, 2, 16, QLatin1Char('0'))
                    .arg(static_cast<qulonglong>(len)));
        }

        if (validation.status == MessageValidationStatus::UnknownType) {
            LogManager::instance().logDetail(
                QString("[datachannel] Ignoring unknown type=0x%1 len=%2")
                    .arg(msgType, 2, 16, QLatin1Char('0'))
                    .arg(static_cast<qulonglong>(len)));
            return true;
        }

        switch (msgType) {
        case MSG_DATA_CHANNEL_ACK:
            m_established = true;
            m_lastRecvTime = currentTimeMs();
            m_keepaliveTimer->start(TCP_RELAY_KEEPALIVE_MS);
            emit connected();
            return true;

        case MSG_TCP_RELAY_DATA: {
            ByteBuffer bb(payload, len);
            const uint32_t srcId = bb.readU32();
            bb.readU32();
            const TrafficClass cls =
                static_cast<TrafficClass>(bb.readU8());
            const size_t dataLen = bb.remaining();
            const QByteArray data(
                reinterpret_cast<const char*>(payload + len - dataLen),
                static_cast<int>(dataLen));
            emit relayDataReceived(srcId, cls, data);
            return true;
        }
        case MSG_PONG:
            return true;
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

void DataChannel::handleMalformedFrame(uint8_t msgType, size_t len,
                                       const char* error, size_t offset) {
    LogManager::instance().logError(
        QString("[datachannel] Invalid frame type=0x%1 len=%2 error=%3 offset=%4")
            .arg(msgType, 2, 16, QLatin1Char('0'))
            .arg(static_cast<qulonglong>(len))
            .arg(QString::fromLatin1(error ? error : "unknown"))
            .arg(static_cast<qulonglong>(offset)));
    m_recvBuf.clear();
    if (m_socket->state() != QAbstractSocket::UnconnectedState)
        m_socket->abort();
}

void DataChannel::onKeepaliveTimer() {
    if (!isConnected()) return;

    sendMsg(MSG_PING);

    uint32_t elapsed = currentTimeMs() - m_lastRecvTime;
    if (elapsed > static_cast<uint32_t>(TCP_RELAY_DEAD_MS)) {
        LogManager::instance().logError(QString("[datachannel] No data for %1 ms, reconnecting").arg(elapsed));
        m_socket->abort();
    }
}

void DataChannel::scheduleReconnect() {
    if (m_reconnectTimer->isActive()) return;
    LogManager::instance().logDetail(QString("[datachannel] Will reconnect in %1 ms").arg(RECONNECT_INTERVAL_MS));
    m_reconnectTimer->start(RECONNECT_INTERVAL_MS);
}

void DataChannel::onReconnectTimer() {
    if (m_peerId == 0) return;
    if (m_socket->state() != QAbstractSocket::UnconnectedState) return;
    LogManager::instance().logDetail(QString("[datachannel] Attempting reconnect"));
    connectToServer(m_host, m_port, m_peerId);
}

void DataChannel::sendMsg(uint8_t msgType, const ByteBuffer& body) {
    if (m_socket->state() != QAbstractSocket::ConnectedState ||
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

void DataChannel::sendMsg(uint8_t msgType) {
    if (m_socket->state() != QAbstractSocket::ConnectedState) return;
    ByteBuffer empty;
    sendMsg(msgType, empty);
}

} // namespace VLan

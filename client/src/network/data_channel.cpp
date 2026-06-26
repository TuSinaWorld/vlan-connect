#include "data_channel.h"
#include "net_common.h"
#include "../ui/log_manager.h"

namespace VLan {

DataChannel::DataChannel(QObject* parent)
    : QObject(parent), m_port(0), m_peerId(0),
      m_established(false), m_lastRecvTime(0)
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
}

void DataChannel::connectToServer(const QString& host, quint16 port, uint32_t peerId) {
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
    if (m_socket->state() != QAbstractSocket::UnconnectedState)
        m_socket->abort();
}

bool DataChannel::isConnected() const {
    return m_established &&
           m_socket->state() == QAbstractSocket::ConnectedState;
}

void DataChannel::sendRelayData(uint32_t srcPeerId, uint32_t dstPeerId,
                                 const QByteArray& data)
{
    if (!isConnected()) return;
    if (g_verboseLog)
        LogManager::instance().logDetail(QString("[datachannel] sendRelayData src=%1 dst=%2 dataSize=%3").arg(srcPeerId).arg(dstPeerId).arg(data.size()));
    ByteBuffer bb;
    bb.writeU32(srcPeerId);
    bb.writeU32(dstPeerId);
    bb.writeBytes(data.constData(), data.size());
    sendMsg(MSG_TCP_RELAY_DATA, bb);
}

void DataChannel::onSocketConnected() {
    m_socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);

    ByteBuffer bb;
    bb.writeU32(m_peerId);
    sendMsg(MSG_DATA_CHANNEL_INIT, bb);

    LogManager::instance().logDetail(QString("[datachannel] TCP connected, sent DATA_CHANNEL_INIT for peer %1").arg(m_peerId));
}

void DataChannel::onSocketDisconnected() {
    m_keepaliveTimer->stop();
    bool wasEstablished = m_established;
    m_established = false;
    m_recvBuf.clear();

    if (wasEstablished) {
        LogManager::instance().logDetail(QString("[datachannel] Disconnected"));
        emit disconnected();
    }

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

    if (m_socket->state() != QAbstractSocket::ConnectedState)
        m_socket->abort();

    if (wasEstablished)
        emit disconnected();

    if (m_peerId != 0)
        scheduleReconnect();
}

void DataChannel::onReadyRead() {
    m_recvBuf.append(m_socket->readAll());
    m_lastRecvTime = currentTimeMs();

    while (m_recvBuf.size() >= static_cast<int>(sizeof(TcpMsgHeader))) {
        const TcpMsgHeader* hdr =
            reinterpret_cast<const TcpMsgHeader*>(m_recvBuf.constData());
        uint16_t payloadLen = ntohs(hdr->length);

        if (payloadLen > MAX_TCP_MSG_PAYLOAD) {
            LogManager::instance().logError(QString("[datachannel] Stream corrupted, reconnecting"));
            m_recvBuf.clear();
            m_socket->abort();
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

void DataChannel::processMessage(uint8_t msgType, const uint8_t* payload, size_t len) {
    switch (msgType) {
    case MSG_DATA_CHANNEL_ACK:
        m_established = true;
        m_lastRecvTime = currentTimeMs();
        m_keepaliveTimer->start(TCP_RELAY_KEEPALIVE_MS);
        LogManager::instance().logDetail(QString("[datachannel] Established for peer %1").arg(m_peerId));
        emit connected();
        break;

    case MSG_TCP_RELAY_DATA: {
        if (len < 8) break;
        ByteBuffer bb(payload, len);
        uint32_t srcId = bb.readU32();
        bb.readU32();
        size_t remaining = bb.remaining();
        QByteArray data(reinterpret_cast<const char*>(payload + len - remaining),
                        static_cast<int>(remaining));
        if (g_verboseLog)
            LogManager::instance().logDetail(QString("[datachannel] relayData srcPeerId=%1 dataSize=%2").arg(srcId).arg(data.size()));
        emit relayDataReceived(srcId, data);
        break;
    }
    case MSG_PONG:
        break;

    default:
        LogManager::instance().logDetail(QString("[datachannel] Unknown msg 0x%1").arg(msgType, 0, 16));
        break;
    }
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
    if (m_socket->state() != QAbstractSocket::ConnectedState) return;
    TcpMsgHeader hdr;
    hdr.msgType = msgType;
    hdr.length  = htons(static_cast<uint16_t>(body.size()));
    m_socket->write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    if (body.size() > 0)
        m_socket->write(reinterpret_cast<const char*>(body.data()), body.size());
}

void DataChannel::sendMsg(uint8_t msgType) {
    if (m_socket->state() != QAbstractSocket::ConnectedState) return;
    TcpMsgHeader hdr;
    hdr.msgType = msgType;
    hdr.length  = 0;
    m_socket->write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
}

} // namespace VLan

#ifndef VLAN_DATA_CHANNEL_H
#define VLAN_DATA_CHANNEL_H

#include <QObject>
#include <QTcpSocket>
#include <QTimer>
#include <QByteArray>
#include "protocol.h"
#include "byte_buffer.h"

namespace VLan {

class DataChannel : public QObject {
    Q_OBJECT
public:
    explicit DataChannel(QObject* parent = nullptr);
    ~DataChannel();

    void connectToServer(const QString& host, quint16 port, uint32_t peerId);
    void disconnect();
    bool isConnected() const;

    void sendRelayData(uint32_t srcPeerId, uint32_t dstPeerId, const QByteArray& data);

signals:
    void connected();
    void disconnected();
    void relayDataReceived(uint32_t srcPeerId, QByteArray data);

private slots:
    void onSocketConnected();
    void onSocketDisconnected();
    void onReadyRead();
    void onSocketError(QAbstractSocket::SocketError err);
    void onKeepaliveTimer();
    void onReconnectTimer();

private:
    void sendMsg(uint8_t msgType, const ByteBuffer& body);
    void sendMsg(uint8_t msgType);
    void processMessage(uint8_t msgType, const uint8_t* payload, size_t len);
    void scheduleReconnect();

    QTcpSocket* m_socket;
    QTimer*     m_keepaliveTimer;
    QTimer*     m_reconnectTimer;
    QByteArray  m_recvBuf;

    QString     m_host;
    quint16     m_port;
    uint32_t    m_peerId;
    bool        m_established;
    uint32_t    m_lastRecvTime;
};

} // namespace VLan
#endif // VLAN_DATA_CHANNEL_H

#ifndef VLAN_SIGNAL_CLIENT_H
#define VLAN_SIGNAL_CLIENT_H

#include <QObject>
#include <QTcpSocket>
#include <QTimer>
#include <QByteArray>
#include <QHostAddress>
#include "protocol.h"
#include "payload_cipher.h"
#include "byte_buffer.h"

namespace VLan {

/*
 * TCP connection to the signaling server.
 *
 * Handles login, room operations, and receives push notifications
 * (peer joined/left, punch notify, relay ready, etc.).
 */
class SignalClient : public QObject {
    Q_OBJECT
public:
    explicit SignalClient(QObject* parent = nullptr);
    ~SignalClient();

    void connectToServer(const QString& host, quint16 port,
                         int timeoutMs = 8000);
    void disconnect();
    bool isConnected() const;
    bool isConnecting() const;

    void login(const QString& name);
    void createRoom(const QString& roomName, uint8_t maxPlayers,
                    TransportMode mode = MODE_RELAY_KCP,
                    FecMode fecMode = FEC_NONE,
                    uint16_t mtu = ROOM_MTU_DEFAULT,
                    bool encrypted = false,
                    const QByteArray& passwordHash = QByteArray());
    void joinRoom(uint32_t roomId,
                  const QByteArray& authHash = QByteArray());
    void leaveRoom();
    void listRooms();
    void reportNatType(NatType type);
    void reportPunchResult(uint32_t targetPeerId, bool success);
    void requestRelay(uint32_t targetPeerId);

    uint32_t myPeerId() const { return m_myPeerId; }

signals:
    void connected();
    void disconnected();
    void connectFailed(QString reason);
    void loginResponse(uint32_t peerId);
    void roomCreated(uint32_t roomId, uint32_t virtualIP,
                     VLan::TransportMode transportMode,
                     VLan::FecMode fecMode,
                     uint16_t mtu,
                     bool encrypted, QByteArray salt,
                     QByteArray sessionSeed);
    void joinResponse(uint32_t roomId, uint32_t virtualIP,
                      VLan::TransportMode transportMode,
                      VLan::FecMode fecMode,
                      uint16_t mtu,
                      bool encrypted, QByteArray salt,
                      QByteArray sessionSeed,
                      QList<PeerInfo> members);
    void peerJoined(PeerInfo info);
    void peerLeft(uint32_t peerId);
    void roomList(QList<RoomListItem> rooms);
    void punchNotify(uint32_t peerId, uint32_t virtualIP,
                     NatType natType, uint32_t publicIP, uint16_t publicPort);
    void relayReady(uint32_t peerId);
    void serverError(QString message);
    void serverRttUpdated(int rttMs);

private slots:
    void onConnected();
    void onDisconnected();
    void onReadyRead();
    void onPingTimer();
    void onSocketError(QAbstractSocket::SocketError err);
    void onConnectTimeout();
    void onRecvTimeoutCheck();

private:
    void sendMsg(uint8_t msgType, const ByteBuffer& body);
    void sendMsg(uint8_t msgType);
    void processMessage(uint8_t msgType, const uint8_t* payload, size_t len);
    void handleStreamCorruption();

    QTcpSocket* m_socket;
    QTimer*     m_pingTimer;
    QTimer*     m_connectTimer;
    QTimer*     m_recvTimeoutTimer;
    QByteArray  m_recvBuf;
    uint32_t    m_myPeerId;
    QString     m_serverHost;
    quint16     m_serverPort;
    int         m_connectTimeoutMs;
    uint32_t    m_lastRecvTime;
    uint32_t    m_pingSentTime;
    QByteArray  m_pendingAuthHash;
};

} // namespace VLan
#endif // VLAN_SIGNAL_CLIENT_H

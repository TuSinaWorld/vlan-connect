#ifndef VLAN_SIGNAL_CLIENT_H
#define VLAN_SIGNAL_CLIENT_H

#include <QObject>
#include <QTcpSocket>
#include <QTimer>
#include <QByteArray>
#include <QHostAddress>
#include "protocol.h"
#include "payload_cipher.h"
#include "secure_frame.h"
#include "byte_buffer.h"

namespace VLan {

/*
 * TCP connection to the signaling server.
 *
 * Handles login, room operations, and receives push notifications
 * (peer joined/left, relay ready, etc.).
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
    bool secureEnabled() const { return m_secureReady; }
    uint32_t secureSessionId() const { return m_secureSessionId; }
    QByteArray secureMaster() const { return m_secureMaster; }

    void setServerPassword(const QString& password);
    void continueServerAuth();

    void login(const QString& name,
               bool hasResume = false,
               uint32_t resumeRoomId = 0,
               uint32_t resumePeerId = 0,
               const QByteArray& resumeToken = QByteArray());
    void createRoom(const QString& roomName, uint8_t maxPlayers,
                    RoomTrafficPolicy tcpPolicy = makeDefaultTcpPolicy(),
                    RoomTrafficPolicy udpPolicy = makeDefaultUdpPolicy(),
                    uint16_t mtu = ROOM_MTU_DEFAULT,
                    bool passwordProtected = false,
                    const QByteArray& passwordHash = QByteArray());
    void joinRoom(uint32_t roomId,
                  const QByteArray& authHash = QByteArray());
    void resumeRoom(uint32_t roomId, uint32_t peerId,
                    const QByteArray& resumeToken);
    void leaveRoom();
    void logout();
    void listRooms();
    void requestRelay(uint32_t targetPeerId);

    uint32_t myPeerId() const { return m_myPeerId; }

signals:
    void connected();
    void disconnected();
    void connectFailed(QString reason);
    void loginResponse(uint32_t peerId, bool resumeAccepted);
    void roomCreated(uint32_t roomId, uint32_t virtualIP,
                     VLan::RoomTrafficPolicy tcpPolicy,
                     VLan::RoomTrafficPolicy udpPolicy,
                     uint16_t mtu,
                     bool passwordProtected,
                     QByteArray leaseToken);
    void joinResponse(uint32_t roomId, uint32_t virtualIP,
                      VLan::RoomTrafficPolicy tcpPolicy,
                      VLan::RoomTrafficPolicy udpPolicy,
                      uint16_t mtu,
                      bool passwordProtected,
                      QList<PeerInfo> members,
                      QByteArray leaseToken);
    void logoutAck();
    void peerJoined(PeerInfo info);
    void peerResumed(PeerInfo info);
    void peerLeft(uint32_t peerId);
    void roomList(QList<RoomListItem> rooms);
    void relayReady(uint32_t peerId);
    void serverPasswordRequired();
    void secureSessionEstablished(uint32_t sessionId, QByteArray master);
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
    void sendClientHello();
    void sendServerAuth();
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
    QString     m_serverPassword;
    bool        m_serverAuthRequired;
    bool        m_secureReady;
    uint32_t    m_secureSessionId;
    uint8_t     m_clientNonce[16];
    uint8_t     m_serverNonce[16];
    uint8_t     m_clientPrivKey[32];
    uint8_t     m_clientPubKey[32];
    uint8_t     m_serverPubKey[32];
    QByteArray  m_secureMaster;
    SecureFrameCipher m_secureCipher;
};

} // namespace VLan
#endif // VLAN_SIGNAL_CLIENT_H

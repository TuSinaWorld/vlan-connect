#ifndef VLAN_ROOM_MANAGER_H
#define VLAN_ROOM_MANAGER_H

#include <QObject>
#include <QHostAddress>
#include <QHostInfo>
#include <QTimer>
#include "protocol.h"
#include "payload_cipher.h"

namespace VLan {

class TunAdapter;
class TunnelManager;
class SignalClient;
class DataChannel;
class NatDetector;
class HolePuncher;

/*
 * Orchestrates the full lifecycle:
 *   connect -> login -> create/join room -> NAT detect -> punch -> tunnel ready
 *
 * Glues SignalClient, TunnelManager, NatDetector, HolePuncher, and TunAdapter.
 */
class RoomManager : public QObject {
    Q_OBJECT
public:
    explicit RoomManager(QObject* parent = nullptr);
    ~RoomManager();

    void setServerAddress(const QString& host, quint16 port);
    void connectAndLogin(const QString& playerName);
    void disconnectFromServer();
    void setDefaultServerMode(bool isDefault) { m_isDefaultServer = isDefault; }
    TransportMode transportMode() const { return m_transportMode; }
    FecMode       fecMode()       const { return m_fecMode; }

    void createRoom(const QString& roomName, uint8_t maxPlayers = 8,
                    TransportMode mode = MODE_RELAY_KCP,
                    FecMode fecMode = FEC_NONE,
                    uint16_t mtu = ROOM_MTU_DEFAULT,
                    bool encrypted = false,
                    const QString& password = QString());
    void joinRoom(uint32_t roomId,
                  const QString& password = QString());
    void leaveRoom();
    void refreshRoomList();

    bool inRoom() const { return m_currentRoomId != 0; }
    uint32_t currentRoomId() const { return m_currentRoomId; }
    uint32_t myVirtualIP()   const { return m_myVirtualIP; }
    uint32_t myPeerId()      const;
    NatType  myNatType()     const { return m_myNatType; }
    uint16_t roomMtu()       const { return m_roomMtu; }

    SignalClient*  signalClient()  { return m_signal; }
    TunnelManager* tunnelManager() { return m_tunnel; }
    TunAdapter*    tunAdapter()    { return m_tun; }

signals:
    void connectionStatusChanged(bool connected);
    void loggedIn(uint32_t peerId);
    void roomCreated(uint32_t roomId);
    void roomJoined(uint32_t roomId);
    void roomLeft();
    void roomListUpdated(QList<VLan::RoomListItem> rooms);
    void peerConnected(uint32_t peerId, uint32_t virtualIP, QString name);
    void peerDisconnected(uint32_t peerId);
    void peerTransportChanged(uint32_t peerId, VLan::TransportType type);
    void peerLatencyUpdated(uint32_t peerId, int latencyMs);
    void natDetected(VLan::NatType type);
    void serverRttUpdated(int rttMs);
    void tunSpeedUpdated(quint64 uploadBytesPerSec, quint64 downloadBytesPerSec);
    void errorOccurred(QString message);
    void statusMessage(QString message);

private slots:
    void onSignalConnected();
    void onSignalDisconnected();
    void onLoginResponse(uint32_t peerId);
    void onRoomCreated(uint32_t roomId, uint32_t virtualIP,
                       VLan::TransportMode transportMode,
                       VLan::FecMode fecMode,
                       uint16_t mtu,
                       bool encrypted, QByteArray salt,
                       QByteArray sessionSeed);
    void onJoinResponse(uint32_t roomId, uint32_t virtualIP,
                        VLan::TransportMode transportMode,
                        VLan::FecMode fecMode,
                        uint16_t mtu,
                        bool encrypted, QByteArray salt,
                        QByteArray sessionSeed,
                        QList<VLan::PeerInfo> members);
    void onPeerJoined(VLan::PeerInfo info);
    void onPeerLeft(uint32_t peerId);
    void onPunchNotify(uint32_t peerId, uint32_t virtualIP,
                       VLan::NatType natType, uint32_t publicIP, uint16_t publicPort);
    void onRelayReady(uint32_t peerId);
    void onNatDetected(VLan::NatType type, uint32_t publicIP, uint16_t publicPort);
    void onPunchSucceeded(uint32_t peerId, QHostAddress addr, quint16 port);
    void onPunchFailed(uint32_t peerId);
    void onHostResolved(const QHostInfo& hostInfo);

private slots:
    void onTcpRelayHealthCheck();
    void onLatencyUpdate();
    void onTrafficUpdate();
    void onDataChannelConnected();
    void onDataChannelDisconnected();
    void onDataChannelRelayReceived(uint32_t srcPeerId, QByteArray data);
    void onLatencyPongReply(uint32_t peerId, QByteArray pongData);

private:
    void resolveAndConnect();
    void proceedWithConnection();
    void setupTun();
    void teardownTun();
    void initiatePunch(uint32_t peerId, uint32_t publicIP, uint16_t publicPort);
    void setupRelayTunnel(uint32_t peerId);
    void setupRawUdpRelayTunnel(uint32_t peerId);
    void setupTcpRelayTunnel(uint32_t peerId);
    void handleTcpRelayReceived(uint32_t srcPeerId, QByteArray data);
    void onTunnelDead(uint32_t peerId);
    void handleReconnectRoomList(QList<VLan::RoomListItem> rooms);
    void scheduleReconnectAttempt();

    SignalClient*  m_signal;
    DataChannel*   m_dataChannel;
    TunnelManager* m_tunnel;
    TunAdapter*    m_tun;
    NatDetector*   m_natDetector;
    HolePuncher*   m_puncher;
    QMetaObject::Connection m_rawUdpConnection;

    QString      m_serverHost;
    QHostAddress m_resolvedAddr;
    quint16  m_port;
    bool     m_isDefaultServer;
    QString  m_playerName;

    QTimer*       m_tcpRelayTimer;
    QTimer*       m_latencyTimer;
    QTimer*       m_trafficTimer;
    quint64       m_lastUploadBytes;
    quint64       m_lastDownloadBytes;

    uint32_t      m_currentRoomId;
    uint32_t      m_myVirtualIP;
    NatType       m_myNatType;
    TransportMode m_transportMode;
    FecMode       m_fecMode;
    uint16_t      m_roomMtu;
    bool          m_encrypted;
    QString       m_roomPassword;
    QByteArray    m_intermediate;
    PayloadCipher* m_cipher;

    static const int MAX_RECONNECT_ATTEMPTS = 3;
    bool          m_wantReconnect;
    int           m_reconnectAttempts;
    bool          m_wasInRoom;
    QString       m_savedRoomName;
    uint8_t       m_savedMaxPlayers;
    TransportMode m_savedTransportMode;
    FecMode       m_savedFecMode;
    uint16_t      m_savedRoomMtu;
    bool          m_savedEncrypted;
    QString       m_savedRoomPassword;
    QList<RoomListItem> m_cachedRoomList;
};

} // namespace VLan
#endif // VLAN_ROOM_MANAGER_H

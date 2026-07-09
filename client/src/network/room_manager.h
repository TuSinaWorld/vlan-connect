#ifndef VLAN_ROOM_MANAGER_H
#define VLAN_ROOM_MANAGER_H

#include <QObject>
#include <QByteArray>
#include <QHostAddress>
#include <QHostInfo>
#include <QMap>
#include <QTimer>
#include "protocol.h"

namespace VLan {

class TunAdapter;
class TunnelManager;
class SignalClient;
class DataChannel;

/*
 * Orchestrates the full lifecycle:
 *   connect -> login -> create/join room -> relay tunnel ready
 *
 * Glues SignalClient, TunnelManager, DataChannel, and TunAdapter.
 */
class RoomManager : public QObject {
    Q_OBJECT
public:
    explicit RoomManager(QObject* parent = nullptr);
    ~RoomManager();

    void setServerAddress(const QString& host, quint16 port);
    void setServerPassword(const QString& password);
    void continueServerAuth();
    void connectAndLogin(const QString& playerName);
    void disconnectFromServer();
    RoomTrafficPolicy tcpPolicy() const { return m_tcpPolicy; }
    RoomTrafficPolicy udpPolicy() const { return m_udpPolicy; }
    FecMode       fecMode()       const { return m_udpPolicy.fecMode; }

    void createRoom(const QString& roomName, uint8_t maxPlayers = 8,
                    RoomTrafficPolicy tcpPolicy = makeDefaultTcpPolicy(),
                    RoomTrafficPolicy udpPolicy = makeDefaultUdpPolicy(),
                    uint16_t mtu = ROOM_MTU_DEFAULT,
                    bool passwordProtected = false,
                    const QString& password = QString());
    void joinRoom(uint32_t roomId,
                  const QString& password = QString());
    void leaveRoom();
    void refreshRoomList();

    bool inRoom() const { return m_currentRoomId != 0; }
    uint32_t currentRoomId() const { return m_currentRoomId; }
    uint32_t myVirtualIP()   const { return m_myVirtualIP; }
    uint32_t myPeerId()      const;
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
    void peerTransportChanged(uint32_t peerId, VLan::TrafficClass cls, VLan::TransportType type);
    void peerLatencyUpdated(uint32_t peerId, VLan::TrafficClass cls, int latencyMs);
    void serverRttUpdated(int rttMs);
    void tunSpeedUpdated(quint64 uploadBytesPerSec, quint64 downloadBytesPerSec);
    void errorOccurred(QString message);
    void statusMessage(QString message);
    void serverPasswordRequired();

private slots:
    void onSignalConnected();
    void onSignalDisconnected();
    void onLoginResponse(uint32_t peerId, bool resumeAccepted);
    void onRoomCreated(uint32_t roomId, uint32_t virtualIP,
                       VLan::RoomTrafficPolicy tcpPolicy,
                       VLan::RoomTrafficPolicy udpPolicy,
                       uint16_t mtu,
                       bool passwordProtected,
                       QByteArray leaseToken);
    void onJoinResponse(uint32_t roomId, uint32_t virtualIP,
                        VLan::RoomTrafficPolicy tcpPolicy,
                        VLan::RoomTrafficPolicy udpPolicy,
                        uint16_t mtu,
                        bool passwordProtected,
                        QList<VLan::PeerInfo> members,
                        QByteArray leaseToken);
    void onPeerJoined(VLan::PeerInfo info);
    void onPeerResumed(VLan::PeerInfo info);
    void onPeerLeft(uint32_t peerId);
    void onRelayReady(uint32_t peerId);
    void onLogoutAck();
    void onSecureSessionEstablished(uint32_t sessionId, QByteArray master);
    void onHostResolved(const QHostInfo& hostInfo);

private slots:
    void onTcpRelayHealthCheck();
    void onLatencyUpdate();
    void onTrafficUpdate();
    void onRoomListRefreshTimer();
    void onDataChannelConnected();
    void onDataChannelDisconnected();
    void onDataChannelRelayReceived(uint32_t srcPeerId, VLan::TrafficClass cls, QByteArray data);

private:
    void resolveAndConnect();
    void proceedWithConnection();
    void setupTun();
    void teardownTun();
    void setupRelayTunnel(uint32_t peerId, TrafficClass cls);
    void setupRawUdpRelayTunnel(uint32_t peerId, TrafficClass cls);
    void setupTcpRelayTunnel(uint32_t peerId, TrafficClass cls);
    void setupPolicyTunnel(uint32_t peerId, TrafficClass cls);
    void handleTcpRelayReceived(uint32_t srcPeerId, TrafficClass cls, QByteArray data);
    void onTransportDead(uint32_t peerId, TrafficClass cls);
    void handleReconnectRoomList(QList<VLan::RoomListItem> rooms);
    void scheduleReconnectAttempt();
    void finishManualDisconnect();
    void rebuildPeerTransports(uint32_t peerId);
    void startResumeLeaseDeadline();
    void expireResumeLeaseIfNeeded();
    bool hasUsableResumeLease();
    void rememberResumeLease(uint32_t roomId, uint32_t peerId,
                             uint32_t virtualIP, const QByteArray& token);
    void clearResumeLease();

    struct TransportKey {
        uint32_t peerId;
        TrafficClass cls;
        bool operator<(const TransportKey& o) const {
            return (peerId < o.peerId) ||
                   (peerId == o.peerId && static_cast<int>(cls) < static_cast<int>(o.cls));
        }
    };
    void clearPendingRebuild(uint32_t peerId);
    bool takePendingRebuild(uint32_t peerId, TrafficClass cls);

    SignalClient*  m_signal;
    DataChannel*   m_dataChannel;
    TunnelManager* m_tunnel;
    TunAdapter*    m_tun;

    QString      m_serverHost;
    QHostAddress m_resolvedAddr;
    quint16  m_port;
    QString  m_playerName;
    QString  m_serverPassword;

    QTimer*       m_tcpRelayTimer;
    QTimer*       m_latencyTimer;
    QTimer*       m_trafficTimer;
    QTimer*       m_roomListTimer;
    quint64       m_lastUploadBytes;
    quint64       m_lastDownloadBytes;

    uint32_t      m_currentRoomId;
    uint32_t      m_myVirtualIP;
    RoomTrafficPolicy m_tcpPolicy;
    RoomTrafficPolicy m_udpPolicy;
    uint16_t      m_roomMtu;
    bool          m_roomPasswordProtected;
    QString       m_roomPassword;

    static const int MAX_RECONNECT_ATTEMPTS = 3;
    bool          m_wantReconnect;
    int           m_reconnectAttempts;
    bool          m_wasInRoom;
    bool          m_pendingResumeRoom;
    bool          m_manualDisconnecting;
    bool          m_logoutPending;
    bool          m_hasResumeLease;
    uint32_t      m_resumeRoomId;
    uint32_t      m_resumePeerId;
    uint32_t      m_resumeVirtualIP;
    uint32_t      m_resumeLeaseDeadlineMs;
    QByteArray    m_resumeToken;
    uint32_t      m_savedRoomId;
    QString       m_savedRoomName;
    uint8_t       m_savedMaxPlayers;
    uint16_t      m_savedRoomMtu;
    bool          m_savedRoomPasswordProtected;
    QString       m_savedRoomPassword;
    RoomTrafficPolicy m_savedTcpPolicy;
    RoomTrafficPolicy m_savedUdpPolicy;
    QList<RoomListItem> m_cachedRoomList;
    QMap<TransportKey, bool> m_pendingRebuild;
};

} // namespace VLan
#endif // VLAN_ROOM_MANAGER_H

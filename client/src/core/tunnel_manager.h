#ifndef VLAN_TUNNEL_MANAGER_H
#define VLAN_TUNNEL_MANAGER_H

#include <QObject>
#include <QUdpSocket>
#include <QTimer>
#include <QMap>
#include "protocol.h"
#include "secure_frame.h"

namespace VLan {

class TunAdapter;
class KcpTunnel;
class RawUdpTunnel;
class PeerConnection;

/*
 * Central routing engine.
 *
 * Connects the virtual TUN adapter to the network:
 *   TUN (game) <-> TunnelManager <-> KcpTunnel(s) <-> UDP socket <-> peers/relay
 *
 * Maintains a routing table: virtualIP -> PeerConnection.
 * Handles broadcast by forwarding to all peers.
 * Sends periodic UDP keepalives so relay sessions stay active.
 */
class TunnelManager : public QObject {
    Q_OBJECT
public:
    explicit TunnelManager(QObject* parent = nullptr);
    ~TunnelManager();

    void setTunAdapter(TunAdapter* tun);
    void setServerEndpoint(const QHostAddress& addr, quint16 udpPort);

    QUdpSocket* udpSocket() { return m_udpSocket; }
    quint16     localUdpPort() const;

    PeerConnection* addPeer(uint32_t peerId, uint32_t virtualIP, const QString& name);
    void     removePeer(uint32_t peerId);
    void     removeTransport(uint32_t peerId, TrafficClass cls);
    void     removeAllPeers();
    PeerConnection* peerById(uint32_t peerId);
    PeerConnection* peerByVirtualIP(uint32_t vip);
    QList<PeerConnection*> allPeers() const { return m_peerById.values(); }

    KcpTunnel* createKcpTunnel(PeerConnection* peer,
                               const QHostAddress& addr, quint16 port,
                               TransportType type,
                               FecMode fecMode = FEC_NONE,
                               uint16_t mtu = ROOM_MTU_DEFAULT,
                               KcpProfile profile = KCP_PROFILE_REALTIME,
                               TrafficClass trafficClass = TRAFFIC_UDP);

    RawUdpTunnel* createRawUdpTunnel(PeerConnection* peer,
                                     const QHostAddress& addr, quint16 port,
                                     FecMode fecMode = FEC_NONE,
                                     uint16_t mtu = ROOM_MTU_DEFAULT,
                                     TrafficClass trafficClass = TRAFFIC_UDP);

    void setMyPeerId(uint32_t id)   { m_myPeerId = id; }
    void setMyVirtualIP(uint32_t ip) { m_myVirtualIP = ip; }
    void setRoomMtu(uint16_t mtu) { m_roomMtu = normalizeRoomMtu(mtu); }
    bool installSecureSession(uint32_t sessionId, const QByteArray& master);
    void clearSecurityContext();
    bool startDataPlane();
    void stopDataPlane();
    DataPlaneState dataPlaneState() const { return m_dataPlaneState; }
    DataPlaneSecurityMode securityMode() const { return m_securityMode; }
    void resetTrafficCounters();
    void trafficCounters(quint64* uploadBytes, quint64* downloadBytes) const;
    void addTunDownloadBytes(quint64 bytes);

signals:
    void peerDataReceived(uint32_t peerId, QByteArray data);
    void transportDead(uint32_t peerId, VLan::TrafficClass cls);

private slots:
    void onTunPacketReceived(QByteArray packet);
    void onUdpReadyRead();
    void onKcpUpdate();
    void onPeerDataReceived(uint32_t peerId, QByteArray ipPacket);
    void onUdpKeepalive();

private:
    void routeFromTun(const QByteArray& ipPacket);
    void routeToTun(uint32_t peerId, const QByteArray& ipPacket);
    void sendUdpDatagram(const QByteArray& datagram,
                         const QHostAddress& addr, quint16 port);
    void markTransportDead(uint32_t peerId, TrafficClass cls);
    void flushPendingTransportDead();

    TunAdapter*             m_tun;
    QUdpSocket*             m_udpSocket;
    QTimer*                 m_kcpTimer;
    QTimer*                 m_udpKeepaliveTimer;

    QMap<uint32_t, PeerConnection*> m_peerById;
    QMap<uint32_t, PeerConnection*> m_peerByVIP;

    struct EndpointKey {
        quint32 ip; quint16 port;
        bool operator<(const EndpointKey& o) const {
            return (ip < o.ip) || (ip == o.ip && port < o.port);
        }
    };
    QMap<EndpointKey, KcpTunnel*> m_endpointToKcp;

    struct TransportKey {
        uint32_t peerId;
        TrafficClass cls;
        bool operator<(const TransportKey& o) const {
            return (peerId < o.peerId) ||
                   (peerId == o.peerId && static_cast<int>(cls) < static_cast<int>(o.cls));
        }
    };
    QMap<TransportKey, bool> m_pendingTransportDead;

    QHostAddress m_serverAddr;
    quint16      m_serverUdpPort;
    uint32_t     m_myPeerId;
    uint32_t     m_myVirtualIP;
    uint16_t     m_roomMtu;
    quint64      m_tunUploadBytes;
    quint64      m_tunDownloadBytes;
    quint64      m_tunGeneration;
    DataPlaneState m_dataPlaneState;
    DataPlaneSecurityMode m_securityMode;
    uint32_t     m_secureSessionId;
    QByteArray   m_secureMaster;
    SecureFrameCipher m_udpCipher;
};

} // namespace VLan
#endif // VLAN_TUNNEL_MANAGER_H

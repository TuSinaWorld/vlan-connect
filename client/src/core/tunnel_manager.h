#ifndef VLAN_TUNNEL_MANAGER_H
#define VLAN_TUNNEL_MANAGER_H

#include <QObject>
#include <QUdpSocket>
#include <QTimer>
#include <QMap>
#include "protocol.h"

namespace VLan {

class TunAdapter;
class KcpTunnel;
class RawUdpTunnel;
class P2PPeer;

/*
 * Central routing engine.
 *
 * Connects the virtual TUN adapter to the network:
 *   TUN (game) <-> TunnelManager <-> KcpTunnel(s) <-> UDP socket <-> peers/relay
 *
 * Maintains a routing table: virtualIP -> P2PPeer.
 * Handles broadcast by forwarding to all peers.
 * Sends periodic UDP keepalives to keep NAT mappings alive.
 */
class TunnelManager : public QObject {
    Q_OBJECT
public:
    explicit TunnelManager(QObject* parent = nullptr);
    ~TunnelManager();

    void setTunAdapter(TunAdapter* tun);
    void setServerEndpoint(const QHostAddress& addr, quint16 stunPort);

    QUdpSocket* udpSocket() { return m_udpSocket; }
    quint16     localUdpPort() const;

    P2PPeer* addPeer(uint32_t peerId, uint32_t virtualIP, const QString& name);
    void     removePeer(uint32_t peerId);
    void     removeAllPeers();
    P2PPeer* peerById(uint32_t peerId);
    P2PPeer* peerByVirtualIP(uint32_t vip);
    QList<P2PPeer*> allPeers() const { return m_peerById.values(); }

    KcpTunnel* createKcpTunnel(P2PPeer* peer,
                               const QHostAddress& addr, quint16 port,
                               TransportType type,
                               FecMode fecMode = FEC_NONE,
                               uint16_t mtu = ROOM_MTU_DEFAULT);

    RawUdpTunnel* createRawUdpTunnel(P2PPeer* peer,
                                     const QHostAddress& addr, quint16 port,
                                     FecMode fecMode = FEC_NONE,
                                     uint16_t mtu = ROOM_MTU_DEFAULT);

    void setMyPeerId(uint32_t id)   { m_myPeerId = id; }
    void setMyVirtualIP(uint32_t ip) { m_myVirtualIP = ip; }
    void resetTrafficCounters();
    void trafficCounters(quint64* uploadBytes, quint64* downloadBytes) const;
    void addTunDownloadBytes(quint64 bytes);

signals:
    void peerDataReceived(uint32_t peerId, QByteArray data);
    void rawUdpReceived(QByteArray data, QHostAddress senderAddr, quint16 senderPort);
    void tunnelDead(uint32_t peerId);

private slots:
    void onTunPacketReceived(QByteArray packet);
    void onUdpReadyRead();
    void onKcpUpdate();
    void onPeerDataReceived(QByteArray ipPacket);
    void onUdpKeepalive();

private:
    void routeFromTun(const QByteArray& ipPacket);
    void routeToTun(const QByteArray& ipPacket);

    TunAdapter*             m_tun;
    QUdpSocket*             m_udpSocket;
    QTimer*                 m_kcpTimer;
    QTimer*                 m_udpKeepaliveTimer;

    QMap<uint32_t, P2PPeer*> m_peerById;
    QMap<uint32_t, P2PPeer*> m_peerByVIP;

    struct EndpointKey {
        quint32 ip; quint16 port;
        bool operator<(const EndpointKey& o) const {
            return (ip < o.ip) || (ip == o.ip && port < o.port);
        }
    };
    QMap<EndpointKey, KcpTunnel*> m_endpointToKcp;

    QHostAddress m_serverAddr;
    quint16      m_serverStunPort;
    uint32_t     m_myPeerId;
    uint32_t     m_myVirtualIP;
    quint64      m_tunUploadBytes;
    quint64      m_tunDownloadBytes;
};

} // namespace VLan
#endif // VLAN_TUNNEL_MANAGER_H

#ifndef VLAN_CLI_TUNNEL_H
#define VLAN_CLI_TUNNEL_H

#include "cli_common.h"
#include "cli_peer.h"
#include "cli_tun.h"
#include <map>
#include <set>

namespace VLan {

class CliTunnelManager {
public:
    CliTunnelManager();
    ~CliTunnelManager();

    bool initUdpSocket();
    void setTunAdapter(CliTunAdapter* tun) { m_tun = tun; }
    void setServerEndpoint(uint32_t ip, uint16_t port);

    socket_t udpFd() const { return m_udpFd; }
    uint16_t localUdpPort() const { return m_localUdpPort; }

    CliPeerConnection* addPeer(uint32_t peerId, uint32_t virtualIP, const std::string& name);
    void removePeer(uint32_t peerId);
    void removeTransport(uint32_t peerId, TrafficClass cls);
    void removeAllPeers();
    CliPeerConnection* peerById(uint32_t peerId);
    CliPeerConnection* peerByVirtualIP(uint32_t vip);
    std::vector<CliPeerConnection*> allPeers() const;

    CliKcpTunnel* createKcpTunnel(CliPeerConnection* peer,
                                  uint32_t addr, uint16_t port,
                                  TransportType type,
                                  FecMode fecMode = FEC_NONE,
                                  uint16_t mtu = ROOM_MTU_DEFAULT,
                                  KcpProfile profile = KCP_PROFILE_REALTIME,
                                  TrafficClass trafficClass = TRAFFIC_UDP);
    CliRawUdpTunnel* createRawUdpTunnel(CliPeerConnection* peer,
                                        uint32_t addr, uint16_t port,
                                        FecMode fecMode = FEC_NONE,
                                        uint16_t mtu = ROOM_MTU_DEFAULT,
                                        TrafficClass trafficClass = TRAFFIC_UDP);

    void setMyPeerId(uint32_t id)    { m_myPeerId = id; }
    void setMyVirtualIP(uint32_t ip) { m_myVirtualIP = ip; }
    void setRoomMtu(uint16_t mtu) { m_roomMtu = normalizeRoomMtu(mtu); }
    bool installSecureSession(uint32_t sessionId, const Buffer& master);
    void clearSecurityContext();
    bool startDataPlane();
    void stopDataPlane();
    DataPlaneState dataPlaneState() const { return m_dataPlaneState; }
    DataPlaneSecurityMode securityMode() const { return m_securityMode; }

    void onUdpReadable();
    void processTunPackets();
    void updateKcp();
    void sendUdpKeepalive();

    PeerDataCallback      onPeerData;
    TunnelDeadCallback    onTunnelDead;

private:
    static uint64_t tunnelKey(uint32_t peerId, TrafficClass cls);
    void markTransportDead(uint32_t peerId, TrafficClass cls);
    void flushPendingTransportDead();
    void udpSend(const uint8_t* data, size_t len, uint32_t dstIP, uint16_t dstPort);
    void routeFromTun(const Buffer& ipPacket);
    void routeToTun(uint32_t peerId, const Buffer& ipPacket);
    void onPeerDataReceived(uint32_t peerId, const Buffer& ipPacket);

    CliTunAdapter*  m_tun;
    socket_t        m_udpFd;
    uint16_t        m_localUdpPort;

    std::map<uint32_t, CliPeerConnection*> m_peerById;
    std::map<uint32_t, CliPeerConnection*> m_peerByVIP;
    std::map<uint64_t, CliKcpTunnel*>      m_kcpByPeerClass;
    std::map<uint64_t, CliRawUdpTunnel*>   m_rawByPeerClass;
    std::set<uint64_t>                     m_pendingTransportDead;

    uint32_t m_serverIP;
    uint16_t m_serverPort;
    uint32_t m_myPeerId;
    uint32_t m_myVirtualIP;
    uint16_t m_roomMtu;
    DataPlaneState m_dataPlaneState;
    DataPlaneSecurityMode m_securityMode;
    uint32_t m_secureSessionId;
    Buffer   m_secureMaster;
    SecureFrameCipher m_udpCipher;
};

} // namespace VLan
#endif // VLAN_CLI_TUNNEL_H

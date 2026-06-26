#ifndef VLAN_CLI_TUNNEL_H
#define VLAN_CLI_TUNNEL_H

#include "cli_common.h"
#include "cli_peer.h"
#include "cli_tun.h"
#include <map>

namespace VLan {

/*
 * Central routing engine (no Qt).
 *
 * TUN (game) <-> CliTunnelManager <-> KCP/RawUDP tunnels <-> UDP socket <-> peers/relay
 */
class CliTunnelManager {
public:
    CliTunnelManager();
    ~CliTunnelManager();

    bool initUdpSocket();
    void setTunAdapter(CliTunAdapter* tun) { m_tun = tun; }
    void setServerEndpoint(uint32_t ip, uint16_t port);

    socket_t udpFd() const { return m_udpFd; }
    uint16_t localUdpPort() const { return m_localUdpPort; }

    CliP2PPeer* addPeer(uint32_t peerId, uint32_t virtualIP, const std::string& name);
    void removePeer(uint32_t peerId);
    void removeAllPeers();
    CliP2PPeer* peerById(uint32_t peerId);
    CliP2PPeer* peerByVirtualIP(uint32_t vip);
    std::vector<CliP2PPeer*> allPeers() const;

    CliKcpTunnel* createKcpTunnel(CliP2PPeer* peer,
                                  uint32_t addr, uint16_t port,
                                  TransportType type,
                                  FecMode fecMode = FEC_NONE,
                                  uint16_t mtu = ROOM_MTU_DEFAULT);
    CliRawUdpTunnel* createRawUdpTunnel(CliP2PPeer* peer,
                                        uint32_t addr, uint16_t port,
                                        FecMode fecMode = FEC_NONE,
                                        uint16_t mtu = ROOM_MTU_DEFAULT);

    void setMyPeerId(uint32_t id)    { m_myPeerId = id; }
    void setMyVirtualIP(uint32_t ip) { m_myVirtualIP = ip; }

    void onUdpReadable();
    void processTunPackets();
    void updateKcp();
    void sendUdpKeepalive();

    PeerDataCallback      onPeerData;
    TunnelDeadCallback    onTunnelDead;

private:
    void udpSend(const uint8_t* data, size_t len, uint32_t dstIP, uint16_t dstPort);
    void routeFromTun(const Buffer& ipPacket);
    void routeToTun(const Buffer& ipPacket);
    void onPeerDataReceived(uint32_t peerId, const Buffer& ipPacket);

    CliTunAdapter*  m_tun;
    socket_t        m_udpFd;
    uint16_t        m_localUdpPort;

    std::map<uint32_t, CliP2PPeer*>    m_peerById;
    std::map<uint32_t, CliP2PPeer*>    m_peerByVIP;
    std::map<uint32_t, CliKcpTunnel*>  m_kcpByPeer;
    std::map<uint32_t, CliRawUdpTunnel*> m_rawByPeer;

    uint32_t m_serverIP;
    uint16_t m_serverPort;
    uint32_t m_myPeerId;
    uint32_t m_myVirtualIP;

public:
    /* Raw UDP callback for STUN/punch packets */
    std::function<void(const uint8_t* data, size_t len,
                       uint32_t senderIP, uint16_t senderPort)> onRawUdpPacket;
};

} // namespace VLan
#endif // VLAN_CLI_TUNNEL_H

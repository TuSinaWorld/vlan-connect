#include "cli_tunnel.h"
#include "cli_log.h"
#include <cstring>

namespace VLan {

CliTunnelManager::CliTunnelManager()
    : m_tun(nullptr), m_udpFd(SOCK_INVALID), m_localUdpPort(0),
      m_serverIP(0), m_serverPort(0), m_myPeerId(0), m_myVirtualIP(0)
{}

CliTunnelManager::~CliTunnelManager() {
    removeAllPeers();
    if (m_udpFd != SOCK_INVALID) {
        sock_close(m_udpFd);
        m_udpFd = SOCK_INVALID;
    }
}

bool CliTunnelManager::initUdpSocket() {
    m_udpFd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_udpFd == SOCK_INVALID) {
        LOG_ERR("Failed to create UDP socket");
        return false;
    }
    setNonBlocking(m_udpFd);

    struct sockaddr_in bindAddr;
    memset(&bindAddr, 0, sizeof(bindAddr));
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_port = 0;
    bindAddr.sin_addr.s_addr = INADDR_ANY;
    if (bind(m_udpFd, (struct sockaddr*)&bindAddr, sizeof(bindAddr)) < 0) {
        LOG_ERR("Failed to bind UDP socket");
        sock_close(m_udpFd);
        m_udpFd = SOCK_INVALID;
        return false;
    }

    struct sockaddr_in localAddr;
    socklen_t addrLen = sizeof(localAddr);
    getsockname(m_udpFd, (struct sockaddr*)&localAddr, &addrLen);
    m_localUdpPort = ntohs(localAddr.sin_port);
    LOG_INFO("UDP socket bound to port %u", m_localUdpPort);
    return true;
}

void CliTunnelManager::setServerEndpoint(uint32_t ip, uint16_t port) {
    m_serverIP   = ip;
    m_serverPort = port;
}

void CliTunnelManager::udpSend(const uint8_t* data, size_t len,
                                uint32_t dstIP, uint16_t dstPort) {
    struct sockaddr_in addr = makeAddr(dstIP, dstPort);
    sendto(m_udpFd, reinterpret_cast<const char*>(data), static_cast<int>(len), 0,
           (struct sockaddr*)&addr, sizeof(addr));
}

// ───────── Peer management ─────────

CliP2PPeer* CliTunnelManager::addPeer(uint32_t peerId, uint32_t virtualIP,
                                       const std::string& name) {
    if (m_peerById.count(peerId)) return m_peerById[peerId];

    CliP2PPeer* peer = new CliP2PPeer(peerId, virtualIP, name);
    m_peerById[peerId]     = peer;
    m_peerByVIP[virtualIP] = peer;

    peer->setOnDataReceived([this](uint32_t pid, const Buffer& data) {
        onPeerDataReceived(pid, data);
    });
    LOG_DBG("[tunnel] Added peer %u vip %s %s", peerId,
            ipToString(virtualIP).c_str(), name.c_str());
    return peer;
}

void CliTunnelManager::removePeer(uint32_t peerId) {
    auto it = m_peerById.find(peerId);
    if (it == m_peerById.end()) return;

    CliP2PPeer* peer = it->second;
    m_peerByVIP.erase(peer->virtualIP());

    auto kit = m_kcpByPeer.find(peerId);
    if (kit != m_kcpByPeer.end()) {
        delete kit->second;
        m_kcpByPeer.erase(kit);
    }
    auto rit = m_rawByPeer.find(peerId);
    if (rit != m_rawByPeer.end()) {
        delete rit->second;
        m_rawByPeer.erase(rit);
    }

    m_peerById.erase(it);
    delete peer;
    LOG_DBG("[tunnel] Removed peer %u", peerId);
}

void CliTunnelManager::removeAllPeers() {
    for (auto& kv : m_kcpByPeer) delete kv.second;
    for (auto& kv : m_rawByPeer) delete kv.second;
    for (auto& kv : m_peerById)  delete kv.second;
    m_kcpByPeer.clear();
    m_rawByPeer.clear();
    m_peerById.clear();
    m_peerByVIP.clear();
}

CliP2PPeer* CliTunnelManager::peerById(uint32_t peerId) {
    auto it = m_peerById.find(peerId);
    return (it != m_peerById.end()) ? it->second : nullptr;
}

CliP2PPeer* CliTunnelManager::peerByVirtualIP(uint32_t vip) {
    auto it = m_peerByVIP.find(vip);
    return (it != m_peerByVIP.end()) ? it->second : nullptr;
}

std::vector<CliP2PPeer*> CliTunnelManager::allPeers() const {
    std::vector<CliP2PPeer*> result;
    for (auto& kv : m_peerById) result.push_back(kv.second);
    return result;
}

// ───────── Tunnel creation ─────────

CliKcpTunnel* CliTunnelManager::createKcpTunnel(CliP2PPeer* peer,
                                                 uint32_t addr, uint16_t port,
                                                 TransportType type,
                                                 FecMode fecMode,
                                                 uint16_t mtu) {
    uint32_t conv = m_myPeerId ^ peer->peerId();
    if (conv == 0) conv = 1;

    UdpSendFunc sender = [this](const uint8_t* data, size_t len,
                                uint32_t dstIP, uint16_t dstPort) {
        udpSend(data, len, dstIP, dstPort);
    };

    CliKcpTunnel* kcp = new CliKcpTunnel(conv, sender, addr, port, fecMode, mtu);
    peer->setKcpTunnel(kcp);
    peer->setTransport(type);
    m_kcpByPeer[peer->peerId()] = kcp;

    uint32_t peerId = peer->peerId();
    kcp->setOnDead([this, peerId]() {
        if (onTunnelDead) onTunnelDead(peerId);
    });

    LOG_INFO("KCP tunnel created for peer %u via %s:%u (%s %s mtu=%u)",
             peer->peerId(), ipToString(addr).c_str(), port,
             transportName(type), fecModeName(fecMode), normalizeRoomMtu(mtu));
    return kcp;
}

CliRawUdpTunnel* CliTunnelManager::createRawUdpTunnel(CliP2PPeer* peer,
                                                       uint32_t addr, uint16_t port,
                                                       FecMode fecMode,
                                                       uint16_t mtu) {
    UdpSendFunc sender = [this](const uint8_t* data, size_t len,
                                uint32_t dstIP, uint16_t dstPort) {
        udpSend(data, len, dstIP, dstPort);
    };

    CliRawUdpTunnel* tunnel = new CliRawUdpTunnel(sender, addr, port, fecMode, mtu);
    peer->setRawUdpTunnel(tunnel);
    peer->setTransport(TRANSPORT_RELAY_RAW_UDP);
    m_rawByPeer[peer->peerId()] = tunnel;

    uint32_t peerId = peer->peerId();
    tunnel->setOnDead([this, peerId]() {
        if (onTunnelDead) onTunnelDead(peerId);
    });

    LOG_INFO("Raw UDP tunnel created for peer %u via %s:%u %s mtu=%u",
             peer->peerId(), ipToString(addr).c_str(), port,
             fecModeName(fecMode), normalizeRoomMtu(mtu));
    return tunnel;
}

// ───────── UDP receive ─────────

void CliTunnelManager::onUdpReadable() {
    char buf[65536];
    struct sockaddr_in senderAddr;
    socklen_t addrLen = sizeof(senderAddr);

    for (;;) {
        int n = recvfrom(m_udpFd, buf, sizeof(buf), 0,
                         (struct sockaddr*)&senderAddr, &addrLen);
        if (n <= 0) break;

        uint8_t pktType = static_cast<uint8_t>(buf[0]);
        uint32_t senderIP = ntohl(senderAddr.sin_addr.s_addr);
        uint16_t senderPort = ntohs(senderAddr.sin_port);

        switch (pktType) {
        case UDP_KCP_DATA: {
            for (auto& kv : m_kcpByPeer) {
                CliKcpTunnel* kcp = kv.second;
                if (kcp->peerIP() == senderIP && kcp->peerPort() == senderPort) {
                    kcp->feedInput(buf + 1, n - 1);
                    break;
                }
            }
            break;
        }
        case UDP_RELAY_DATA: {
            if (n < static_cast<int>(sizeof(UdpRelayHeader))) break;
            const UdpRelayHeader* hdr = reinterpret_cast<const UdpRelayHeader*>(buf);
            uint32_t srcPeerId = ntohl(hdr->srcPeerId);
            auto it = m_kcpByPeer.find(srcPeerId);
            if (it != m_kcpByPeer.end()) {
                const char* kcpData = buf + sizeof(UdpRelayHeader);
                int kcpLen = n - sizeof(UdpRelayHeader);
                it->second->feedInput(kcpData, kcpLen);
            }
            break;
        }
        case UDP_RAW_RELAY_DATA: {
            if (n < static_cast<int>(sizeof(UdpRelayHeader) + sizeof(FragHeader))) break;
            const UdpRelayHeader* hdr = reinterpret_cast<const UdpRelayHeader*>(buf);
            uint32_t srcPeerId = ntohl(hdr->srcPeerId);
            auto it = m_rawByPeer.find(srcPeerId);
            if (it != m_rawByPeer.end()) {
                const char* fragData = buf + sizeof(UdpRelayHeader);
                int fragLen = n - sizeof(UdpRelayHeader);
                it->second->feedInput(fragData, fragLen);
            }
            break;
        }
        case UDP_STUN_RESPONSE:
        case UDP_PUNCH:
        case UDP_PUNCH_ACK:
            if (onRawUdpPacket)
                onRawUdpPacket(reinterpret_cast<const uint8_t*>(buf), n,
                               senderIP, senderPort);
            break;
        default:
            break;
        }
    }
}

// ───────── TUN packet processing ─────────

void CliTunnelManager::processTunPackets() {
    if (!m_tun) return;
    auto packets = m_tun->recvQueue().popAll();
    for (auto& pkt : packets)
        routeFromTun(pkt);
}

void CliTunnelManager::routeFromTun(const Buffer& ipPacket) {
    if (ipPacket.size() < 20) return;
    uint32_t dstIP = extractDstIP(ipPacket.data(), ipPacket.size());

    if (isBroadcast(dstIP) || dstIP == VNET_BROADCAST) {
        for (auto& kv : m_peerById)
            kv.second->sendData(ipPacket);
    } else {
        CliP2PPeer* peer = peerByVirtualIP(dstIP);
        if (peer) peer->sendData(ipPacket);
    }
}

void CliTunnelManager::routeToTun(const Buffer& ipPacket) {
    if (m_tun) m_tun->writePacket(ipPacket);
}

void CliTunnelManager::onPeerDataReceived(uint32_t, const Buffer& ipPacket) {
    routeToTun(ipPacket);
}

// ───────── KCP update ─────────

void CliTunnelManager::updateKcp() {
    for (auto& kv : m_kcpByPeer)
        kv.second->update();
    for (auto& kv : m_rawByPeer)
        kv.second->update();
}

// ───────── UDP keepalive ─────────

void CliTunnelManager::sendUdpKeepalive() {
    if (m_serverIP == 0 || m_serverPort == 0) return;
    UdpHeader hdr;
    hdr.type = UDP_KEEPALIVE;
    udpSend(reinterpret_cast<const uint8_t*>(&hdr), sizeof(hdr),
            m_serverIP, m_serverPort);
}

} // namespace VLan

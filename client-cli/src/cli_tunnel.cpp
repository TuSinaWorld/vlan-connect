#include "cli_tunnel.h"
#include "cli_log.h"
#include "overlay_packet_validator.h"
#include <cstring>
#include <vector>

namespace VLan {

CliTunnelManager::CliTunnelManager()
    : m_tun(nullptr), m_udpFd(SOCK_INVALID), m_localUdpPort(0),
      m_serverIP(0), m_serverPort(0), m_myPeerId(0), m_myVirtualIP(0),
      m_roomMtu(ROOM_MTU_DEFAULT),
      m_dataPlaneState(DataPlaneState::Stopped),
      m_securityMode(DataPlaneSecurityMode::Unconfigured),
      m_secureSessionId(0)
{}

CliTunnelManager::~CliTunnelManager() {
    stopDataPlane();
    removeAllPeers();
    clearSecurityContext();
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
    m_serverIP = ip;
    m_serverPort = port;
}

bool CliTunnelManager::installSecureSession(uint32_t sessionId, const Buffer& master) {
    if (!dataPlaneCanReconfigure(m_dataPlaneState)) {
        LOG_ERR("[tunnel] Refusing to change security while data plane is running");
        return false;
    }
    clearSecurityContext();
    if (sessionId == 0 || master.size() != SECURE_KEY_SIZE) {
        LOG_ERR("[tunnel] Invalid secure data-plane session");
        return false;
    }
    m_secureSessionId = sessionId;
    m_secureMaster = master;
    m_udpCipher.init(m_secureMaster.data(), true, "udp");
    m_securityMode = DataPlaneSecurityMode::Secure;
    return true;
}

void CliTunnelManager::clearSecurityContext() {
    if (!dataPlaneCanReconfigure(m_dataPlaneState)) {
        LOG_ERR("[tunnel] Refusing to clear security while data plane is running");
        return;
    }
    m_udpCipher.reset();
    m_secureSessionId = 0;
    if (!m_secureMaster.empty()) {
        crypto_wipe(m_secureMaster.data(), m_secureMaster.size());
        m_secureMaster.clear();
    }
    m_securityMode = DataPlaneSecurityMode::Unconfigured;
}

bool CliTunnelManager::startDataPlane() {
    if (m_dataPlaneState == DataPlaneState::Running)
        return true;
    if (!m_tun || !m_tun->isRunning() ||
        m_securityMode == DataPlaneSecurityMode::Unconfigured) {
        LOG_ERR("[tunnel] Cannot start unconfigured data plane");
        return false;
    }
    m_dataPlaneState = DataPlaneState::Running;
    return true;
}

void CliTunnelManager::stopDataPlane() {
    m_dataPlaneState = DataPlaneState::Stopped;
    if (m_tun)
        m_tun->recvQueue().popAll();
}

uint64_t CliTunnelManager::tunnelKey(uint32_t peerId, TrafficClass cls) {
    return (static_cast<uint64_t>(peerId) << 8) | static_cast<uint8_t>(cls);
}

void CliTunnelManager::udpSend(const uint8_t* data, size_t len,
                               uint32_t dstIP, uint16_t dstPort) {
    if (!dataPlaneAllowsTraffic(m_dataPlaneState, m_securityMode) ||
        m_udpFd == SOCK_INVALID) {
        return;
    }
    Buffer wrapped;
    if (m_securityMode == DataPlaneSecurityMode::Secure) {
        if (m_secureSessionId == 0 || m_secureMaster.size() != SECURE_KEY_SIZE)
            return;
        std::vector<uint8_t> enc = m_udpCipher.encrypt(data, len);
        wrapped.resize(1 + SECURE_SESSION_ID_SIZE + enc.size());
        wrapped[0] = UDP_ENCRYPTED;
        writeU32BE(wrapped.data() + 1, m_secureSessionId);
        if (!enc.empty())
            memcpy(wrapped.data() + 1 + SECURE_SESSION_ID_SIZE, enc.data(), enc.size());
        data = wrapped.data();
        len = wrapped.size();
    }
    struct sockaddr_in addr = makeAddr(dstIP, dstPort);
    sendto(m_udpFd, reinterpret_cast<const char*>(data), static_cast<int>(len), 0,
           (struct sockaddr*)&addr, sizeof(addr));
}

CliPeerConnection* CliTunnelManager::addPeer(uint32_t peerId, uint32_t virtualIP,
                                             const std::string& name) {
    if (m_peerById.count(peerId)) return m_peerById[peerId];

    CliPeerConnection* peer = new CliPeerConnection(peerId, virtualIP, name);
    m_peerById[peerId] = peer;
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

    CliPeerConnection* peer = it->second;
    for (auto pit = m_pendingTransportDead.begin(); pit != m_pendingTransportDead.end(); ) {
        if ((*pit >> 8) == peerId)
            pit = m_pendingTransportDead.erase(pit);
        else
            ++pit;
    }
    m_peerByVIP.erase(peer->virtualIP());

    for (int cls = TRAFFIC_TCP; cls <= TRAFFIC_UDP; ++cls) {
        uint64_t key = tunnelKey(peerId, static_cast<TrafficClass>(cls));
        auto kit = m_kcpByPeerClass.find(key);
        if (kit != m_kcpByPeerClass.end()) {
            delete kit->second;
            m_kcpByPeerClass.erase(kit);
        }
        auto rit = m_rawByPeerClass.find(key);
        if (rit != m_rawByPeerClass.end()) {
            delete rit->second;
            m_rawByPeerClass.erase(rit);
        }
    }

    m_peerById.erase(it);
    delete peer;
    LOG_DBG("[tunnel] Removed peer %u", peerId);
}

void CliTunnelManager::removeTransport(uint32_t peerId, TrafficClass cls) {
    CliPeerConnection* peer = peerById(peerId);
    if (!peer) return;

    int idx = static_cast<int>(cls);
    if (idx < 0 || idx > 2) cls = TRAFFIC_UDP;

    uint64_t key = tunnelKey(peerId, cls);
    m_pendingTransportDead.erase(key);

    auto kit = m_kcpByPeerClass.find(key);
    if (kit != m_kcpByPeerClass.end()) {
        peer->clearKcpTunnel(cls);
        delete kit->second;
        m_kcpByPeerClass.erase(kit);
    }

    auto rit = m_rawByPeerClass.find(key);
    if (rit != m_rawByPeerClass.end()) {
        peer->clearRawUdpTunnel(cls);
        delete rit->second;
        m_rawByPeerClass.erase(rit);
    }

    peer->clearTransport(cls);
    LOG_DBG("[tunnel] Removed transport peer=%u class=%u",
            peerId, static_cast<unsigned>(cls));
}

void CliTunnelManager::removeAllPeers() {
    m_pendingTransportDead.clear();
    for (auto& kv : m_kcpByPeerClass) delete kv.second;
    for (auto& kv : m_rawByPeerClass) delete kv.second;
    for (auto& kv : m_peerById) delete kv.second;
    m_kcpByPeerClass.clear();
    m_rawByPeerClass.clear();
    m_peerById.clear();
    m_peerByVIP.clear();
}

CliPeerConnection* CliTunnelManager::peerById(uint32_t peerId) {
    auto it = m_peerById.find(peerId);
    return (it != m_peerById.end()) ? it->second : nullptr;
}

CliPeerConnection* CliTunnelManager::peerByVirtualIP(uint32_t vip) {
    auto it = m_peerByVIP.find(vip);
    return (it != m_peerByVIP.end()) ? it->second : nullptr;
}

std::vector<CliPeerConnection*> CliTunnelManager::allPeers() const {
    std::vector<CliPeerConnection*> result;
    for (auto& kv : m_peerById) result.push_back(kv.second);
    return result;
}

CliKcpTunnel* CliTunnelManager::createKcpTunnel(CliPeerConnection* peer,
                                                uint32_t addr, uint16_t port,
                                                TransportType type,
                                                FecMode fecMode,
                                                uint16_t mtu,
                                                KcpProfile profile,
                                                TrafficClass trafficClass) {
    uint32_t conv = m_myPeerId ^ peer->peerId() ^ (static_cast<uint32_t>(trafficClass) << 24);
    if (conv == 0) conv = 1;

    UdpSendFunc sender = [this](const uint8_t* data, size_t len,
                                uint32_t dstIP, uint16_t dstPort) {
        udpSend(data, len, dstIP, dstPort);
    };

    CliKcpTunnel* kcp = new CliKcpTunnel(conv, sender, addr, port, fecMode,
                                         mtu, profile, trafficClass,
                                         m_securityMode == DataPlaneSecurityMode::Secure);
    peer->setKcpTunnel(trafficClass, kcp);
    peer->setTransport(trafficClass, type);
    m_kcpByPeerClass[tunnelKey(peer->peerId(), trafficClass)] = kcp;

    uint32_t peerId = peer->peerId();
    kcp->setOnDead([this, peerId, trafficClass]() {
        markTransportDead(peerId, trafficClass);
    });

    LOG_INFO("KCP tunnel created for peer %u class=%u via %s:%u (%s %s mtu=%u)",
             peer->peerId(), static_cast<unsigned>(trafficClass), ipToString(addr).c_str(), port,
             transportName(type), fecModeName(fecMode), normalizeRoomMtu(mtu));
    return kcp;
}

CliRawUdpTunnel* CliTunnelManager::createRawUdpTunnel(CliPeerConnection* peer,
                                                      uint32_t addr, uint16_t port,
                                                      FecMode fecMode,
                                                      uint16_t mtu,
                                                      TrafficClass trafficClass) {
    UdpSendFunc sender = [this](const uint8_t* data, size_t len,
                                uint32_t dstIP, uint16_t dstPort) {
        udpSend(data, len, dstIP, dstPort);
    };

    CliRawUdpTunnel* tunnel = new CliRawUdpTunnel(sender, addr, port, fecMode,
                                                  mtu, trafficClass,
                                                  m_securityMode == DataPlaneSecurityMode::Secure);
    peer->setRawUdpTunnel(trafficClass, tunnel);
    peer->setTransport(trafficClass, TRANSPORT_RELAY_RAW_UDP);
    m_rawByPeerClass[tunnelKey(peer->peerId(), trafficClass)] = tunnel;

    uint32_t peerId = peer->peerId();
    tunnel->setOnDead([this, peerId, trafficClass]() {
        markTransportDead(peerId, trafficClass);
    });

    LOG_INFO("Raw UDP tunnel created for peer %u class=%u via %s:%u %s mtu=%u",
             peer->peerId(), static_cast<unsigned>(trafficClass), ipToString(addr).c_str(), port,
             fecModeName(fecMode), normalizeRoomMtu(mtu));
    return tunnel;
}

void CliTunnelManager::onUdpReadable() {
    char buf[65536];
    struct sockaddr_in senderAddr;
    socklen_t addrLen = sizeof(senderAddr);

    for (;;) {
        int n = recvfrom(m_udpFd, buf, sizeof(buf), 0,
                         (struct sockaddr*)&senderAddr, &addrLen);
        if (n <= 0) break;
        if (!dataPlaneAllowsTraffic(m_dataPlaneState, m_securityMode)) {
            continue;
        }
        if (ntohl(senderAddr.sin_addr.s_addr) != m_serverIP ||
            ntohs(senderAddr.sin_port) != m_serverPort)
            continue;

        uint8_t pktType = static_cast<uint8_t>(buf[0]);
        Buffer plain;
        if (m_securityMode == DataPlaneSecurityMode::Secure) {
            if (pktType != UDP_ENCRYPTED)
                continue;
            if (n < static_cast<int>(1 + SECURE_SESSION_ID_SIZE + SECURE_FRAME_OVERHEAD))
                continue;
            uint32_t sid = readU32BE(reinterpret_cast<const uint8_t*>(buf) + 1);
            if (sid != m_secureSessionId)
                continue;
            std::vector<uint8_t> decrypted;
            const uint8_t* frame = reinterpret_cast<const uint8_t*>(buf) + 1 + SECURE_SESSION_ID_SIZE;
            size_t frameLen = static_cast<size_t>(n - 1 - SECURE_SESSION_ID_SIZE);
            if (!m_udpCipher.decrypt(frame, frameLen, &decrypted) || decrypted.empty())
                continue;
            plain.assign(decrypted.begin(), decrypted.end());
            if (plain.size() > sizeof(buf))
                continue;
            memcpy(buf, plain.data(), plain.size());
            n = static_cast<int>(plain.size());
            pktType = static_cast<uint8_t>(buf[0]);
        }

        switch (pktType) {
        case UDP_RELAY_DATA: {
            if (n < static_cast<int>(sizeof(UdpRelayHeader))) break;
            const UdpRelayHeader* hdr = reinterpret_cast<const UdpRelayHeader*>(buf);
            uint32_t srcPeerId = ntohl(hdr->srcPeerId);
            uint32_t dstPeerId = ntohl(hdr->dstPeerId);
            if (dstPeerId != m_myPeerId ||
                (hdr->trafficClass != TRAFFIC_TCP &&
                 hdr->trafficClass != TRAFFIC_UDP))
                break;
            TrafficClass cls = static_cast<TrafficClass>(hdr->trafficClass);
            auto it = m_kcpByPeerClass.find(tunnelKey(srcPeerId, cls));
            if (it != m_kcpByPeerClass.end()) {
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
            uint32_t dstPeerId = ntohl(hdr->dstPeerId);
            if (dstPeerId != m_myPeerId ||
                (hdr->trafficClass != TRAFFIC_TCP &&
                 hdr->trafficClass != TRAFFIC_UDP))
                break;
            TrafficClass cls = static_cast<TrafficClass>(hdr->trafficClass);
            auto it = m_rawByPeerClass.find(tunnelKey(srcPeerId, cls));
            if (it != m_rawByPeerClass.end()) {
                const char* fragData = buf + sizeof(UdpRelayHeader);
                int fragLen = n - sizeof(UdpRelayHeader);
                it->second->feedInput(fragData, fragLen);
            }
            break;
        }
        default:
            break;
        }
    }
}

void CliTunnelManager::processTunPackets() {
    if (!m_tun) return;
    auto packets = m_tun->recvQueue().popAll();
    if (m_dataPlaneState != DataPlaneState::Running ||
        m_securityMode == DataPlaneSecurityMode::Unconfigured) {
        return;
    }
    for (auto& pkt : packets)
        routeFromTun(pkt);
}

void CliTunnelManager::routeFromTun(const Buffer& ipPacket) {
    if (!dataPlaneAllowsTraffic(m_dataPlaneState, m_securityMode)) {
        return;
    }
    if (!validateOutboundOverlayIpv4(ipPacket.data(), ipPacket.size(),
                                     m_roomMtu, m_myVirtualIP).isValid())
        return;
    uint32_t dstIP = extractDstIP(ipPacket.data(), ipPacket.size());

    if (isBroadcast(dstIP) || dstIP == VNET_BROADCAST) {
        for (auto& kv : m_peerById)
            kv.second->sendData(ipPacket);
    } else {
        CliPeerConnection* peer = peerByVirtualIP(dstIP);
        if (peer) peer->sendData(ipPacket);
    }
}

void CliTunnelManager::routeToTun(uint32_t peerId, const Buffer& ipPacket) {
    if (!dataPlaneAllowsTraffic(m_dataPlaneState, m_securityMode)) {
        return;
    }
    CliPeerConnection* peer = peerById(peerId);
    if (!peer || !validateInboundOverlayIpv4(
            ipPacket.data(), ipPacket.size(), m_roomMtu,
            peer->virtualIP(), m_myVirtualIP).isValid())
        return;
    if (m_tun) m_tun->writePacket(ipPacket);
}

void CliTunnelManager::onPeerDataReceived(uint32_t peerId, const Buffer& ipPacket) {
    if (m_dataPlaneState != DataPlaneState::Running ||
        m_securityMode == DataPlaneSecurityMode::Unconfigured) {
        return;
    }
    routeToTun(peerId, ipPacket);
}

void CliTunnelManager::updateKcp() {
    if (m_dataPlaneState != DataPlaneState::Running ||
        m_securityMode == DataPlaneSecurityMode::Unconfigured) {
        return;
    }
    std::vector<uint64_t> kcpKeys;
    kcpKeys.reserve(m_kcpByPeerClass.size());
    for (auto& kv : m_kcpByPeerClass)
        kcpKeys.push_back(kv.first);
    for (uint64_t key : kcpKeys) {
        auto it = m_kcpByPeerClass.find(key);
        if (it != m_kcpByPeerClass.end())
            it->second->update();
    }

    std::vector<uint64_t> rawKeys;
    rawKeys.reserve(m_rawByPeerClass.size());
    for (auto& kv : m_rawByPeerClass)
        rawKeys.push_back(kv.first);
    for (uint64_t key : rawKeys) {
        auto it = m_rawByPeerClass.find(key);
        if (it != m_rawByPeerClass.end())
            it->second->update();
    }

    flushPendingTransportDead();
}

void CliTunnelManager::markTransportDead(uint32_t peerId, TrafficClass cls) {
    if (m_dataPlaneState != DataPlaneState::Running)
        return;
    int idx = static_cast<int>(cls);
    if (idx < 0 || idx > 2) cls = TRAFFIC_UDP;
    m_pendingTransportDead.insert(tunnelKey(peerId, cls));
}

void CliTunnelManager::flushPendingTransportDead() {
    if (m_dataPlaneState != DataPlaneState::Running) {
        m_pendingTransportDead.clear();
        return;
    }
    if (m_pendingTransportDead.empty())
        return;

    std::vector<uint64_t> dead(m_pendingTransportDead.begin(), m_pendingTransportDead.end());
    m_pendingTransportDead.clear();

    for (uint64_t key : dead) {
        uint32_t peerId = static_cast<uint32_t>(key >> 8);
        TrafficClass cls = static_cast<TrafficClass>(key & 0xFF);
        if (!peerById(peerId))
            continue;
        if (onTunnelDead)
            onTunnelDead(peerId, cls);
    }
}

void CliTunnelManager::sendUdpKeepalive() {
    if (m_dataPlaneState != DataPlaneState::Running ||
        m_securityMode == DataPlaneSecurityMode::Unconfigured) {
        return;
    }
    if (m_serverIP == 0 || m_serverPort == 0) return;
    UdpHeader hdr;
    hdr.type = UDP_KEEPALIVE;
    udpSend(reinterpret_cast<const uint8_t*>(&hdr), sizeof(hdr),
            m_serverIP, m_serverPort);
}

} // namespace VLan

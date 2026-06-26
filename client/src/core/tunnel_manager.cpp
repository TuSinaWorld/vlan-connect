#include "tunnel_manager.h"
#include "tun_adapter.h"
#include "kcp_tunnel.h"
#include "raw_udp_tunnel.h"
#include "p2p_peer.h"
#include "net_common.h"
#include "../ui/log_manager.h"
#include <cstring>

namespace VLan {

TunnelManager::TunnelManager(QObject* parent)
    : QObject(parent),
      m_tun(nullptr),
      m_serverStunPort(0), m_myPeerId(0), m_myVirtualIP(0),
      m_tunUploadBytes(0), m_tunDownloadBytes(0)
{
    m_udpSocket = new QUdpSocket(this);
    m_udpSocket->bind(QHostAddress(QHostAddress::Any), quint16(0));
    connect(m_udpSocket, &QUdpSocket::readyRead, this, &TunnelManager::onUdpReadyRead);

    m_kcpTimer = new QTimer(this);
    m_kcpTimer->setTimerType(Qt::PreciseTimer);
    connect(m_kcpTimer, &QTimer::timeout, this, &TunnelManager::onKcpUpdate);
    m_kcpTimer->start(5);

    m_udpKeepaliveTimer = new QTimer(this);
    connect(m_udpKeepaliveTimer, &QTimer::timeout, this, &TunnelManager::onUdpKeepalive);
    m_udpKeepaliveTimer->start(UDP_KEEPALIVE_INTERVAL_MS);
}

TunnelManager::~TunnelManager() {
    m_kcpTimer->stop();
    m_udpKeepaliveTimer->stop();
    qDeleteAll(m_peerById);
}

void TunnelManager::setTunAdapter(TunAdapter* tun) {
    m_tun = tun;
    if (tun)
        connect(tun, &TunAdapter::packetReceived, this, &TunnelManager::onTunPacketReceived);
}

void TunnelManager::setServerEndpoint(const QHostAddress& addr, quint16 stunPort) {
    m_serverAddr     = addr;
    m_serverStunPort = stunPort;
}

quint16 TunnelManager::localUdpPort() const {
    return m_udpSocket->localPort();
}

// ───────── Peer management ─────────

P2PPeer* TunnelManager::addPeer(uint32_t peerId, uint32_t virtualIP, const QString& name) {
    if (m_peerById.contains(peerId)) return m_peerById[peerId];

    P2PPeer* peer = new P2PPeer(peerId, virtualIP, name, this);
    m_peerById[peerId]    = peer;
    m_peerByVIP[virtualIP] = peer;

    connect(peer, &P2PPeer::dataReceived, this, &TunnelManager::onPeerDataReceived);
    LogManager::instance().logDetail(QString("[tunnel] Added peer %1 vip %2 %3").arg(peerId).arg(virtualIPToString(virtualIP)).arg(name));
    return peer;
}

void TunnelManager::removePeer(uint32_t peerId) {
    auto it = m_peerById.find(peerId);
    if (it == m_peerById.end()) return;

    P2PPeer* peer = it.value();
    m_peerByVIP.remove(peer->virtualIP());

    KcpTunnel* kcp = peer->kcpTunnel();
    if (kcp) {
        EndpointKey key;
        key.ip   = kcp->peerAddress().toIPv4Address();
        key.port = kcp->peerPort();
        m_endpointToKcp.remove(key);
        delete kcp;
    }
    RawUdpTunnel* raw = peer->rawUdpTunnel();
    if (raw) {
        delete raw;
    }

    m_peerById.erase(it);
    delete peer;
    LogManager::instance().logDetail(QString("[tunnel] Removed peer %1").arg(peerId));
}

void TunnelManager::removeAllPeers() {
    for (auto it = m_peerById.begin(); it != m_peerById.end(); ++it) {
        P2PPeer* peer = it.value();
        KcpTunnel* kcp = peer->kcpTunnel();
        if (kcp) {
            EndpointKey key;
            key.ip   = kcp->peerAddress().toIPv4Address();
            key.port = kcp->peerPort();
            m_endpointToKcp.remove(key);
            delete kcp;
        }
        RawUdpTunnel* raw = peer->rawUdpTunnel();
        if (raw) {
            delete raw;
        }
        delete peer;
    }
    m_peerById.clear();
    m_peerByVIP.clear();
    m_endpointToKcp.clear();
    LogManager::instance().logDetail(QString("[tunnel] All peers removed"));
}

P2PPeer* TunnelManager::peerById(uint32_t peerId) {
    auto it = m_peerById.find(peerId);
    return (it != m_peerById.end()) ? it.value() : nullptr;
}

P2PPeer* TunnelManager::peerByVirtualIP(uint32_t vip) {
    auto it = m_peerByVIP.find(vip);
    return (it != m_peerByVIP.end()) ? it.value() : nullptr;
}

// ───────── KCP tunnel creation ─────────

KcpTunnel* TunnelManager::createKcpTunnel(P2PPeer* peer,
                                          const QHostAddress& addr, quint16 port,
                                          TransportType type,
                                          FecMode fecMode,
                                          uint16_t mtu)
{
    uint32_t conv = m_myPeerId ^ peer->peerId();
    if (conv == 0) conv = 1;
    KcpTunnel* kcp = new KcpTunnel(conv, m_udpSocket, addr, port, fecMode, mtu, this);
    peer->setKcpTunnel(kcp);
    peer->setTransport(type);

    EndpointKey key;
    key.ip   = addr.toIPv4Address();
    key.port = port;
    m_endpointToKcp[key] = kcp;

    uint32_t peerId = peer->peerId();
    connect(kcp, &KcpTunnel::tunnelDead, this, [this, peerId]() {
        emit tunnelDead(peerId);
    });

    LogManager::instance().logDetail(QString("[tunnel] KCP tunnel created for peer %1 via %2:%3 %4 %5 mtu=%6").arg(peer->peerId()).arg(addr.toString()).arg(port).arg(transportName(type)).arg(fecModeName(fecMode)).arg(normalizeRoomMtu(mtu)));
    return kcp;
}

RawUdpTunnel* TunnelManager::createRawUdpTunnel(P2PPeer* peer,
                                                 const QHostAddress& addr, quint16 port,
                                                 FecMode fecMode,
                                                 uint16_t mtu)
{
    RawUdpTunnel* tunnel = new RawUdpTunnel(m_udpSocket, addr, port, fecMode, mtu, this);
    peer->setRawUdpTunnel(tunnel);
    peer->setTransport(TRANSPORT_RELAY_RAW_UDP);

    uint32_t peerId = peer->peerId();
    connect(tunnel, &RawUdpTunnel::tunnelDead, this, [this, peerId]() {
        emit tunnelDead(peerId);
    });

    LogManager::instance().logDetail(QString("[tunnel] Raw UDP tunnel created for peer %1 via %2:%3 %4 mtu=%5").arg(peer->peerId()).arg(addr.toString()).arg(port).arg(fecModeName(fecMode)).arg(normalizeRoomMtu(mtu)));
    return tunnel;
}

// ───────── TUN -> Network ─────────

void TunnelManager::onTunPacketReceived(QByteArray packet) {
    routeFromTun(packet);
}

void TunnelManager::routeFromTun(const QByteArray& ipPacket) {
    if (ipPacket.size() < 20) return;
    m_tunUploadBytes += static_cast<quint64>(ipPacket.size());

    uint32_t dstIP = extractDstIP(
        reinterpret_cast<const uint8_t*>(ipPacket.constData()), ipPacket.size());

    if (isBroadcast(dstIP) || dstIP == VNET_BROADCAST) {
        if (g_verboseLog)
            LogManager::instance().logDetail(QString("[tunnel] routeFromTun BROADCAST size=%1 peers=%2").arg(ipPacket.size()).arg(m_peerById.size()));
        for (auto it = m_peerById.begin(); it != m_peerById.end(); ++it) {
            it.value()->sendData(ipPacket);
        }
    } else {
        P2PPeer* peer = peerByVirtualIP(dstIP);
        if (g_verboseLog)
            LogManager::instance().logDetail(QString("[tunnel] routeFromTun UNICAST dst=%1 size=%2 found=%3").arg(virtualIPToString(dstIP)).arg(ipPacket.size()).arg(peer != nullptr));
        if (peer) peer->sendData(ipPacket);
    }
}

// ───────── Network -> TUN ─────────

void TunnelManager::onPeerDataReceived(QByteArray ipPacket) {
    routeToTun(ipPacket);
}

void TunnelManager::routeToTun(const QByteArray& ipPacket) {
    if (g_verboseLog)
        LogManager::instance().logDetail(QString("[tunnel] routeToTun size=%1").arg(ipPacket.size()));
    if (m_tun && m_tun->writePacket(ipPacket))
        m_tunDownloadBytes += static_cast<quint64>(ipPacket.size());
}

void TunnelManager::resetTrafficCounters() {
    m_tunUploadBytes = 0;
    m_tunDownloadBytes = 0;
}

void TunnelManager::trafficCounters(quint64* uploadBytes, quint64* downloadBytes) const {
    if (uploadBytes) *uploadBytes = m_tunUploadBytes;
    if (downloadBytes) *downloadBytes = m_tunDownloadBytes;
}

void TunnelManager::addTunDownloadBytes(quint64 bytes) {
    m_tunDownloadBytes += bytes;
}

// ───────── UDP receive ─────────

void TunnelManager::onUdpReadyRead() {
    while (m_udpSocket->hasPendingDatagrams()) {
        QByteArray datagram;
        datagram.resize(m_udpSocket->pendingDatagramSize());
        QHostAddress sender;
        quint16 senderPort;
        m_udpSocket->readDatagram(datagram.data(), datagram.size(),
                                  &sender, &senderPort);

        if (datagram.isEmpty()) continue;
        uint8_t pktType = static_cast<uint8_t>(datagram[0]);

        if (g_verboseLog)
            LogManager::instance().logDetail(QString("[tunnel] onUdpReadyRead type=0x%1 size=%2 from=%3:%4").arg(pktType, 0, 16).arg(datagram.size()).arg(sender.toString()).arg(senderPort));

        switch (pktType) {
        case UDP_KCP_DATA: {
            EndpointKey key;
            key.ip   = sender.toIPv4Address();
            key.port = senderPort;
            auto it = m_endpointToKcp.find(key);
            if (it != m_endpointToKcp.end()) {
                it.value()->feedInput(datagram.constData() + 1, datagram.size() - 1);
            }
            break;
        }
        case UDP_RELAY_DATA: {
            if (datagram.size() < static_cast<int>(sizeof(UdpRelayHeader))) break;
            const UdpRelayHeader* hdr =
                reinterpret_cast<const UdpRelayHeader*>(datagram.constData());
            uint32_t srcPeerId = ntohl(hdr->srcPeerId);
            P2PPeer* peer = peerById(srcPeerId);
            if (peer && peer->kcpTunnel()) {
                const char* kcpData = datagram.constData() + sizeof(UdpRelayHeader);
                int kcpLen = datagram.size() - sizeof(UdpRelayHeader);
                peer->kcpTunnel()->feedInput(kcpData, kcpLen);
            }
            break;
        }
        case UDP_RAW_RELAY_DATA: {
            if (datagram.size() < static_cast<int>(sizeof(UdpRelayHeader) + sizeof(FragHeader)))
                break;
            const UdpRelayHeader* hdr =
                reinterpret_cast<const UdpRelayHeader*>(datagram.constData());
            uint32_t srcPeerId = ntohl(hdr->srcPeerId);
            P2PPeer* peer = peerById(srcPeerId);
            if (peer && peer->rawUdpTunnel()) {
                const char* fragData = datagram.constData() + sizeof(UdpRelayHeader);
                int fragLen = datagram.size() - sizeof(UdpRelayHeader);
                peer->rawUdpTunnel()->feedInput(fragData, fragLen);
            }
            break;
        }
        case UDP_STUN_RESPONSE:
        case UDP_PUNCH:
        case UDP_PUNCH_ACK:
            emit rawUdpReceived(datagram, sender, senderPort);
            break;
        default:
            break;
        }
    }
}

// ───────── KCP periodic update ─────────

void TunnelManager::onKcpUpdate() {
    QList<P2PPeer*> peers = m_peerById.values();
    for (P2PPeer* peer : peers) {
        if (!m_peerById.contains(peer->peerId())) continue;
        KcpTunnel* kcp = peer->kcpTunnel();
        if (kcp) kcp->update();
        RawUdpTunnel* raw = peer->rawUdpTunnel();
        if (raw) raw->update();
    }
}

// ───────── UDP keepalive to server ─────────

void TunnelManager::onUdpKeepalive() {
    if (m_serverAddr.isNull() || m_serverStunPort == 0) return;

    UdpHeader hdr;
    hdr.type = UDP_KEEPALIVE;
    m_udpSocket->writeDatagram(
        reinterpret_cast<const char*>(&hdr), sizeof(hdr),
        m_serverAddr, m_serverStunPort);
}

} // namespace VLan

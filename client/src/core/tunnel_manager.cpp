#include "tunnel_manager.h"
#include "tun_adapter.h"
#include "kcp_tunnel.h"
#include "raw_udp_tunnel.h"
#include "peer_connection.h"
#include "net_common.h"
#include "../ui/log_manager.h"
#include <cstring>

namespace VLan {

TunnelManager::TunnelManager(QObject* parent)
    : QObject(parent),
      m_tun(nullptr),
      m_serverUdpPort(0), m_myPeerId(0), m_myVirtualIP(0),
      m_tunUploadBytes(0), m_tunDownloadBytes(0),
      m_secureUdpEnabled(false), m_secureSessionId(0)
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

void TunnelManager::setServerEndpoint(const QHostAddress& addr, quint16 udpPort) {
    m_serverAddr     = addr;
    m_serverUdpPort = udpPort;
}

quint16 TunnelManager::localUdpPort() const {
    return m_udpSocket->localPort();
}

void TunnelManager::setSecureSession(uint32_t sessionId, const QByteArray& master) {
    m_secureSessionId = sessionId;
    m_secureMaster = master;
    m_secureUdpEnabled = (sessionId != 0 && master.size() == 32);
    if (m_secureUdpEnabled)
        m_udpCipher.init(reinterpret_cast<const uint8_t*>(m_secureMaster.constData()), true, "udp");
}

// ───────── Peer management ─────────

PeerConnection* TunnelManager::addPeer(uint32_t peerId, uint32_t virtualIP, const QString& name) {
    if (m_peerById.contains(peerId)) return m_peerById[peerId];

    PeerConnection* peer = new PeerConnection(peerId, virtualIP, name, this);
    m_peerById[peerId]    = peer;
    m_peerByVIP[virtualIP] = peer;

    connect(peer, &PeerConnection::dataReceived, this, &TunnelManager::onPeerDataReceived);
    LogManager::instance().logDetail(QString("[tunnel] Added peer %1 vip %2 %3").arg(peerId).arg(virtualIPToString(virtualIP)).arg(name));
    return peer;
}

void TunnelManager::removePeer(uint32_t peerId) {
    auto it = m_peerById.find(peerId);
    if (it == m_peerById.end()) return;

    PeerConnection* peer = it.value();
    for (auto pit = m_pendingTransportDead.begin(); pit != m_pendingTransportDead.end(); ) {
        if (pit.key().peerId == peerId)
            pit = m_pendingTransportDead.erase(pit);
        else
            ++pit;
    }
    m_peerByVIP.remove(peer->virtualIP());

    for (int cls = TRAFFIC_TCP; cls <= TRAFFIC_UDP; ++cls) {
        KcpTunnel* kcp = peer->kcpTunnel(static_cast<TrafficClass>(cls));
        if (kcp) {
            EndpointKey key;
            key.ip   = kcp->peerAddress().toIPv4Address();
            key.port = kcp->peerPort();
            m_endpointToKcp.remove(key);
            delete kcp;
        }
        RawUdpTunnel* raw = peer->rawUdpTunnel(static_cast<TrafficClass>(cls));
        if (raw) delete raw;
    }

    m_peerById.erase(it);
    delete peer;
    LogManager::instance().logDetail(QString("[tunnel] Removed peer %1").arg(peerId));
}

void TunnelManager::removeTransport(uint32_t peerId, TrafficClass cls) {
    PeerConnection* peer = peerById(peerId);
    if (!peer) return;

    int idx = static_cast<int>(cls);
    if (idx < 0 || idx > 2) cls = TRAFFIC_UDP;

    TransportKey pendingKey;
    pendingKey.peerId = peerId;
    pendingKey.cls = cls;
    m_pendingTransportDead.remove(pendingKey);

    KcpTunnel* kcp = peer->kcpTunnel(cls);
    if (kcp) {
        EndpointKey key;
        key.ip   = kcp->peerAddress().toIPv4Address();
        key.port = kcp->peerPort();
        m_endpointToKcp.remove(key);
        peer->clearKcpTunnel(cls);
        delete kcp;
    }

    RawUdpTunnel* raw = peer->rawUdpTunnel(cls);
    if (raw) {
        peer->clearRawUdpTunnel(cls);
        delete raw;
    }

    peer->clearTransport(cls);
    LogManager::instance().logDetail(QString("[tunnel] Removed transport peer=%1 class=%2")
        .arg(peerId).arg(static_cast<int>(cls)));
}

void TunnelManager::removeAllPeers() {
    m_pendingTransportDead.clear();
    for (auto it = m_peerById.begin(); it != m_peerById.end(); ++it) {
        PeerConnection* peer = it.value();
        for (int cls = TRAFFIC_TCP; cls <= TRAFFIC_UDP; ++cls) {
            KcpTunnel* kcp = peer->kcpTunnel(static_cast<TrafficClass>(cls));
            if (kcp) {
                EndpointKey key;
                key.ip   = kcp->peerAddress().toIPv4Address();
                key.port = kcp->peerPort();
                m_endpointToKcp.remove(key);
                delete kcp;
            }
            RawUdpTunnel* raw = peer->rawUdpTunnel(static_cast<TrafficClass>(cls));
            if (raw) delete raw;
        }
        delete peer;
    }
    m_peerById.clear();
    m_peerByVIP.clear();
    m_endpointToKcp.clear();
    LogManager::instance().logDetail(QString("[tunnel] All peers removed"));
}

PeerConnection* TunnelManager::peerById(uint32_t peerId) {
    auto it = m_peerById.find(peerId);
    return (it != m_peerById.end()) ? it.value() : nullptr;
}

PeerConnection* TunnelManager::peerByVirtualIP(uint32_t vip) {
    auto it = m_peerByVIP.find(vip);
    return (it != m_peerByVIP.end()) ? it.value() : nullptr;
}

// ───────── KCP tunnel creation ─────────

KcpTunnel* TunnelManager::createKcpTunnel(PeerConnection* peer,
                                          const QHostAddress& addr, quint16 port,
                                          TransportType type,
                                          FecMode fecMode,
                                          uint16_t mtu,
                                          KcpProfile profile,
                                          TrafficClass trafficClass)
{
    uint32_t conv = m_myPeerId ^ peer->peerId() ^ (static_cast<uint32_t>(trafficClass) << 24);
    if (conv == 0) conv = 1;
    KcpTunnel* kcp = new KcpTunnel(conv, m_udpSocket, addr, port, fecMode, mtu,
                                   profile, trafficClass, m_secureUdpEnabled, this);
    kcp->setDatagramSender([this](const QByteArray& pkt, const QHostAddress& a, quint16 p) {
        sendUdpDatagram(pkt, a, p);
    });
    peer->setKcpTunnel(trafficClass, kcp);
    peer->setTransport(trafficClass, type);

    EndpointKey key;
    key.ip   = addr.toIPv4Address();
    key.port = port;
    m_endpointToKcp[key] = kcp;

    uint32_t peerId = peer->peerId();
    connect(kcp, &KcpTunnel::tunnelDead, this, [this, peerId, trafficClass]() {
        markTransportDead(peerId, trafficClass);
    });

    LogManager::instance().logDetail(QString("[tunnel] KCP tunnel created for peer %1 class=%2 via %3:%4 %5 %6 mtu=%7")
        .arg(peer->peerId()).arg(static_cast<int>(trafficClass)).arg(addr.toString()).arg(port)
        .arg(transportName(type)).arg(fecModeName(fecMode)).arg(normalizeRoomMtu(mtu)));
    return kcp;
}

RawUdpTunnel* TunnelManager::createRawUdpTunnel(PeerConnection* peer,
                                                 const QHostAddress& addr, quint16 port,
                                                 FecMode fecMode,
                                                 uint16_t mtu,
                                                 TrafficClass trafficClass)
{
    RawUdpTunnel* tunnel = new RawUdpTunnel(m_udpSocket, addr, port, fecMode, mtu,
                                            trafficClass, m_secureUdpEnabled, this);
    tunnel->setDatagramSender([this](const QByteArray& pkt, const QHostAddress& a, quint16 p) {
        sendUdpDatagram(pkt, a, p);
    });
    peer->setRawUdpTunnel(trafficClass, tunnel);
    peer->setTransport(trafficClass, TRANSPORT_RELAY_RAW_UDP);

    uint32_t peerId = peer->peerId();
    connect(tunnel, &RawUdpTunnel::tunnelDead, this, [this, peerId, trafficClass]() {
        markTransportDead(peerId, trafficClass);
    });

    LogManager::instance().logDetail(QString("[tunnel] Raw UDP tunnel created for peer %1 class=%2 via %3:%4 %5 mtu=%6")
        .arg(peer->peerId()).arg(static_cast<int>(trafficClass)).arg(addr.toString()).arg(port)
        .arg(fecModeName(fecMode)).arg(normalizeRoomMtu(mtu)));
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
        PeerConnection* peer = peerByVirtualIP(dstIP);
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

void TunnelManager::sendUdpDatagram(const QByteArray& datagram,
                                    const QHostAddress& addr, quint16 port) {
    if (!m_secureUdpEnabled) {
        m_udpSocket->writeDatagram(datagram, addr, port);
        return;
    }
    std::vector<uint8_t> enc = m_udpCipher.encrypt(
        reinterpret_cast<const uint8_t*>(datagram.constData()), datagram.size());
    QByteArray pkt;
    pkt.resize(1 + SECURE_SESSION_ID_SIZE + static_cast<int>(enc.size()));
    pkt[0] = static_cast<char>(UDP_ENCRYPTED);
    writeU32BE(reinterpret_cast<uint8_t*>(pkt.data()) + 1, m_secureSessionId);
    if (!enc.empty()) {
        memcpy(pkt.data() + 1 + SECURE_SESSION_ID_SIZE,
               enc.data(), enc.size());
    }
    m_udpSocket->writeDatagram(pkt, addr, port);
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

        if (m_secureUdpEnabled) {
            if (pktType != UDP_ENCRYPTED)
                continue;
            if (datagram.size() < 1 + SECURE_SESSION_ID_SIZE + SECURE_FRAME_OVERHEAD)
                continue;
            uint32_t sid = readU32BE(reinterpret_cast<const uint8_t*>(datagram.constData()) + 1);
            if (sid != m_secureSessionId)
                continue;
            std::vector<uint8_t> plain;
            const uint8_t* frame = reinterpret_cast<const uint8_t*>(datagram.constData()) + 1 + SECURE_SESSION_ID_SIZE;
            size_t frameLen = static_cast<size_t>(datagram.size() - 1 - SECURE_SESSION_ID_SIZE);
            if (!m_udpCipher.decrypt(frame, frameLen, &plain) || plain.empty())
                continue;
            datagram = QByteArray(reinterpret_cast<const char*>(plain.data()),
                                  static_cast<int>(plain.size()));
            pktType = static_cast<uint8_t>(datagram[0]);
        }

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
            TrafficClass cls = hdr->trafficClass == TRAFFIC_TCP ? TRAFFIC_TCP : TRAFFIC_UDP;
            PeerConnection* peer = peerById(srcPeerId);
            if (peer && peer->kcpTunnel(cls)) {
                const char* kcpData = datagram.constData() + sizeof(UdpRelayHeader);
                int kcpLen = datagram.size() - sizeof(UdpRelayHeader);
                peer->kcpTunnel(cls)->feedInput(kcpData, kcpLen);
            }
            break;
        }
        case UDP_RAW_RELAY_DATA: {
            if (datagram.size() < static_cast<int>(sizeof(UdpRelayHeader) + sizeof(FragHeader)))
                break;
            const UdpRelayHeader* hdr =
                reinterpret_cast<const UdpRelayHeader*>(datagram.constData());
            uint32_t srcPeerId = ntohl(hdr->srcPeerId);
            TrafficClass cls = hdr->trafficClass == TRAFFIC_TCP ? TRAFFIC_TCP : TRAFFIC_UDP;
            PeerConnection* peer = peerById(srcPeerId);
            if (peer && peer->rawUdpTunnel(cls)) {
                const char* fragData = datagram.constData() + sizeof(UdpRelayHeader);
                int fragLen = datagram.size() - sizeof(UdpRelayHeader);
                peer->rawUdpTunnel(cls)->feedInput(fragData, fragLen);
            }
            break;
        }
        default:
            break;
        }
    }
}

// ───────── KCP periodic update ─────────

void TunnelManager::onKcpUpdate() {
    QList<uint32_t> peerIds = m_peerById.keys();
    for (uint32_t peerId : peerIds) {
        for (int cls = TRAFFIC_TCP; cls <= TRAFFIC_UDP; ++cls) {
            TrafficClass trafficClass = static_cast<TrafficClass>(cls);
            PeerConnection* peer = peerById(peerId);
            if (!peer) break;

            KcpTunnel* kcp = peer->kcpTunnel(trafficClass);
            if (kcp) kcp->update();

            peer = peerById(peerId);
            if (!peer) break;

            RawUdpTunnel* raw = peer->rawUdpTunnel(trafficClass);
            if (raw) raw->update();
        }
    }
    flushPendingTransportDead();
}

void TunnelManager::markTransportDead(uint32_t peerId, TrafficClass cls) {
    int idx = static_cast<int>(cls);
    if (idx < 0 || idx > 2) cls = TRAFFIC_UDP;

    TransportKey key;
    key.peerId = peerId;
    key.cls = cls;
    m_pendingTransportDead[key] = true;
}

void TunnelManager::flushPendingTransportDead() {
    if (m_pendingTransportDead.isEmpty())
        return;

    QList<TransportKey> dead = m_pendingTransportDead.keys();
    m_pendingTransportDead.clear();

    for (const TransportKey& key : dead) {
        if (!m_peerById.contains(key.peerId))
            continue;
        emit transportDead(key.peerId, key.cls);
    }
}

// ───────── UDP keepalive to server ─────────

void TunnelManager::onUdpKeepalive() {
    if (m_serverAddr.isNull() || m_serverUdpPort == 0) return;

    UdpHeader hdr;
    hdr.type = UDP_KEEPALIVE;
    QByteArray pkt(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    sendUdpDatagram(pkt, m_serverAddr, m_serverUdpPort);
}

} // namespace VLan

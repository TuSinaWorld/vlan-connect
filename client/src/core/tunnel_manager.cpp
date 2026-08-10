#include "tunnel_manager.h"
#include "tun_adapter.h"
#include "kcp_tunnel.h"
#include "raw_udp_tunnel.h"
#include "peer_connection.h"
#include "net_common.h"
#include "overlay_packet_validator.h"
#include "../ui/log_manager.h"
#include <cstring>

namespace VLan {

TunnelManager::TunnelManager(QObject* parent)
    : QObject(parent),
      m_tun(nullptr),
      m_serverUdpPort(0), m_myPeerId(0), m_myVirtualIP(0),
      m_roomMtu(ROOM_MTU_DEFAULT),
      m_tunUploadBytes(0), m_tunDownloadBytes(0),
      m_tunGeneration(0),
      m_dataPlaneState(DataPlaneState::Stopped),
      m_securityMode(DataPlaneSecurityMode::Unconfigured),
      m_secureSessionId(0)
{
    m_udpSocket = new QUdpSocket(this);
    m_udpSocket->bind(QHostAddress(QHostAddress::Any), quint16(0));
    connect(m_udpSocket, &QUdpSocket::readyRead, this, &TunnelManager::onUdpReadyRead);

    m_kcpTimer = new QTimer(this);
    m_kcpTimer->setTimerType(Qt::PreciseTimer);
    connect(m_kcpTimer, &QTimer::timeout, this, &TunnelManager::onKcpUpdate);

    m_udpKeepaliveTimer = new QTimer(this);
    connect(m_udpKeepaliveTimer, &QTimer::timeout, this, &TunnelManager::onUdpKeepalive);
}

TunnelManager::~TunnelManager() {
    stopDataPlane();
    qDeleteAll(m_peerById);
    clearSecurityContext();
}

void TunnelManager::setTunAdapter(TunAdapter* tun) {
    if (m_tun)
        QObject::disconnect(m_tun, nullptr, this, nullptr);
    m_tun = tun;
    const quint64 generation = ++m_tunGeneration;
    if (tun) {
        connect(tun, &TunAdapter::packetReceived, this,
                [this, tun, generation](QByteArray packet) {
            if (m_tun != tun || m_tunGeneration != generation ||
                !dataPlaneAllowsTraffic(
                    m_dataPlaneState, m_securityMode)) {
                return;
            }
            onTunPacketReceived(packet);
        });
    }
}

void TunnelManager::setServerEndpoint(const QHostAddress& addr, quint16 udpPort) {
    m_serverAddr     = addr;
    m_serverUdpPort = udpPort;
}

quint16 TunnelManager::localUdpPort() const {
    return m_udpSocket->localPort();
}

bool TunnelManager::installSecureSession(uint32_t sessionId, const QByteArray& master) {
    if (!dataPlaneCanReconfigure(m_dataPlaneState)) {
        LogManager::instance().logError("[tunnel] Refusing to change security while data plane is running");
        return false;
    }
    clearSecurityContext();
    if (sessionId == 0 || master.size() != SECURE_KEY_SIZE) {
        LogManager::instance().logError("[tunnel] Invalid secure data-plane session");
        return false;
    }
    m_secureSessionId = sessionId;
    m_secureMaster = master;
    m_udpCipher.init(reinterpret_cast<const uint8_t*>(m_secureMaster.constData()), true, "udp");
    m_securityMode = DataPlaneSecurityMode::Secure;
    return true;
}

void TunnelManager::clearSecurityContext() {
    if (!dataPlaneCanReconfigure(m_dataPlaneState)) {
        LogManager::instance().logError("[tunnel] Refusing to clear security while data plane is running");
        return;
    }
    m_udpCipher.reset();
    m_secureSessionId = 0;
    if (!m_secureMaster.isEmpty()) {
        crypto_wipe(reinterpret_cast<uint8_t*>(m_secureMaster.data()),
                    static_cast<size_t>(m_secureMaster.size()));
        m_secureMaster.clear();
    }
    m_securityMode = DataPlaneSecurityMode::Unconfigured;
}

bool TunnelManager::startDataPlane() {
    if (m_dataPlaneState == DataPlaneState::Running)
        return true;
    if (!m_tun || m_securityMode == DataPlaneSecurityMode::Unconfigured) {
        LogManager::instance().logError("[tunnel] Cannot start unconfigured data plane");
        return false;
    }
    setTunAdapter(m_tun);
    m_dataPlaneState = DataPlaneState::Running;
    m_kcpTimer->start(5);
    m_udpKeepaliveTimer->start(UDP_KEEPALIVE_INTERVAL_MS);
    return true;
}

void TunnelManager::stopDataPlane() {
    m_dataPlaneState = DataPlaneState::Stopped;
    if (m_kcpTimer)
        m_kcpTimer->stop();
    if (m_udpKeepaliveTimer)
        m_udpKeepaliveTimer->stop();
    ++m_tunGeneration;
    if (m_tun)
        QObject::disconnect(m_tun, nullptr, this, nullptr);
    while (m_udpSocket && m_udpSocket->hasPendingDatagrams()) {
        QByteArray discarded;
        discarded.resize(static_cast<int>(m_udpSocket->pendingDatagramSize()));
        m_udpSocket->readDatagram(discarded.data(), discarded.size());
    }
}

// ───────── Peer management ─────────

PeerConnection* TunnelManager::addPeer(uint32_t peerId, uint32_t virtualIP, const QString& name) {
    if (m_peerById.contains(peerId)) return m_peerById[peerId];

    PeerConnection* peer = new PeerConnection(peerId, virtualIP, name, this);
    m_peerById[peerId]    = peer;
    m_peerByVIP[virtualIP] = peer;

    connect(peer, &PeerConnection::dataReceived, this,
            [this, peerId](QByteArray packet) {
        onPeerDataReceived(peerId, packet);
    });
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
    KcpTunnel* kcp = new KcpTunnel(
        conv, m_udpSocket, addr, port, fecMode, mtu, profile, trafficClass,
        m_securityMode == DataPlaneSecurityMode::Secure, this);
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
    RawUdpTunnel* tunnel = new RawUdpTunnel(
        m_udpSocket, addr, port, fecMode, mtu, trafficClass,
        m_securityMode == DataPlaneSecurityMode::Secure, this);
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
    if (!dataPlaneAllowsTraffic(m_dataPlaneState, m_securityMode)) {
        return;
    }
    routeFromTun(packet);
}

void TunnelManager::routeFromTun(const QByteArray& ipPacket) {
    if (!dataPlaneAllowsTraffic(m_dataPlaneState, m_securityMode)) {
        return;
    }
    if (!validateOutboundOverlayIpv4(
            reinterpret_cast<const uint8_t*>(ipPacket.constData()),
            static_cast<size_t>(ipPacket.size()), m_roomMtu,
            m_myVirtualIP).isValid())
        return;
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

void TunnelManager::onPeerDataReceived(uint32_t peerId, QByteArray ipPacket) {
    if (m_dataPlaneState != DataPlaneState::Running ||
        m_securityMode == DataPlaneSecurityMode::Unconfigured) {
        return;
    }
    routeToTun(peerId, ipPacket);
}

void TunnelManager::routeToTun(uint32_t peerId, const QByteArray& ipPacket) {
    if (!dataPlaneAllowsTraffic(m_dataPlaneState, m_securityMode)) {
        return;
    }
    PeerConnection* peer = peerById(peerId);
    if (!peer || !validateInboundOverlayIpv4(
            reinterpret_cast<const uint8_t*>(ipPacket.constData()),
            static_cast<size_t>(ipPacket.size()), m_roomMtu,
            peer->virtualIP(), m_myVirtualIP).isValid())
        return;
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
    if (m_dataPlaneState != DataPlaneState::Running)
        return;
    m_tunDownloadBytes += bytes;
}

void TunnelManager::sendUdpDatagram(const QByteArray& datagram,
                                    const QHostAddress& addr, quint16 port) {
    if (!dataPlaneAllowsTraffic(m_dataPlaneState, m_securityMode)) {
        return;
    }
    if (m_securityMode != DataPlaneSecurityMode::Secure ||
        m_secureSessionId == 0 || m_secureMaster.size() != SECURE_KEY_SIZE) {
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

        if (!dataPlaneAllowsTraffic(m_dataPlaneState, m_securityMode)) {
            continue;
        }
        if (sender != m_serverAddr || senderPort != m_serverUdpPort)
            continue;
        if (datagram.isEmpty()) continue;
        uint8_t pktType = static_cast<uint8_t>(datagram[0]);

        if (m_securityMode != DataPlaneSecurityMode::Secure ||
            pktType != UDP_ENCRYPTED)
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

        if (g_verboseLog)
            LogManager::instance().logDetail(QString("[tunnel] onUdpReadyRead type=0x%1 size=%2 from=%3:%4").arg(pktType, 0, 16).arg(datagram.size()).arg(sender.toString()).arg(senderPort));

        switch (pktType) {
        case UDP_RELAY_DATA: {
            if (datagram.size() < static_cast<int>(sizeof(UdpRelayHeader))) break;
            const UdpRelayHeader* hdr =
                reinterpret_cast<const UdpRelayHeader*>(datagram.constData());
            uint32_t srcPeerId = ntohl(hdr->srcPeerId);
            uint32_t dstPeerId = ntohl(hdr->dstPeerId);
            if (dstPeerId != m_myPeerId ||
                (hdr->trafficClass != TRAFFIC_TCP &&
                 hdr->trafficClass != TRAFFIC_UDP))
                break;
            TrafficClass cls = static_cast<TrafficClass>(hdr->trafficClass);
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
            uint32_t dstPeerId = ntohl(hdr->dstPeerId);
            if (dstPeerId != m_myPeerId ||
                (hdr->trafficClass != TRAFFIC_TCP &&
                 hdr->trafficClass != TRAFFIC_UDP))
                break;
            TrafficClass cls = static_cast<TrafficClass>(hdr->trafficClass);
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
    if (m_dataPlaneState != DataPlaneState::Running ||
        m_securityMode == DataPlaneSecurityMode::Unconfigured) {
        return;
    }
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
    if (m_dataPlaneState != DataPlaneState::Running)
        return;
    int idx = static_cast<int>(cls);
    if (idx < 0 || idx > 2) cls = TRAFFIC_UDP;

    TransportKey key;
    key.peerId = peerId;
    key.cls = cls;
    m_pendingTransportDead[key] = true;
}

void TunnelManager::flushPendingTransportDead() {
    if (m_dataPlaneState != DataPlaneState::Running) {
        m_pendingTransportDead.clear();
        return;
    }
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
    if (m_dataPlaneState != DataPlaneState::Running ||
        m_securityMode == DataPlaneSecurityMode::Unconfigured) {
        return;
    }
    if (m_serverAddr.isNull() || m_serverUdpPort == 0) return;

    UdpHeader hdr;
    hdr.type = UDP_KEEPALIVE;
    QByteArray pkt(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    sendUdpDatagram(pkt, m_serverAddr, m_serverUdpPort);
}

} // namespace VLan

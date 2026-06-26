#include "kcp_tunnel.h"
#include "fec_codec.h"
#include "net_common.h"
#include "protocol.h"
#include <cstring>
#include "../ui/log_manager.h"

namespace VLan {

static const char KCP_KEEPALIVE_MARKER = 0x00;

KcpTunnel::KcpTunnel(uint32_t conv, QUdpSocket* socket,
                     const QHostAddress& peerAddr, quint16 peerPort,
                     FecMode fecMode,
                     uint16_t mtu,
                     QObject* parent)
    : QObject(parent),
      m_socket(socket), m_peerAddr(peerAddr), m_peerPort(peerPort),
      m_relayMode(false), m_relaySrcPeerId(0), m_relayDstPeerId(0),
      m_dead(false),
      m_fecMode(fecMode), m_fecEncoder(nullptr), m_fecDecoder(nullptr)
{
    uint32_t now = currentTimeMs();
    m_lastRecvTime = now;
    m_lastSendTime = now;

    m_kcp = ikcp_create(conv, this);
    m_kcp->output = kcpOutput;

    ikcp_setmtu(m_kcp, kcpMtuFromRoomMtu(mtu));
    ikcp_nodelay(m_kcp, 1, 10, 2, 1);
    ikcp_wndsize(m_kcp, 256, 256);
    m_kcp->rx_minrto = 10;
    m_kcp->fastresend = 2;

    if (m_fecMode != FEC_NONE) {
        m_fecEncoder = new FecEncoder(m_fecMode,
            [this](const QByteArray& pkt) { sendKcpPacket(pkt); });
        m_fecDecoder = new FecDecoder(
            [this](const QByteArray& payload) {
                ikcp_input(m_kcp, payload.constData(), payload.size());
                tryRecv();
            });
    }
}

KcpTunnel::~KcpTunnel() {
    if (m_kcp) {
        ikcp_release(m_kcp);
        m_kcp = nullptr;
    }
    delete m_fecEncoder;
    delete m_fecDecoder;
}

int KcpTunnel::kcpOutput(const char* buf, int len, ikcpcb*, void* user) {
    KcpTunnel* self = static_cast<KcpTunnel*>(user);

    QByteArray kcpData(buf, len);
    if (self->m_fecMode != FEC_NONE && self->m_fecEncoder) {
        if (g_verboseLog)
            LogManager::instance().logDetail(QString("[kcp] kcpOutput len=%1 -> FEC encoder").arg(len));
        self->m_fecEncoder->addPacket(kcpData);
    } else {
        if (g_verboseLog)
            LogManager::instance().logDetail(QString("[kcp] kcpOutput len=%1 -> direct send").arg(len));
        self->sendKcpPacket(kcpData);
    }
    return 0;
}

void KcpTunnel::sendKcpPacket(const QByteArray& payload) {
    if (g_verboseLog)
        LogManager::instance().logDetail(QString("[kcp] sendKcpPacket size=%1 relay=%2 peer=%3:%4").arg(payload.size()).arg(m_relayMode).arg(m_peerAddr.toString()).arg(m_peerPort));
    QByteArray pkt;
    if (m_relayMode) {
        UdpRelayHeader hdr;
        hdr.type      = UDP_RELAY_DATA;
        hdr.srcPeerId = htonl(m_relaySrcPeerId);
        hdr.dstPeerId = htonl(m_relayDstPeerId);
        pkt.reserve(sizeof(hdr) + payload.size());
        pkt.append(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
        pkt.append(payload);
    } else {
        pkt.reserve(1 + payload.size());
        pkt.append(static_cast<char>(UDP_KCP_DATA));
        pkt.append(payload);
    }

    m_socket->writeDatagram(pkt, m_peerAddr, m_peerPort);
    m_lastSendTime = currentTimeMs();
}

void KcpTunnel::feedInput(const char* data, int len) {
    m_lastRecvTime = currentTimeMs();
    m_dead = false;

    if (g_verboseLog)
        LogManager::instance().logDetail(QString("[kcp] feedInput len=%1 fecMode=%2").arg(len).arg(m_fecMode));

    if (m_fecMode != FEC_NONE && m_fecDecoder) {
        m_fecDecoder->addPacket(data, len);
        return;
    }

    ikcp_input(m_kcp, data, len);
    tryRecv();
}

int KcpTunnel::send(const QByteArray& data) {
    return ikcp_send(m_kcp, data.constData(), data.size());
}

void KcpTunnel::update() {
    uint32_t now = currentTimeMs();
    ikcp_update(m_kcp, now);
    tryRecv();

    if (!m_dead && (now - m_lastRecvTime > static_cast<uint32_t>(KCP_DEAD_TIMEOUT_MS))) {
        m_dead = true;
        LogManager::instance().logDetail(QString("[kcp] Tunnel dead: no data for %1 ms, peer %2").arg(KCP_DEAD_TIMEOUT_MS).arg(m_peerAddr.toString()));
        emit tunnelDead();
        return;
    }

    if (now - m_lastSendTime > static_cast<uint32_t>(KCP_KEEPALIVE_INTERVAL_MS)) {
        ikcp_send(m_kcp, &KCP_KEEPALIVE_MARKER, 1);
    }

    if (m_fecEncoder) m_fecEncoder->update(now);
    if (m_fecDecoder) m_fecDecoder->cleanup(now);
}

void KcpTunnel::tryRecv() {
    char buf[65536];
    for (;;) {
        int n = ikcp_recv(m_kcp, buf, sizeof(buf));
        if (n <= 0) break;
        if (n == 1 && buf[0] == KCP_KEEPALIVE_MARKER) continue;
        emit dataReceived(QByteArray(buf, n));
    }
}

void KcpTunnel::setPeerEndpoint(const QHostAddress& addr, quint16 port) {
    m_peerAddr = addr;
    m_peerPort = port;
}

void KcpTunnel::setRelayMode(uint32_t srcPeerId, uint32_t dstPeerId) {
    m_relayMode      = true;
    m_relaySrcPeerId = srcPeerId;
    m_relayDstPeerId = dstPeerId;
}

bool KcpTunnel::isAlive() const {
    if (m_dead) return false;
    return (currentTimeMs() - m_lastRecvTime) < static_cast<uint32_t>(KCP_DEAD_TIMEOUT_MS);
}

int KcpTunnel::waitSendCount() const {
    return ikcp_waitsnd(m_kcp);
}

int KcpTunnel::getRttMs() const {
    return m_kcp ? m_kcp->rx_srtt : -1;
}

} // namespace VLan

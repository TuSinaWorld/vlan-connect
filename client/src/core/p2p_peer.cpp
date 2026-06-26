#include "p2p_peer.h"
#include "kcp_tunnel.h"
#include "raw_udp_tunnel.h"
#include "net_common.h"
#include "../ui/log_manager.h"

namespace VLan {

P2PPeer::P2PPeer(uint32_t peerId, uint32_t virtualIP,
                 const QString& name, QObject* parent)
    : QObject(parent),
      m_peerId(peerId), m_virtualIP(virtualIP), m_name(name),
      m_natType(NAT_UNKNOWN), m_transport(TRANSPORT_NONE),
      m_publicPort(0), m_kcpTunnel(nullptr), m_rawUdpTunnel(nullptr), m_cipher(nullptr),
      m_tcpRelayLastRecv(currentTimeMs()),
      m_tcpRelayLastSend(currentTimeMs()),
      m_tcpRtt(-1), m_latencyPingSentTime(0)
{}

P2PPeer::~P2PPeer() {}

void P2PPeer::setPublicEndpoint(const QHostAddress& addr, quint16 port) {
    m_publicAddr = addr;
    m_publicPort = port;
}

void P2PPeer::setKcpTunnel(KcpTunnel* tunnel) {
    m_kcpTunnel = tunnel;
    if (tunnel) {
        connect(tunnel, &KcpTunnel::dataReceived,
                this, &P2PPeer::onTunnelDataReceived);
    }
}

void P2PPeer::setRawUdpTunnel(RawUdpTunnel* tunnel) {
    m_rawUdpTunnel = tunnel;
    if (tunnel) {
        connect(tunnel, &RawUdpTunnel::dataReceived,
                this, &P2PPeer::onTunnelDataReceived);
    }
}

void P2PPeer::onTunnelDataReceived(QByteArray data) {
    if (m_cipher && data.size() >= 20
        && (static_cast<uint8_t>(data[0]) & 0xF0) == 0x40) {
        data = m_cipher->decrypt(data, m_peerId);
        if (data.isEmpty()) return;
    }
    emit dataReceived(data);
}

void P2PPeer::setTransport(TransportType t) {
    if (m_transport != t) {
        m_transport = t;
        emit transportChanged(m_peerId, t);
    }
}

int P2PPeer::sendData(const QByteArray& ipPacket) {
    if (m_transport == TRANSPORT_NONE) return -1;

    QByteArray pkt = ipPacket;
    if (m_cipher) {
        pkt = m_cipher->encrypt(pkt);
    }

    if (g_verboseLog)
        LogManager::instance().logDetail(QString("[peer] sendData peerId=%1 transport=%2 size=%3").arg(m_peerId).arg(transportName(m_transport)).arg(pkt.size()));

    if (m_transport == TRANSPORT_RELAY_TCP) {
        if (m_tcpSender) {
            m_tcpSender(m_peerId, pkt);
            return pkt.size();
        }
        return -1;
    }

    if (m_transport == TRANSPORT_RELAY_RAW_UDP) {
        if (!m_rawUdpTunnel) return -1;
        return m_rawUdpTunnel->send(pkt);
    }

    if (!m_kcpTunnel) return -1;
    return m_kcpTunnel->send(pkt);
}

void P2PPeer::onTcpRelayDataReceived() {
    m_tcpRelayLastRecv = currentTimeMs();
}

void P2PPeer::sendTcpRelayKeepalive() {
    if (m_transport != TRANSPORT_RELAY_TCP || !m_tcpSender) return;
    uint32_t now = currentTimeMs();
    if (now - m_tcpRelayLastSend >= static_cast<uint32_t>(TCP_RELAY_KEEPALIVE_MS)) {
        static const char marker[1] = {0};
        m_tcpSender(m_peerId, QByteArray(marker, 0));
        m_tcpRelayLastSend = now;
    }
}

bool P2PPeer::isTcpRelayDead() const {
    if (m_transport != TRANSPORT_RELAY_TCP) return false;
    uint32_t elapsed = currentTimeMs() - m_tcpRelayLastRecv;
    return elapsed > static_cast<uint32_t>(TCP_RELAY_DEAD_MS);
}

int P2PPeer::latencyMs() const {
    if (m_transport == TRANSPORT_RELAY_TCP)
        return m_tcpRtt;
    if (m_transport == TRANSPORT_RELAY_RAW_UDP)
        return m_rawUdpTunnel ? m_rawUdpTunnel->getRttMs() : -1;
    if (m_kcpTunnel)
        return m_kcpTunnel->getRttMs();
    return -1;
}

void P2PPeer::sendLatencyPing() {
    if (m_transport != TRANSPORT_RELAY_TCP || !m_tcpSender) return;
    m_latencyPingSentTime = currentTimeMs();
    QByteArray probe(6, '\0');
    probe[0] = static_cast<char>(LATENCY_PROBE_MARKER);
    probe[1] = static_cast<char>(LATENCY_PROBE_PING);
    uint32_t ts = m_latencyPingSentTime;
    probe[2] = static_cast<char>((ts >> 24) & 0xFF);
    probe[3] = static_cast<char>((ts >> 16) & 0xFF);
    probe[4] = static_cast<char>((ts >> 8) & 0xFF);
    probe[5] = static_cast<char>(ts & 0xFF);
    m_tcpSender(m_peerId, probe);
}

bool P2PPeer::handleLatencyProbe(const QByteArray& data) {
    if (data.size() < 6) return false;
    uint8_t marker = static_cast<uint8_t>(data[0]);
    if (marker != LATENCY_PROBE_MARKER) return false;

    uint8_t probeType = static_cast<uint8_t>(data[1]);
    LogManager::instance().logDetail(QString("[peer] handleLatencyProbe peerId=%1 type=%2").arg(m_peerId).arg(probeType == LATENCY_PROBE_PING ? "PING" : "PONG"));
    if (probeType == LATENCY_PROBE_PING) {
        QByteArray pong(6, '\0');
        pong[0] = static_cast<char>(LATENCY_PROBE_MARKER);
        pong[1] = static_cast<char>(LATENCY_PROBE_PONG);
        pong[2] = data[2]; pong[3] = data[3];
        pong[4] = data[4]; pong[5] = data[5];
        emit latencyPongReply(m_peerId, pong);
        return true;
    }
    if (probeType == LATENCY_PROBE_PONG) {
        uint32_t sentTs = (static_cast<uint8_t>(data[2]) << 24) |
                          (static_cast<uint8_t>(data[3]) << 16) |
                          (static_cast<uint8_t>(data[4]) << 8) |
                           static_cast<uint8_t>(data[5]);
        uint32_t now = currentTimeMs();
        m_tcpRtt = static_cast<int>(now - sentTs);
        if (m_tcpRtt < 0) m_tcpRtt = 0;
        LogManager::instance().logDetail(QString("[peer] latency PONG peerId=%1 rtt=%2 ms").arg(m_peerId).arg(m_tcpRtt));
        return true;
    }
    return false;
}

} // namespace VLan

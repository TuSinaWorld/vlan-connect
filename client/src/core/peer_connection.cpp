#include "peer_connection.h"
#include "kcp_tunnel.h"
#include "raw_udp_tunnel.h"
#include "net_common.h"
#include "../ui/log_manager.h"

namespace VLan {

PeerConnection::PeerConnection(uint32_t peerId, uint32_t virtualIP,
                 const QString& name, QObject* parent)
    : QObject(parent),
      m_peerId(peerId), m_virtualIP(virtualIP), m_name(name)
{
    uint32_t now = currentTimeMs();
    for (int i = 0; i < 3; ++i) {
        m_transport[i] = TRANSPORT_NONE;
        m_kcpTunnel[i] = nullptr;
        m_rawUdpTunnel[i] = nullptr;
        m_tcpRelayLastRecv[i] = now;
        m_tcpRelayLastSend[i] = now;
        m_rtt[i] = -1;
        m_latencyPingSentTime[i] = 0;
        m_latencyLastReply[i] = 0;
    }
}

PeerConnection::~PeerConnection() {}

TransportType PeerConnection::transport(TrafficClass cls) const {
    int idx = static_cast<int>(cls);
    if (idx < 0 || idx > 2) idx = TRAFFIC_UDP;
    return m_transport[idx];
}

void PeerConnection::setKcpTunnel(TrafficClass cls, KcpTunnel* tunnel) {
    int idx = static_cast<int>(cls);
    if (idx < 0 || idx > 2) idx = TRAFFIC_UDP;
    m_kcpTunnel[idx] = tunnel;
    if (tunnel) {
        connect(tunnel, &KcpTunnel::dataReceived,
                this, [this, cls](QByteArray data) {
            onTunnelDataReceived(cls, data);
        });
    }
}

KcpTunnel* PeerConnection::kcpTunnel(TrafficClass cls) const {
    int idx = static_cast<int>(cls);
    if (idx < 0 || idx > 2) idx = TRAFFIC_UDP;
    return m_kcpTunnel[idx];
}

void PeerConnection::clearKcpTunnel(TrafficClass cls) {
    int idx = static_cast<int>(cls);
    if (idx < 0 || idx > 2) idx = TRAFFIC_UDP;
    m_kcpTunnel[idx] = nullptr;
}

void PeerConnection::setRawUdpTunnel(TrafficClass cls, RawUdpTunnel* tunnel) {
    int idx = static_cast<int>(cls);
    if (idx < 0 || idx > 2) idx = TRAFFIC_UDP;
    m_rawUdpTunnel[idx] = tunnel;
    if (tunnel) {
        connect(tunnel, &RawUdpTunnel::dataReceived,
                this, [this, cls](QByteArray data) {
            onTunnelDataReceived(cls, data);
        });
    }
}

RawUdpTunnel* PeerConnection::rawUdpTunnel(TrafficClass cls) const {
    int idx = static_cast<int>(cls);
    if (idx < 0 || idx > 2) idx = TRAFFIC_UDP;
    return m_rawUdpTunnel[idx];
}

void PeerConnection::clearRawUdpTunnel(TrafficClass cls) {
    int idx = static_cast<int>(cls);
    if (idx < 0 || idx > 2) idx = TRAFFIC_UDP;
    m_rawUdpTunnel[idx] = nullptr;
}

void PeerConnection::onTunnelDataReceived(TrafficClass cls, QByteArray data) {
    if (handleLatencyProbe(cls, data))
        return;
    emit dataReceived(data);
}

void PeerConnection::setTransport(TrafficClass cls, TransportType t) {
    int idx = static_cast<int>(cls);
    if (idx < 0 || idx > 2) idx = TRAFFIC_UDP;
    if (m_transport[idx] != t) {
        m_transport[idx] = t;
        emit transportChanged(m_peerId, cls, t);
    }
}

void PeerConnection::clearTransport(TrafficClass cls) {
    int idx = static_cast<int>(cls);
    if (idx < 0 || idx > 2) idx = TRAFFIC_UDP;
    uint32_t now = currentTimeMs();
    m_transport[idx] = TRANSPORT_NONE;
    m_rtt[idx] = -1;
    m_latencyPingSentTime[idx] = 0;
    m_latencyLastReply[idx] = 0;
    m_tcpRelayLastRecv[idx] = now;
    m_tcpRelayLastSend[idx] = now;
    emit transportChanged(m_peerId, cls, TRANSPORT_NONE);
}

int PeerConnection::sendData(const QByteArray& ipPacket) {
    TrafficClass cls = trafficClassFromIpPacket(
        reinterpret_cast<const uint8_t*>(ipPacket.constData()), ipPacket.size());
    int idx = static_cast<int>(cls);
    if (idx < 0 || idx > 2) idx = TRAFFIC_UDP;
    TransportType t = m_transport[idx];
    if (t == TRANSPORT_NONE) return -1;

    QByteArray pkt = ipPacket;

    if (g_verboseLog)
        LogManager::instance().logDetail(QString("[peer] sendData peerId=%1 class=%2 transport=%3 size=%4").arg(m_peerId).arg(idx).arg(transportName(t)).arg(pkt.size()));

    if (t == TRANSPORT_RELAY_TCP) {
        if (m_tcpSender) {
            m_tcpSender(m_peerId, cls, pkt);
            return pkt.size();
        }
        return -1;
    }

    if (t == TRANSPORT_RELAY_RAW_UDP) {
        if (!m_rawUdpTunnel[idx]) return -1;
        return m_rawUdpTunnel[idx]->send(pkt);
    }

    if (!m_kcpTunnel[idx]) return -1;
    return m_kcpTunnel[idx]->send(pkt);
}

void PeerConnection::onTcpRelayDataReceived(TrafficClass cls) {
    int idx = static_cast<int>(cls);
    if (idx < 0 || idx > 2) idx = TRAFFIC_TCP;
    m_tcpRelayLastRecv[idx] = currentTimeMs();
}

void PeerConnection::sendTcpRelayKeepalive() {
    uint32_t now = currentTimeMs();
    for (int idx = TRAFFIC_TCP; idx <= TRAFFIC_UDP; ++idx) {
        if (m_transport[idx] != TRANSPORT_RELAY_TCP || !m_tcpSender) continue;
        if (now - m_tcpRelayLastSend[idx] >= static_cast<uint32_t>(TCP_RELAY_KEEPALIVE_MS)) {
            static const char marker[1] = {0};
            m_tcpSender(m_peerId, static_cast<TrafficClass>(idx), QByteArray(marker, 0));
            m_tcpRelayLastSend[idx] = now;
        }
    }
}

bool PeerConnection::isTcpRelayDead(TrafficClass cls) const {
    int idx = static_cast<int>(cls);
    if (idx < 0 || idx > 2) idx = TRAFFIC_TCP;
    if (m_transport[idx] != TRANSPORT_RELAY_TCP)
        return false;

    uint32_t now = currentTimeMs();
    return now - m_tcpRelayLastRecv[idx] > static_cast<uint32_t>(TCP_RELAY_DEAD_MS);
}

bool PeerConnection::isTcpRelayDead() const {
    for (int idx = TRAFFIC_TCP; idx <= TRAFFIC_UDP; ++idx) {
        if (isTcpRelayDead(static_cast<TrafficClass>(idx)))
            return true;
    }
    return false;
}

int PeerConnection::latencyMs(TrafficClass cls) const {
    int idx = static_cast<int>(cls);
    if (idx < 0 || idx > 2) idx = TRAFFIC_UDP;
    if (m_latencyLastReply[idx] == 0)
        return -1;
    uint32_t now = currentTimeMs();
    if (now - m_latencyLastReply[idx] > LATENCY_STALE_MS)
        return -1;
    return m_rtt[idx];
}

void PeerConnection::sendLatencyPing(TrafficClass cls) {
    int idx = static_cast<int>(cls);
    if (idx < 0 || idx > 2) idx = TRAFFIC_UDP;
    if (m_transport[idx] == TRANSPORT_NONE) return;
    m_latencyPingSentTime[idx] = currentTimeMs();
    QByteArray probe(6, '\0');
    probe[0] = static_cast<char>(LATENCY_PROBE_MARKER);
    probe[1] = static_cast<char>(LATENCY_PROBE_PING);
    uint32_t ts = m_latencyPingSentTime[idx];
    probe[2] = static_cast<char>((ts >> 24) & 0xFF);
    probe[3] = static_cast<char>((ts >> 16) & 0xFF);
    probe[4] = static_cast<char>((ts >> 8) & 0xFF);
    probe[5] = static_cast<char>(ts & 0xFF);
    sendControlPacket(cls, probe);
}

bool PeerConnection::handleLatencyProbe(TrafficClass cls, const QByteArray& data) {
    if (data.size() < 6) return false;
    uint8_t marker = static_cast<uint8_t>(data[0]);
    if (marker != LATENCY_PROBE_MARKER) return false;

    uint8_t probeType = static_cast<uint8_t>(data[1]);
    int idx = static_cast<int>(cls);
    if (idx < 0 || idx > 2) idx = TRAFFIC_UDP;
    LogManager::instance().logDetail(QString("[peer] handleLatencyProbe peerId=%1 class=%2 type=%3")
        .arg(m_peerId).arg(idx).arg(probeType == LATENCY_PROBE_PING ? "PING" : "PONG"));
    if (probeType == LATENCY_PROBE_PING) {
        QByteArray pong(6, '\0');
        pong[0] = static_cast<char>(LATENCY_PROBE_MARKER);
        pong[1] = static_cast<char>(LATENCY_PROBE_PONG);
        pong[2] = data[2]; pong[3] = data[3];
        pong[4] = data[4]; pong[5] = data[5];
        sendControlPacket(cls, pong);
        return true;
    }
    if (probeType == LATENCY_PROBE_PONG) {
        uint32_t sentTs = (static_cast<uint8_t>(data[2]) << 24) |
                          (static_cast<uint8_t>(data[3]) << 16) |
                          (static_cast<uint8_t>(data[4]) << 8) |
                           static_cast<uint8_t>(data[5]);
        uint32_t now = currentTimeMs();
        m_rtt[idx] = static_cast<int>(now - sentTs);
        if (m_rtt[idx] < 0) m_rtt[idx] = 0;
        m_latencyLastReply[idx] = now;
        LogManager::instance().logDetail(QString("[peer] latency PONG peerId=%1 class=%2 rtt=%3 ms")
            .arg(m_peerId).arg(idx).arg(m_rtt[idx]));
        return true;
    }
    return false;
}

bool PeerConnection::sendControlPacket(TrafficClass cls, const QByteArray& data) {
    int idx = static_cast<int>(cls);
    if (idx < 0 || idx > 2) idx = TRAFFIC_UDP;
    TransportType t = m_transport[idx];
    if (t == TRANSPORT_RELAY_TCP) {
        if (!m_tcpSender) return false;
        m_tcpSender(m_peerId, cls, data);
        return true;
    }
    if (t == TRANSPORT_RELAY_RAW_UDP) {
        if (!m_rawUdpTunnel[idx]) return false;
        return m_rawUdpTunnel[idx]->send(data) >= 0;
    }
    if (t == TRANSPORT_RELAY_KCP) {
        if (!m_kcpTunnel[idx]) return false;
        return m_kcpTunnel[idx]->send(data) >= 0;
    }
    return false;
}

} // namespace VLan

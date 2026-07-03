#ifndef VLAN_PEER_CONNECTION_H
#define VLAN_PEER_CONNECTION_H

#include <QObject>
#include <QTimer>
#include <functional>
#include "protocol.h"

namespace VLan {

class KcpTunnel;
class RawUdpTunnel;

static const uint8_t LATENCY_PROBE_MARKER = 0xFE;
static const uint8_t LATENCY_PROBE_PING   = 0x01;
static const uint8_t LATENCY_PROBE_PONG   = 0x02;
static const uint32_t LATENCY_STALE_MS    = 10000;

/*
 * Represents one remote peer's relay connection state.
 * Owns its KCP tunnel (if any) and tracks transport type.
 */
class PeerConnection : public QObject {
    Q_OBJECT
public:
    explicit PeerConnection(uint32_t peerId, uint32_t virtualIP,
                            const QString& name, QObject* parent = nullptr);
    ~PeerConnection();

    uint32_t      peerId()     const { return m_peerId; }
    uint32_t      virtualIP()  const { return m_virtualIP; }
    QString       name()       const { return m_name; }
    TransportType transport()  const { return transport(TRAFFIC_TCP); }
    TransportType transport(TrafficClass cls) const;
    void setKcpTunnel(TrafficClass cls, KcpTunnel* tunnel);
    KcpTunnel* kcpTunnel(TrafficClass cls) const;
    void clearKcpTunnel(TrafficClass cls);

    void setRawUdpTunnel(TrafficClass cls, RawUdpTunnel* tunnel);
    RawUdpTunnel* rawUdpTunnel(TrafficClass cls) const;
    void clearRawUdpTunnel(TrafficClass cls);

    using TcpSendFunc = std::function<void(uint32_t dstPeerId, TrafficClass cls, const QByteArray& data)>;
    void setTcpRelaySender(TcpSendFunc sender) { m_tcpSender = sender; }

    void setTransport(TrafficClass cls, TransportType t);
    void clearTransport(TrafficClass cls);

    int  sendData(const QByteArray& ipPacket);

    void onTcpRelayDataReceived(TrafficClass cls = TRAFFIC_TCP);
    void sendTcpRelayKeepalive();
    bool isTcpRelayDead(TrafficClass cls) const;
    bool isTcpRelayDead() const;
    uint32_t tcpRelayLastRecv(TrafficClass cls = TRAFFIC_TCP) const {
        int idx = static_cast<int>(cls);
        if (idx < 0 || idx > 2) idx = TRAFFIC_TCP;
        return m_tcpRelayLastRecv[idx];
    }

    int  latencyMs(TrafficClass cls) const;
    void sendLatencyPing(TrafficClass cls);
    bool handleLatencyProbe(TrafficClass cls, const QByteArray& data);

signals:
    void dataReceived(QByteArray ipPacket);
    void transportChanged(uint32_t peerId, VLan::TrafficClass cls, VLan::TransportType newType);

private slots:
    void onTunnelDataReceived(VLan::TrafficClass cls, QByteArray data);

private:
    bool sendControlPacket(TrafficClass cls, const QByteArray& data);

    uint32_t      m_peerId;
    uint32_t      m_virtualIP;
    QString       m_name;
    TransportType m_transport[3];
    KcpTunnel*    m_kcpTunnel[3];
    RawUdpTunnel* m_rawUdpTunnel[3];
    TcpSendFunc   m_tcpSender;
    uint32_t      m_tcpRelayLastRecv[3];
    uint32_t      m_tcpRelayLastSend[3];
    int           m_rtt[3];
    uint32_t      m_latencyPingSentTime[3];
    uint32_t      m_latencyLastReply[3];
};

} // namespace VLan
#endif // VLAN_PEER_CONNECTION_H

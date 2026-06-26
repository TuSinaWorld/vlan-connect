#ifndef VLAN_P2P_PEER_H
#define VLAN_P2P_PEER_H

#include <QObject>
#include <QHostAddress>
#include <QTimer>
#include <functional>
#include "protocol.h"
#include "payload_cipher.h"

namespace VLan {

class KcpTunnel;
class RawUdpTunnel;

static const uint8_t LATENCY_PROBE_MARKER = 0xFE;
static const uint8_t LATENCY_PROBE_PING   = 0x01;
static const uint8_t LATENCY_PROBE_PONG   = 0x02;

/*
 * Represents the connection state to one remote peer.
 *
 * Each peer goes through: Idle -> Punching -> Connected (P2P or Relay).
 * Owns its KCP tunnel (if any) and tracks transport type.
 */
class P2PPeer : public QObject {
    Q_OBJECT
public:
    explicit P2PPeer(uint32_t peerId, uint32_t virtualIP,
                     const QString& name, QObject* parent = nullptr);
    ~P2PPeer();

    uint32_t      peerId()     const { return m_peerId; }
    uint32_t      virtualIP()  const { return m_virtualIP; }
    QString       name()       const { return m_name; }
    TransportType transport()  const { return m_transport; }
    NatType       natType()    const { return m_natType; }

    void setNatType(NatType t) { m_natType = t; }
    void setPublicEndpoint(const QHostAddress& addr, quint16 port);
    QHostAddress publicAddress() const { return m_publicAddr; }
    quint16      publicPort()    const { return m_publicPort; }

    void setKcpTunnel(KcpTunnel* tunnel);
    KcpTunnel* kcpTunnel() const { return m_kcpTunnel; }

    void setRawUdpTunnel(RawUdpTunnel* tunnel);
    RawUdpTunnel* rawUdpTunnel() const { return m_rawUdpTunnel; }

    void setCipher(PayloadCipher* cipher) { m_cipher = cipher; }

    using TcpSendFunc = std::function<void(uint32_t dstPeerId, const QByteArray& data)>;
    void setTcpRelaySender(TcpSendFunc sender) { m_tcpSender = sender; }

    void setTransport(TransportType t);

    int  sendData(const QByteArray& ipPacket);

    void onTcpRelayDataReceived();
    void sendTcpRelayKeepalive();
    bool isTcpRelayDead() const;
    uint32_t tcpRelayLastRecv() const { return m_tcpRelayLastRecv; }

    int  latencyMs() const;
    void sendLatencyPing();
    bool handleLatencyProbe(const QByteArray& data);

signals:
    void dataReceived(QByteArray ipPacket);
    void transportChanged(uint32_t peerId, VLan::TransportType newType);
    void latencyPongReply(uint32_t peerId, QByteArray pongData);

private slots:
    void onTunnelDataReceived(QByteArray data);

private:
    uint32_t      m_peerId;
    uint32_t      m_virtualIP;
    QString       m_name;
    NatType       m_natType;
    TransportType m_transport;
    QHostAddress  m_publicAddr;
    quint16       m_publicPort;
    KcpTunnel*    m_kcpTunnel;
    RawUdpTunnel* m_rawUdpTunnel;
    TcpSendFunc   m_tcpSender;
    PayloadCipher* m_cipher;
    uint32_t      m_tcpRelayLastRecv;
    uint32_t      m_tcpRelayLastSend;
    int           m_tcpRtt;
    uint32_t      m_latencyPingSentTime;
};

} // namespace VLan
#endif // VLAN_P2P_PEER_H

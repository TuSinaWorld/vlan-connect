#ifndef VLAN_KCP_TUNNEL_H
#define VLAN_KCP_TUNNEL_H

#include <QObject>
#include <QUdpSocket>
#include <QHostAddress>
#include <QByteArray>
#include <QTimer>
#include "ikcp.h"
#include "protocol.h"

namespace VLan {

class FecEncoder;
class FecDecoder;

/*
 * A single KCP session between this client and one peer.
 *
 * Wraps the KCP library to provide reliable, ordered delivery over UDP.
 * The caller supplies a shared QUdpSocket.
 *
 * Must be fed incoming data via feedInput().
 * Includes built-in keepalive: sends a 1-byte probe through KCP if idle,
 * and declares the tunnel dead after KCP_DEAD_TIMEOUT_MS of silence.
 */
class KcpTunnel : public QObject {
    Q_OBJECT
public:
    KcpTunnel(uint32_t conv, QUdpSocket* socket,
              const QHostAddress& peerAddr, quint16 peerPort,
              FecMode fecMode = FEC_NONE,
              uint16_t mtu = ROOM_MTU_DEFAULT,
              QObject* parent = nullptr);
    ~KcpTunnel();

    void feedInput(const char* data, int len);
    int  send(const QByteArray& data);
    void update();

    void setPeerEndpoint(const QHostAddress& addr, quint16 port);
    QHostAddress peerAddress() const { return m_peerAddr; }
    quint16      peerPort()    const { return m_peerPort; }

    void setRelayMode(uint32_t srcPeerId, uint32_t dstPeerId);
    bool isRelay() const { return m_relayMode; }

    int  waitSendCount() const;
    bool isAlive() const;
    int  getRttMs() const;
    uint32_t lastRecvTime() const { return m_lastRecvTime; }

signals:
    void dataReceived(QByteArray data);
    void tunnelDead();

private:
    static int kcpOutput(const char* buf, int len, ikcpcb* kcp, void* user);
    void sendKcpPacket(const QByteArray& payload);
    void tryRecv();

    ikcpcb*       m_kcp;
    QUdpSocket*   m_socket;
    QHostAddress  m_peerAddr;
    quint16       m_peerPort;

    bool     m_relayMode;
    uint32_t m_relaySrcPeerId;
    uint32_t m_relayDstPeerId;

    uint32_t m_lastRecvTime;
    uint32_t m_lastSendTime;
    bool     m_dead;

    FecMode      m_fecMode;
    FecEncoder*  m_fecEncoder;
    FecDecoder*  m_fecDecoder;
};

} // namespace VLan
#endif // VLAN_KCP_TUNNEL_H

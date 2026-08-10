#ifndef VLAN_RAW_UDP_TUNNEL_H
#define VLAN_RAW_UDP_TUNNEL_H

#include <QObject>
#include <QUdpSocket>
#include <QHostAddress>
#include <QByteArray>
#include <QMap>
#include <functional>
#include "protocol.h"

namespace VLan {

class FecEncoder;
class FecDecoder;

/*
 * Raw UDP tunnel for one peer, parallel to KcpTunnel.
 *
 * Sends/receives IP packets over a UDP relay path without any
 * reliability layer (no KCP).  Includes:
 *   - Fragmentation / reassembly (6-byte FragHeader)
 *   - Keepalive probing through the relay path
 *   - Dead-peer detection (RAW_UDP_DEAD_TIMEOUT_MS)
 */
class RawUdpTunnel : public QObject {
    Q_OBJECT
public:
    RawUdpTunnel(QUdpSocket* socket,
                 const QHostAddress& peerAddr, quint16 peerPort,
                 FecMode fecMode = FEC_NONE,
                 uint16_t mtu = ROOM_MTU_DEFAULT,
                 TrafficClass trafficClass = TRAFFIC_UDP,
                 bool secureFrames = false,
                 QObject* parent = nullptr);
    ~RawUdpTunnel();

    int  send(const QByteArray& ipPacket);
    void feedInput(const char* data, int len);
    void update();

    void setPeerEndpoint(const QHostAddress& addr, quint16 port);
    QHostAddress peerAddress() const { return m_peerAddr; }
    quint16      peerPort()    const { return m_peerPort; }

    void setRelayMode(uint32_t srcPeerId, uint32_t dstPeerId);
    using DatagramSender = std::function<void(const QByteArray&, const QHostAddress&, quint16)>;
    void setDatagramSender(DatagramSender sender) { m_datagramSender = sender; }
    bool isRelay() const { return m_relayMode; }

    bool isAlive() const;
    uint32_t lastRecvTime() const { return m_lastRecvTime; }
    int  getRttMs() const;

signals:
    void dataReceived(QByteArray data);
    void tunnelDead();

private:
    struct ReassemblyEntry {
        uint16_t totalLen;
        uint8_t  fragTotal;
        uint8_t  receivedCount;
        uint32_t createTime;
        QMap<uint8_t, QByteArray> fragments;
    };

    void sendFragment(const char* data, int len, uint8_t fragIndex,
                      uint8_t fragTotal, uint16_t msgId, uint16_t totalLen);
    void sendRawPacket(const QByteArray& payload);
    void processFrag(const char* data, int len);
    void sendKeepalive();
    void sendLatencyPing();
    void sendLatencyPong(const char* timestampData);
    void cleanupStaleEntries();
    void removeReassemblyEntry(uint16_t msgId);
    bool ensureReassemblyCapacity(size_t incomingBytes, bool newMessage);
    int  maxFragmentPayload() const;

    QUdpSocket*   m_socket;
    QHostAddress  m_peerAddr;
    quint16       m_peerPort;

    bool     m_relayMode;
    uint32_t m_relaySrcPeerId;
    uint32_t m_relayDstPeerId;
    TrafficClass m_trafficClass;
    DatagramSender m_datagramSender;

    uint16_t m_nextMsgId;
    uint32_t m_lastRecvTime;
    uint32_t m_lastSendTime;
    bool     m_dead;

    QMap<uint16_t, ReassemblyEntry> m_reassembly;
    size_t m_reassemblyBytes;

    FecMode      m_fecMode;
    FecEncoder*  m_fecEncoder;
    FecDecoder*  m_fecDecoder;
    uint16_t     m_roomMtu;
    bool         m_secureFrames;

    int          m_rttMs;
};

} // namespace VLan
#endif // VLAN_RAW_UDP_TUNNEL_H

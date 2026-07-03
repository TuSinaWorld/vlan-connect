#ifndef VLAN_CLI_PEER_H
#define VLAN_CLI_PEER_H

#include "cli_common.h"
#include "cli_fec.h"
#include "ikcp.h"
#include <functional>
#include <map>

namespace VLan {

typedef std::function<void(uint32_t peerId, const Buffer& ipPacket)> PeerDataCallback;
typedef std::function<void(uint32_t peerId, TrafficClass cls)> TunnelDeadCallback;
typedef std::function<void(uint32_t peerId, TransportType newType)> TransportChangedCallback;

typedef std::function<void(const uint8_t* data, size_t len,
                           uint32_t dstIP, uint16_t dstPort)> UdpSendFunc;
typedef std::function<void(uint32_t dstPeerId, TrafficClass cls,
                           const Buffer& data)> TcpRelaySendFunc;

class CliKcpTunnel {
public:
    CliKcpTunnel(uint32_t conv, UdpSendFunc udpSend,
                 uint32_t peerIP, uint16_t peerPort,
                 FecMode fecMode = FEC_NONE,
                 uint16_t mtu = ROOM_MTU_DEFAULT,
                 KcpProfile profile = KCP_PROFILE_REALTIME,
                 TrafficClass trafficClass = TRAFFIC_UDP,
                 bool secureFrames = false);
    ~CliKcpTunnel();

    void feedInput(const char* data, int len);
    int  send(const Buffer& data);
    void update();

    uint32_t peerIP()   const { return m_peerIP; }
    uint16_t peerPort() const { return m_peerPort; }

    void setRelayMode(uint32_t srcPeerId, uint32_t dstPeerId);
    int  getRttMs() const;

    typedef std::function<void(const Buffer& ipPacket)> DataRecvFunc;
    typedef std::function<void()> DeadFunc;
    void setOnDataReceived(DataRecvFunc cb) { m_onData = cb; }
    void setOnDead(DeadFunc cb) { m_onDead = cb; }

private:
    static int kcpOutput(const char* buf, int len, ikcpcb* kcp, void* user);
    void sendKcpPacket(const Buffer& payload);
    void tryRecv();

    ikcpcb*       m_kcp;
    UdpSendFunc   m_udpSend;
    uint32_t      m_peerIP;
    uint16_t      m_peerPort;
    TrafficClass  m_trafficClass;
    KcpProfile    m_profile;

    bool     m_relayMode;
    uint32_t m_relaySrcPeerId;
    uint32_t m_relayDstPeerId;

    uint32_t m_lastRecvTime;
    uint32_t m_lastSendTime;
    bool     m_dead;

    FecMode         m_fecMode;
    CliFecEncoder*  m_fecEncoder;
    CliFecDecoder*  m_fecDecoder;

    DataRecvFunc m_onData;
    DeadFunc     m_onDead;
};

class CliRawUdpTunnel {
public:
    CliRawUdpTunnel(UdpSendFunc udpSend,
                    uint32_t peerIP, uint16_t peerPort,
                    FecMode fecMode = FEC_NONE,
                    uint16_t mtu = ROOM_MTU_DEFAULT,
                    TrafficClass trafficClass = TRAFFIC_UDP,
                    bool secureFrames = false);
    ~CliRawUdpTunnel();

    int  send(const Buffer& ipPacket);
    void feedInput(const char* data, int len);
    void update();

    void setRelayMode(uint32_t srcPeerId, uint32_t dstPeerId);
    int  getRttMs() const { return m_rttMs; }

    typedef std::function<void(const Buffer& ipPacket)> DataRecvFunc;
    typedef std::function<void()> DeadFunc;
    void setOnDataReceived(DataRecvFunc cb) { m_onData = cb; }
    void setOnDead(DeadFunc cb) { m_onDead = cb; }

private:
    struct ReassemblyEntry {
        uint16_t totalLen;
        uint8_t  fragTotal;
        uint8_t  receivedCount;
        uint32_t createTime;
        std::map<uint8_t, Buffer> fragments;
    };

    void sendFragment(const char* data, int len, uint8_t fragIndex,
                      uint8_t fragTotal, uint16_t msgId, uint16_t totalLen);
    void sendRawPacket(const Buffer& payload);
    void processFrag(const char* data, int len);
    void cleanupStaleEntries();
    int  maxFragmentPayload() const;

    UdpSendFunc   m_udpSend;
    uint32_t      m_peerIP;
    uint16_t      m_peerPort;
    TrafficClass  m_trafficClass;

    bool     m_relayMode;
    uint32_t m_relaySrcPeerId;
    uint32_t m_relayDstPeerId;

    uint16_t m_nextMsgId;
    uint32_t m_lastRecvTime;
    uint32_t m_lastSendTime;
    bool     m_dead;
    int      m_rttMs;

    std::map<uint16_t, ReassemblyEntry> m_reassembly;

    FecMode         m_fecMode;
    CliFecEncoder*  m_fecEncoder;
    CliFecDecoder*  m_fecDecoder;
    uint16_t        m_roomMtu;
    bool            m_secureFrames;

    DataRecvFunc m_onData;
    DeadFunc     m_onDead;
};

static const uint8_t CLI_LATENCY_PROBE_MARKER = 0xFE;
static const uint8_t CLI_LATENCY_PROBE_PING   = 0x01;
static const uint8_t CLI_LATENCY_PROBE_PONG   = 0x02;
static const uint32_t CLI_LATENCY_STALE_MS    = 10000;

class CliPeerConnection {
public:
    CliPeerConnection(uint32_t peerId, uint32_t virtualIP, const std::string& name);
    ~CliPeerConnection();

    uint32_t      peerId()     const { return m_peerId; }
    uint32_t      virtualIP()  const { return m_virtualIP; }
    std::string   name()       const { return m_name; }
    TransportType transport()  const { return transport(TRAFFIC_TCP); }
    TransportType transport(TrafficClass cls) const;
    void setKcpTunnel(TrafficClass cls, CliKcpTunnel* tunnel);
    CliKcpTunnel* kcpTunnel(TrafficClass cls) const;
    void clearKcpTunnel(TrafficClass cls);

    void setRawUdpTunnel(TrafficClass cls, CliRawUdpTunnel* tunnel);
    CliRawUdpTunnel* rawUdpTunnel(TrafficClass cls) const;
    void clearRawUdpTunnel(TrafficClass cls);

    void setTransport(TrafficClass cls, TransportType t);
    void clearTransport(TrafficClass cls);
    void setTcpRelaySender(TcpRelaySendFunc sender) { m_tcpSender = sender; }

    int sendData(const Buffer& ipPacket);

    void onTcpRelayDataReceived(TrafficClass cls);
    void sendTcpRelayKeepalive();
    bool isTcpRelayDead(TrafficClass cls) const;
    bool isTcpRelayDead() const;
    int  latencyMs(TrafficClass cls) const;
    int  latencyMs() const;
    void sendLatencyPing(TrafficClass cls);
    void sendLatencyPing();
    bool handleLatencyProbe(TrafficClass cls, const Buffer& data);
    bool handleLatencyProbe(const Buffer& data);

    void setOnDataReceived(PeerDataCallback cb) { m_onData = cb; }

private:
    void onTunnelDataReceived(TrafficClass cls, const Buffer& data);
    bool sendControlPacket(TrafficClass cls, const Buffer& data);

    uint32_t      m_peerId;
    uint32_t      m_virtualIP;
    std::string   m_name;
    TransportType m_transport[3];
    CliKcpTunnel*    m_kcpTunnel[3];
    CliRawUdpTunnel* m_rawUdpTunnel[3];
    TcpRelaySendFunc m_tcpSender;

    PeerDataCallback    m_onData;

    uint32_t m_tcpRelayLastRecv[3];
    uint32_t m_tcpRelayLastSend[3];
    int      m_rtt[3];
    uint32_t m_latencyPingSentTime[3];
    uint32_t m_latencyLastReply[3];
};

} // namespace VLan
#endif // VLAN_CLI_PEER_H

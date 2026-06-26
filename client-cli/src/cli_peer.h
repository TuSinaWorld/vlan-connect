#ifndef VLAN_CLI_PEER_H
#define VLAN_CLI_PEER_H

#include "cli_common.h"
#include "cli_fec.h"
#include "ikcp.h"
#include <functional>
#include <map>

namespace VLan {

/* Callback: received decrypted IP packet from peer */
typedef std::function<void(uint32_t peerId, const Buffer& ipPacket)> PeerDataCallback;
/* Callback: tunnel declared dead */
typedef std::function<void(uint32_t peerId)> TunnelDeadCallback;
/* Callback: transport type changed */
typedef std::function<void(uint32_t peerId, TransportType newType)> TransportChangedCallback;
/* Callback: latency pong needs to be relayed back */
typedef std::function<void(uint32_t peerId, const Buffer& pongData)> LatencyPongCallback;

/* UDP send primitive used by tunnels */
typedef std::function<void(const uint8_t* data, size_t len,
                           uint32_t dstIP, uint16_t dstPort)> UdpSendFunc;
/* TCP relay send primitive */
typedef std::function<void(uint32_t dstPeerId, const Buffer& data)> TcpRelaySendFunc;

// ======================== CliKcpTunnel ========================

class CliKcpTunnel {
public:
    CliKcpTunnel(uint32_t conv, UdpSendFunc udpSend,
                 uint32_t peerIP, uint16_t peerPort,
                 FecMode fecMode = FEC_NONE,
                 uint16_t mtu = ROOM_MTU_DEFAULT);
    ~CliKcpTunnel();

    void feedInput(const char* data, int len);
    int  send(const Buffer& data);
    void update();

    void setPeerEndpoint(uint32_t ip, uint16_t port) { m_peerIP = ip; m_peerPort = port; }
    uint32_t peerIP()   const { return m_peerIP; }
    uint16_t peerPort() const { return m_peerPort; }

    void setRelayMode(uint32_t srcPeerId, uint32_t dstPeerId);
    bool isRelay() const { return m_relayMode; }

    bool isAlive() const;
    int  getRttMs() const;
    uint32_t lastRecvTime() const { return m_lastRecvTime; }

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

// ======================== CliRawUdpTunnel ========================

class CliRawUdpTunnel {
public:
    CliRawUdpTunnel(UdpSendFunc udpSend,
                    uint32_t peerIP, uint16_t peerPort,
                    FecMode fecMode = FEC_NONE,
                    uint16_t mtu = ROOM_MTU_DEFAULT);
    ~CliRawUdpTunnel();

    int  send(const Buffer& ipPacket);
    void feedInput(const char* data, int len);
    void update();

    void setPeerEndpoint(uint32_t ip, uint16_t port) { m_peerIP = ip; m_peerPort = port; }
    void setRelayMode(uint32_t srcPeerId, uint32_t dstPeerId);
    bool isRelay() const { return m_relayMode; }
    bool isAlive() const;
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

    DataRecvFunc m_onData;
    DeadFunc     m_onDead;
};

// ======================== CliP2PPeer ========================

static const uint8_t CLI_LATENCY_PROBE_MARKER = 0xFE;
static const uint8_t CLI_LATENCY_PROBE_PING   = 0x01;
static const uint8_t CLI_LATENCY_PROBE_PONG   = 0x02;

class CliP2PPeer {
public:
    CliP2PPeer(uint32_t peerId, uint32_t virtualIP, const std::string& name);
    ~CliP2PPeer();

    uint32_t      peerId()     const { return m_peerId; }
    uint32_t      virtualIP()  const { return m_virtualIP; }
    std::string   name()       const { return m_name; }
    TransportType transport()  const { return m_transport; }
    NatType       natType()    const { return m_natType; }

    void setNatType(NatType t) { m_natType = t; }
    void setPublicEndpoint(uint32_t ip, uint16_t port) { m_publicIP = ip; m_publicPort = port; }
    uint32_t publicIP()   const { return m_publicIP; }
    uint16_t publicPort() const { return m_publicPort; }

    void setKcpTunnel(CliKcpTunnel* tunnel);
    CliKcpTunnel* kcpTunnel() const { return m_kcpTunnel; }

    void setRawUdpTunnel(CliRawUdpTunnel* tunnel);
    CliRawUdpTunnel* rawUdpTunnel() const { return m_rawUdpTunnel; }

    void setTransport(TransportType t) { m_transport = t; }
    void setTcpRelaySender(TcpRelaySendFunc sender) { m_tcpSender = sender; }

    int sendData(const Buffer& ipPacket);

    void onTcpRelayDataReceived() { m_tcpRelayLastRecv = currentTimeMs(); }
    void sendTcpRelayKeepalive();
    bool isTcpRelayDead() const;
    int  latencyMs() const;
    void sendLatencyPing();
    bool handleLatencyProbe(const Buffer& data);

    void setOnDataReceived(PeerDataCallback cb) { m_onData = cb; }
    void setOnLatencyPong(LatencyPongCallback cb) { m_onLatencyPong = cb; }

    void setCipherKey(const uint8_t key[CIPHER_KEY_SIZE],
                      uint32_t myPeerId,
                      const uint8_t sessionSeed[CIPHER_SESSION_SEED_SIZE]);
    bool hasCipher() const { return m_hasCipher; }
    Buffer decryptData(const Buffer& data);

private:
    void onTunnelDataReceived(const Buffer& data);
    void resetReplayState();
    bool checkAndRecordCounter(uint32_t counter);

    uint32_t      m_peerId;
    uint32_t      m_virtualIP;
    std::string   m_name;
    NatType       m_natType;
    TransportType m_transport;
    uint32_t      m_publicIP;
    uint16_t      m_publicPort;

    CliKcpTunnel*    m_kcpTunnel;
    CliRawUdpTunnel* m_rawUdpTunnel;
    TcpRelaySendFunc m_tcpSender;

    PeerDataCallback    m_onData;
    LatencyPongCallback m_onLatencyPong;

    uint32_t m_tcpRelayLastRecv;
    uint32_t m_tcpRelayLastSend;
    int      m_tcpRtt;
    uint32_t m_latencyPingSentTime;

    bool     m_hasCipher;
    uint8_t  m_cipherKey[CIPHER_KEY_SIZE];
    uint8_t  m_sessionSeed[CIPHER_SESSION_SEED_SIZE];
    uint32_t m_myPeerId;
    uint32_t m_sendCounter;
    uint32_t m_recvMaxCounter;
    uint8_t  m_replayBitmap[REPLAY_WINDOW_SIZE / 8];
    bool     m_replayActive;
};

/* Standalone encrypt/decrypt for use by both P2PPeer and CliApp (TCP relay path) */
Buffer cliEncryptPacket(const Buffer& ipPacket, uint8_t key[CIPHER_KEY_SIZE],
                        uint32_t myPeerId, uint32_t& sendCounter,
                        const uint8_t sessionSeed[CIPHER_SESSION_SEED_SIZE]);

Buffer cliDecryptPacket(const Buffer& encPacket, uint8_t key[CIPHER_KEY_SIZE],
                        uint32_t senderPeerId,
                        const uint8_t sessionSeed[CIPHER_SESSION_SEED_SIZE]);

} // namespace VLan
#endif // VLAN_CLI_PEER_H

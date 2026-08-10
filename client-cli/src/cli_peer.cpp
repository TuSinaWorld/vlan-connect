#include "cli_peer.h"
#include "cli_log.h"
#include <cstring>
#include <algorithm>

namespace VLan {

static const char KCP_KEEPALIVE_MARKER = 0x00;

CliKcpTunnel::CliKcpTunnel(uint32_t conv, UdpSendFunc udpSend,
                           uint32_t peerIP, uint16_t peerPort,
                           FecMode fecMode, uint16_t mtu,
                           KcpProfile profile, TrafficClass trafficClass,
                           bool secureFrames)
    : m_kcp(nullptr), m_udpSend(udpSend), m_peerIP(peerIP), m_peerPort(peerPort),
      m_trafficClass(trafficClass), m_profile(profile),
      m_relayMode(false), m_relaySrcPeerId(0), m_relayDstPeerId(0),
      m_dead(false), m_fecMode(fecMode),
      m_fecEncoder(nullptr), m_fecDecoder(nullptr)
{
    uint32_t now = currentTimeMs();
    m_lastRecvTime = now;
    m_lastSendTime = now;

    m_kcp = ikcp_create(conv, this);
    m_kcp->output = kcpOutput;
    ikcp_setmtu(m_kcp, kcpMtuFromRoomMtu(mtu, m_fecMode != FEC_NONE, secureFrames));
    if (m_profile == KCP_PROFILE_BULK) {
        ikcp_nodelay(m_kcp, 1, 20, 4, 0);
        ikcp_wndsize(m_kcp, 1024, 1024);
        m_kcp->rx_minrto = 30;
        m_kcp->fastresend = 4;
    } else {
        ikcp_nodelay(m_kcp, 1, 10, 2, 1);
        ikcp_wndsize(m_kcp, 256, 256);
        m_kcp->rx_minrto = 10;
        m_kcp->fastresend = 2;
    }

    if (m_fecMode != FEC_NONE) {
        m_fecEncoder = new CliFecEncoder(m_fecMode,
            [this](const Buffer& pkt) { sendKcpPacket(pkt); });
        m_fecDecoder = new CliFecDecoder(
            [this](const Buffer& payload) {
                ikcp_input(m_kcp, reinterpret_cast<const char*>(payload.data()),
                           static_cast<int>(payload.size()));
                tryRecv();
            });
    }
}

CliKcpTunnel::~CliKcpTunnel() {
    if (m_kcp) { ikcp_release(m_kcp); m_kcp = nullptr; }
    delete m_fecEncoder;
    delete m_fecDecoder;
}

int CliKcpTunnel::kcpOutput(const char* buf, int len, ikcpcb*, void* user) {
    CliKcpTunnel* self = static_cast<CliKcpTunnel*>(user);
    Buffer kcpData(buf, buf + len);
    if (self->m_fecMode != FEC_NONE && self->m_fecEncoder)
        self->m_fecEncoder->addPacket(kcpData);
    else
        self->sendKcpPacket(kcpData);
    return 0;
}

void CliKcpTunnel::sendKcpPacket(const Buffer& payload) {
    Buffer pkt;
    if (m_relayMode) {
        UdpRelayHeader hdr;
        memset(&hdr, 0, sizeof(hdr));
        hdr.type = UDP_RELAY_DATA;
        hdr.trafficClass = static_cast<uint8_t>(m_trafficClass);
        hdr.srcPeerId = htonl(m_relaySrcPeerId);
        hdr.dstPeerId = htonl(m_relayDstPeerId);
        pkt.reserve(sizeof(hdr) + payload.size());
        pkt.insert(pkt.end(), reinterpret_cast<const uint8_t*>(&hdr),
                   reinterpret_cast<const uint8_t*>(&hdr) + sizeof(hdr));
        pkt.insert(pkt.end(), payload.begin(), payload.end());
    } else {
        pkt.reserve(1 + payload.size());
        pkt.push_back(static_cast<uint8_t>(UDP_KCP_DATA));
        pkt.insert(pkt.end(), payload.begin(), payload.end());
    }
    m_udpSend(pkt.data(), pkt.size(), m_peerIP, m_peerPort);
    m_lastSendTime = currentTimeMs();
}

void CliKcpTunnel::feedInput(const char* data, int len) {
    m_lastRecvTime = currentTimeMs();
    m_dead = false;
    if (m_fecMode != FEC_NONE && m_fecDecoder) {
        m_fecDecoder->addPacket(data, len);
        return;
    }
    ikcp_input(m_kcp, data, len);
    tryRecv();
}

int CliKcpTunnel::send(const Buffer& data) {
    if (m_profile == KCP_PROFILE_BULK && ikcp_waitsnd(m_kcp) > 2048)
        return -1;
    return ikcp_send(m_kcp, reinterpret_cast<const char*>(data.data()),
                     static_cast<int>(data.size()));
}

void CliKcpTunnel::update() {
    uint32_t now = currentTimeMs();
    ikcp_update(m_kcp, now);
    tryRecv();

    if (!m_dead && (now - m_lastRecvTime > static_cast<uint32_t>(KCP_DEAD_TIMEOUT_MS))) {
        m_dead = true;
        if (m_onDead) m_onDead();
        return;
    }
    if (now - m_lastSendTime > static_cast<uint32_t>(KCP_KEEPALIVE_INTERVAL_MS))
        ikcp_send(m_kcp, &KCP_KEEPALIVE_MARKER, 1);

    if (m_fecEncoder) m_fecEncoder->update(now);
    if (m_fecDecoder) m_fecDecoder->cleanup(now);
}

void CliKcpTunnel::tryRecv() {
    char buf[65536];
    for (;;) {
        int n = ikcp_recv(m_kcp, buf, sizeof(buf));
        if (n <= 0) break;
        if (n == 1 && buf[0] == KCP_KEEPALIVE_MARKER) continue;
        if (m_onData) m_onData(Buffer(buf, buf + n));
    }
}

void CliKcpTunnel::setRelayMode(uint32_t srcPeerId, uint32_t dstPeerId) {
    m_relayMode = true;
    m_relaySrcPeerId = srcPeerId;
    m_relayDstPeerId = dstPeerId;
}

int CliKcpTunnel::getRttMs() const {
    return m_kcp ? m_kcp->rx_srtt : -1;
}

static const char RAW_UDP_KEEPALIVE_MARKER = 0x00;
static const uint8_t RAW_UDP_LATENCY_PING = 0x01;
static const uint8_t RAW_UDP_LATENCY_PONG = 0x02;

CliRawUdpTunnel::CliRawUdpTunnel(UdpSendFunc udpSend,
                                 uint32_t peerIP, uint16_t peerPort,
                                 FecMode fecMode, uint16_t mtu,
                                 TrafficClass trafficClass,
                                 bool secureFrames)
    : m_udpSend(udpSend), m_peerIP(peerIP), m_peerPort(peerPort),
      m_trafficClass(trafficClass),
      m_relayMode(false), m_relaySrcPeerId(0), m_relayDstPeerId(0),
      m_nextMsgId(0), m_dead(false), m_rttMs(-1), m_reassemblyBytes(0),
      m_fecMode(fecMode), m_fecEncoder(nullptr), m_fecDecoder(nullptr),
      m_roomMtu(normalizeRoomMtu(mtu)),
      m_secureFrames(secureFrames)
{
    uint32_t now = currentTimeMs();
    m_lastRecvTime = now;
    m_lastSendTime = now;

    if (m_fecMode != FEC_NONE) {
        m_fecEncoder = new CliFecEncoder(m_fecMode,
            [this](const Buffer& pkt) { sendRawPacket(pkt); });
        m_fecDecoder = new CliFecDecoder(
            [this](const Buffer& payload) {
                processFrag(reinterpret_cast<const char*>(payload.data()),
                            static_cast<int>(payload.size()));
            });
    }
}

CliRawUdpTunnel::~CliRawUdpTunnel() {
    delete m_fecEncoder;
    delete m_fecDecoder;
}

int CliRawUdpTunnel::send(const Buffer& ipPacket) {
    if (ipPacket.empty()) return 0;
    if (ipPacket.size() > m_roomMtu) return -1;
    uint16_t msgId = m_nextMsgId++;
    int totalLen = static_cast<int>(ipPacket.size());
    int maxPayload = maxFragmentPayload();
    int fragTotal = (totalLen + maxPayload - 1) / maxPayload;
    if (fragTotal <= 0 || fragTotal > 255) return -1;

    for (int i = 0; i < fragTotal; ++i) {
        int offset = i * maxPayload;
        int fragLen = std::min(maxPayload, totalLen - offset);
        sendFragment(reinterpret_cast<const char*>(ipPacket.data()) + offset,
                     fragLen, static_cast<uint8_t>(i),
                     static_cast<uint8_t>(fragTotal), msgId,
                     static_cast<uint16_t>(totalLen));
    }
    return totalLen;
}

void CliRawUdpTunnel::sendFragment(const char* data, int len,
                                   uint8_t fragIndex, uint8_t fragTotal,
                                   uint16_t msgId, uint16_t totalLen)
{
    FragHeader fh;
    fh.msgId = htons(msgId);
    fh.fragIndex = fragIndex;
    fh.fragTotal = fragTotal;
    fh.totalLen = htons(totalLen);

    Buffer innerPayload;
    innerPayload.reserve(sizeof(fh) + len);
    innerPayload.insert(innerPayload.end(), reinterpret_cast<const uint8_t*>(&fh),
                        reinterpret_cast<const uint8_t*>(&fh) + sizeof(fh));
    innerPayload.insert(innerPayload.end(), data, data + len);

    if (m_fecMode != FEC_NONE && m_fecEncoder)
        m_fecEncoder->addPacket(innerPayload);
    else
        sendRawPacket(innerPayload);
}

void CliRawUdpTunnel::sendRawPacket(const Buffer& payload) {
    Buffer pkt;
    if (m_relayMode) {
        UdpRelayHeader hdr;
        memset(&hdr, 0, sizeof(hdr));
        hdr.type = UDP_RAW_RELAY_DATA;
        hdr.trafficClass = static_cast<uint8_t>(m_trafficClass);
        hdr.srcPeerId = htonl(m_relaySrcPeerId);
        hdr.dstPeerId = htonl(m_relayDstPeerId);
        pkt.reserve(sizeof(hdr) + payload.size());
        pkt.insert(pkt.end(), reinterpret_cast<const uint8_t*>(&hdr),
                   reinterpret_cast<const uint8_t*>(&hdr) + sizeof(hdr));
        pkt.insert(pkt.end(), payload.begin(), payload.end());
    } else {
        pkt.reserve(1 + payload.size());
        pkt.push_back(static_cast<uint8_t>(UDP_RAW_RELAY_DATA));
        pkt.insert(pkt.end(), payload.begin(), payload.end());
    }
    m_udpSend(pkt.data(), pkt.size(), m_peerIP, m_peerPort);
    m_lastSendTime = currentTimeMs();
}

void CliRawUdpTunnel::feedInput(const char* data, int len) {
    if (len <= 0) return;
    m_lastRecvTime = currentTimeMs();
    m_dead = false;
    if (m_fecMode != FEC_NONE && m_fecDecoder) {
        m_fecDecoder->addPacket(data, len);
        return;
    }
    processFrag(data, len);
}

void CliRawUdpTunnel::processFrag(const char* data, int len) {
    if (len < static_cast<int>(sizeof(FragHeader))) return;
    const FragHeader* fh = reinterpret_cast<const FragHeader*>(data);
    uint16_t msgId = ntohs(fh->msgId);
    uint8_t fragIndex = fh->fragIndex;
    uint8_t fragTotal = fh->fragTotal;
    uint16_t totalLen = ntohs(fh->totalLen);

    const char* payload = data + sizeof(FragHeader);
    int payloadLen = len - sizeof(FragHeader);
    if (payloadLen <= 0 || fragTotal == 0 || fragIndex >= fragTotal ||
        totalLen == 0 || totalLen > m_roomMtu)
        return;
    const int maxPayload = maxFragmentPayload();
    const int expectedTotal =
        (static_cast<int>(totalLen) + maxPayload - 1) / maxPayload;
    if (expectedTotal != fragTotal) return;
    const int expectedLength = fragIndex + 1 == fragTotal
        ? static_cast<int>(totalLen) - maxPayload * (fragTotal - 1)
        : maxPayload;
    if (payloadLen != expectedLength) return;

    if (fragTotal == 1 && payloadLen == 1 && payload[0] == RAW_UDP_KEEPALIVE_MARKER
        && totalLen == 1)
        return;

    if (fragTotal == 1 && payloadLen == 5 && totalLen == 5) {
        uint8_t probeType = static_cast<uint8_t>(payload[0]);
        if (probeType == RAW_UDP_LATENCY_PING) {
            char pongPayload[5];
            pongPayload[0] = static_cast<char>(RAW_UDP_LATENCY_PONG);
            memcpy(pongPayload + 1, payload + 1, 4);
            sendFragment(pongPayload, 5, 0, 1, m_nextMsgId++, 5);
            return;
        }
        if (probeType == RAW_UDP_LATENCY_PONG) {
            uint32_t sentTs = (static_cast<uint8_t>(payload[1]) << 24) |
                              (static_cast<uint8_t>(payload[2]) << 16) |
                              (static_cast<uint8_t>(payload[3]) << 8) |
                               static_cast<uint8_t>(payload[4]);
            m_rttMs = static_cast<int>(currentTimeMs() - sentTs);
            if (m_rttMs < 0) m_rttMs = 0;
            return;
        }
    }

    if (fragTotal == 1) {
        if (payloadLen == totalLen && m_onData)
            m_onData(Buffer(payload, payload + payloadLen));
        return;
    }

    std::map<uint16_t, ReassemblyEntry>::iterator existing =
        m_reassembly.find(msgId);
    if (existing != m_reassembly.end() &&
        (existing->second.totalLen != totalLen ||
         existing->second.fragTotal != fragTotal)) {
        removeReassemblyEntry(msgId);
        return;
    }
    if (existing == m_reassembly.end()) {
        if (!ensureReassemblyCapacity(static_cast<size_t>(payloadLen), true))
            return;
        ReassemblyEntry entry;
        entry.totalLen = totalLen;
        entry.fragTotal = fragTotal;
        entry.receivedCount = 0;
        entry.createTime = currentTimeMs();
        m_reassembly[msgId] = entry;
        existing = m_reassembly.find(msgId);
    }
    if (existing->second.fragments.find(fragIndex) ==
        existing->second.fragments.end()) {
        if (!ensureReassemblyCapacity(static_cast<size_t>(payloadLen), false))
            return;
        existing = m_reassembly.find(msgId);
        if (existing == m_reassembly.end()) return;
        existing->second.fragments[fragIndex] =
            Buffer(payload, payload + payloadLen);
        existing->second.receivedCount++;
        m_reassemblyBytes += static_cast<size_t>(payloadLen);
    }
    existing = m_reassembly.find(msgId);
    if (existing == m_reassembly.end()) return;
    ReassemblyEntry& entry = existing->second;
    if (entry.receivedCount >= entry.fragTotal) {
        Buffer assembled;
        assembled.reserve(entry.totalLen);
        for (uint8_t i = 0; i < entry.fragTotal; ++i) {
            auto it = entry.fragments.find(i);
            if (it == entry.fragments.end()) {
                removeReassemblyEntry(msgId);
                return;
            }
            assembled.insert(assembled.end(), it->second.begin(), it->second.end());
        }
        const bool exact = assembled.size() == entry.totalLen;
        removeReassemblyEntry(msgId);
        if (exact && m_onData) m_onData(assembled);
    }
}

void CliRawUdpTunnel::update() {
    uint32_t now = currentTimeMs();
    if (!m_dead && (now - m_lastRecvTime > static_cast<uint32_t>(RAW_UDP_DEAD_TIMEOUT_MS))) {
        m_dead = true;
        if (m_onDead) m_onDead();
        return;
    }
    if (now - m_lastSendTime > static_cast<uint32_t>(RAW_UDP_KEEPALIVE_MS)) {
        uint32_t ts = now;
        char payload[5];
        payload[0] = static_cast<char>(RAW_UDP_LATENCY_PING);
        payload[1] = static_cast<char>((ts >> 24) & 0xFF);
        payload[2] = static_cast<char>((ts >> 16) & 0xFF);
        payload[3] = static_cast<char>((ts >> 8) & 0xFF);
        payload[4] = static_cast<char>(ts & 0xFF);
        sendFragment(payload, 5, 0, 1, m_nextMsgId++, 5);
    }
    if (m_fecEncoder) m_fecEncoder->update(now);
    if (m_fecDecoder) m_fecDecoder->cleanup(now);
    cleanupStaleEntries();
}

void CliRawUdpTunnel::cleanupStaleEntries() {
    uint32_t now = currentTimeMs();
    std::vector<uint16_t> stale;
    for (auto it = m_reassembly.begin(); it != m_reassembly.end(); ++it) {
        if (now - it->second.createTime > static_cast<uint32_t>(RAW_UDP_FRAG_TIMEOUT_MS))
            stale.push_back(it->first);
    }
    for (uint16_t id : stale)
        removeReassemblyEntry(id);
}

void CliRawUdpTunnel::removeReassemblyEntry(uint16_t msgId) {
    std::map<uint16_t, ReassemblyEntry>::iterator it = m_reassembly.find(msgId);
    if (it == m_reassembly.end()) return;
    size_t bytes = 0;
    for (std::map<uint8_t, Buffer>::const_iterator frag =
             it->second.fragments.begin(); frag != it->second.fragments.end(); ++frag)
        bytes += frag->second.size();
    m_reassemblyBytes = bytes > m_reassemblyBytes
        ? 0 : m_reassemblyBytes - bytes;
    m_reassembly.erase(it);
}

bool CliRawUdpTunnel::ensureReassemblyCapacity(size_t incomingBytes,
                                                bool newMessage) {
    if (incomingBytes > RAW_UDP_MAX_REASSEMBLY_BYTES) return false;
    while (!m_reassembly.empty() &&
           ((newMessage && m_reassembly.size() >=
               RAW_UDP_MAX_ACTIVE_MESSAGES) ||
            m_reassemblyBytes + incomingBytes >
                RAW_UDP_MAX_REASSEMBLY_BYTES)) {
        std::map<uint16_t, ReassemblyEntry>::const_iterator oldest =
            m_reassembly.begin();
        for (std::map<uint16_t, ReassemblyEntry>::const_iterator it =
                 m_reassembly.begin(); it != m_reassembly.end(); ++it) {
            if (it->second.createTime < oldest->second.createTime) oldest = it;
        }
        removeReassemblyEntry(oldest->first);
    }
    return (!newMessage || m_reassembly.size() <
                RAW_UDP_MAX_ACTIVE_MESSAGES) &&
           m_reassemblyBytes + incomingBytes <=
                RAW_UDP_MAX_REASSEMBLY_BYTES;
}

int CliRawUdpTunnel::maxFragmentPayload() const {
    return transportPayloadBudget(m_roomMtu, MODE_RELAY_RAW_UDP,
                                  m_fecMode != FEC_NONE, m_secureFrames);
}

void CliRawUdpTunnel::setRelayMode(uint32_t srcPeerId, uint32_t dstPeerId) {
    m_relayMode = true;
    m_relaySrcPeerId = srcPeerId;
    m_relayDstPeerId = dstPeerId;
}

CliPeerConnection::CliPeerConnection(uint32_t peerId, uint32_t virtualIP,
                                     const std::string& name)
    : m_peerId(peerId), m_virtualIP(virtualIP), m_name(name)
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

CliPeerConnection::~CliPeerConnection() {}

TransportType CliPeerConnection::transport(TrafficClass cls) const {
    int idx = static_cast<int>(cls);
    if (idx < 0 || idx > 2) idx = TRAFFIC_UDP;
    return m_transport[idx];
}

void CliPeerConnection::setKcpTunnel(TrafficClass cls, CliKcpTunnel* tunnel) {
    int idx = static_cast<int>(cls);
    if (idx < 0 || idx > 2) idx = TRAFFIC_UDP;
    m_kcpTunnel[idx] = tunnel;
    if (tunnel) {
        tunnel->setOnDataReceived([this, cls](const Buffer& data) {
            onTunnelDataReceived(cls, data);
        });
    }
}

CliKcpTunnel* CliPeerConnection::kcpTunnel(TrafficClass cls) const {
    int idx = static_cast<int>(cls);
    if (idx < 0 || idx > 2) idx = TRAFFIC_UDP;
    return m_kcpTunnel[idx];
}

void CliPeerConnection::clearKcpTunnel(TrafficClass cls) {
    int idx = static_cast<int>(cls);
    if (idx < 0 || idx > 2) idx = TRAFFIC_UDP;
    m_kcpTunnel[idx] = nullptr;
}

void CliPeerConnection::setRawUdpTunnel(TrafficClass cls, CliRawUdpTunnel* tunnel) {
    int idx = static_cast<int>(cls);
    if (idx < 0 || idx > 2) idx = TRAFFIC_UDP;
    m_rawUdpTunnel[idx] = tunnel;
    if (tunnel) {
        tunnel->setOnDataReceived([this, cls](const Buffer& data) {
            onTunnelDataReceived(cls, data);
        });
    }
}

CliRawUdpTunnel* CliPeerConnection::rawUdpTunnel(TrafficClass cls) const {
    int idx = static_cast<int>(cls);
    if (idx < 0 || idx > 2) idx = TRAFFIC_UDP;
    return m_rawUdpTunnel[idx];
}

void CliPeerConnection::clearRawUdpTunnel(TrafficClass cls) {
    int idx = static_cast<int>(cls);
    if (idx < 0 || idx > 2) idx = TRAFFIC_UDP;
    m_rawUdpTunnel[idx] = nullptr;
}

void CliPeerConnection::setTransport(TrafficClass cls, TransportType t) {
    int idx = static_cast<int>(cls);
    if (idx < 0 || idx > 2) idx = TRAFFIC_UDP;
    m_transport[idx] = t;
}

void CliPeerConnection::clearTransport(TrafficClass cls) {
    int idx = static_cast<int>(cls);
    if (idx < 0 || idx > 2) idx = TRAFFIC_UDP;
    uint32_t now = currentTimeMs();
    m_transport[idx] = TRANSPORT_NONE;
    m_tcpRelayLastRecv[idx] = now;
    m_tcpRelayLastSend[idx] = now;
    m_rtt[idx] = -1;
    m_latencyPingSentTime[idx] = 0;
    m_latencyLastReply[idx] = 0;
}

void CliPeerConnection::onTunnelDataReceived(TrafficClass cls, const Buffer& data) {
    if (handleLatencyProbe(cls, data))
        return;
    if (m_onData) m_onData(m_peerId, data);
}

int CliPeerConnection::sendData(const Buffer& ipPacket) {
    TrafficClass cls = trafficClassFromIpPacket(ipPacket.data(), ipPacket.size());
    int idx = static_cast<int>(cls);
    if (idx < 0 || idx > 2) idx = TRAFFIC_UDP;
    TransportType t = m_transport[idx];
    if (t == TRANSPORT_NONE) return -1;

    if (t == TRANSPORT_RELAY_TCP) {
        if (m_tcpSender) {
            m_tcpSender(m_peerId, cls, ipPacket);
            return static_cast<int>(ipPacket.size());
        }
        return -1;
    }
    if (t == TRANSPORT_RELAY_RAW_UDP) {
        if (!m_rawUdpTunnel[idx]) return -1;
        return m_rawUdpTunnel[idx]->send(ipPacket);
    }
    if (!m_kcpTunnel[idx]) return -1;
    return m_kcpTunnel[idx]->send(ipPacket);
}

void CliPeerConnection::onTcpRelayDataReceived(TrafficClass cls) {
    int idx = static_cast<int>(cls);
    if (idx < 0 || idx > 2) idx = TRAFFIC_TCP;
    m_tcpRelayLastRecv[idx] = currentTimeMs();
}

void CliPeerConnection::sendTcpRelayKeepalive() {
    uint32_t now = currentTimeMs();
    for (int idx = TRAFFIC_TCP; idx <= TRAFFIC_UDP; ++idx) {
        if (m_transport[idx] != TRANSPORT_RELAY_TCP || !m_tcpSender) continue;
        if (now - m_tcpRelayLastSend[idx] >= static_cast<uint32_t>(TCP_RELAY_KEEPALIVE_MS)) {
            m_tcpSender(m_peerId, static_cast<TrafficClass>(idx), Buffer());
            m_tcpRelayLastSend[idx] = now;
        }
    }
}

bool CliPeerConnection::isTcpRelayDead(TrafficClass cls) const {
    int idx = static_cast<int>(cls);
    if (idx < 0 || idx > 2) idx = TRAFFIC_TCP;
    if (m_transport[idx] != TRANSPORT_RELAY_TCP)
        return false;

    uint32_t now = currentTimeMs();
    return now - m_tcpRelayLastRecv[idx] > static_cast<uint32_t>(TCP_RELAY_DEAD_MS);
}

bool CliPeerConnection::isTcpRelayDead() const {
    for (int idx = TRAFFIC_TCP; idx <= TRAFFIC_UDP; ++idx) {
        if (isTcpRelayDead(static_cast<TrafficClass>(idx)))
            return true;
    }
    return false;
}

int CliPeerConnection::latencyMs(TrafficClass cls) const {
    int idx = static_cast<int>(cls);
    if (idx < 0 || idx > 2) idx = TRAFFIC_UDP;
    if (m_latencyLastReply[idx] == 0)
        return -1;
    uint32_t now = currentTimeMs();
    if (now - m_latencyLastReply[idx] > CLI_LATENCY_STALE_MS)
        return -1;
    return m_rtt[idx];
}

int CliPeerConnection::latencyMs() const {
    int tcp = latencyMs(TRAFFIC_TCP);
    if (tcp >= 0)
        return tcp;
    int udp = latencyMs(TRAFFIC_UDP);
    if (udp >= 0)
        return udp;
    TransportType t = m_transport[TRAFFIC_TCP] != TRANSPORT_NONE
        ? m_transport[TRAFFIC_TCP] : m_transport[TRAFFIC_UDP];
    if (t == TRANSPORT_RELAY_RAW_UDP)
        return m_rawUdpTunnel[TRAFFIC_UDP] ? m_rawUdpTunnel[TRAFFIC_UDP]->getRttMs() : -1;
    if (m_kcpTunnel[TRAFFIC_UDP]) return m_kcpTunnel[TRAFFIC_UDP]->getRttMs();
    return -1;
}

void CliPeerConnection::sendLatencyPing(TrafficClass cls) {
    int idx = static_cast<int>(cls);
    if (idx < 0 || idx > 2) idx = TRAFFIC_UDP;
    if (m_transport[idx] == TRANSPORT_NONE) return;
    m_latencyPingSentTime[idx] = currentTimeMs();
    Buffer probe(6, 0);
    probe[0] = CLI_LATENCY_PROBE_MARKER;
    probe[1] = CLI_LATENCY_PROBE_PING;
    uint32_t ts = m_latencyPingSentTime[idx];
    probe[2] = (ts >> 24) & 0xFF;
    probe[3] = (ts >> 16) & 0xFF;
    probe[4] = (ts >> 8) & 0xFF;
    probe[5] = ts & 0xFF;
    sendControlPacket(cls, probe);
}

void CliPeerConnection::sendLatencyPing() {
    sendLatencyPing(TRAFFIC_TCP);
    sendLatencyPing(TRAFFIC_UDP);
}

bool CliPeerConnection::handleLatencyProbe(TrafficClass cls, const Buffer& data) {
    if (data.size() < 6 || data[0] != CLI_LATENCY_PROBE_MARKER) return false;
    int idx = static_cast<int>(cls);
    if (idx < 0 || idx > 2) idx = TRAFFIC_UDP;
    if (data[1] == CLI_LATENCY_PROBE_PING) {
        Buffer pong(6, 0);
        pong[0] = CLI_LATENCY_PROBE_MARKER;
        pong[1] = CLI_LATENCY_PROBE_PONG;
        pong[2] = data[2]; pong[3] = data[3];
        pong[4] = data[4]; pong[5] = data[5];
        sendControlPacket(cls, pong);
        return true;
    }
    if (data[1] == CLI_LATENCY_PROBE_PONG) {
        uint32_t sentTs = (static_cast<uint32_t>(data[2]) << 24) |
                          (static_cast<uint32_t>(data[3]) << 16) |
                          (static_cast<uint32_t>(data[4]) << 8) |
                           static_cast<uint32_t>(data[5]);
        m_rtt[idx] = static_cast<int>(currentTimeMs() - sentTs);
        if (m_rtt[idx] < 0) m_rtt[idx] = 0;
        m_latencyLastReply[idx] = currentTimeMs();
        return true;
    }
    return false;
}

bool CliPeerConnection::handleLatencyProbe(const Buffer& data) {
    return handleLatencyProbe(TRAFFIC_TCP, data);
}

bool CliPeerConnection::sendControlPacket(TrafficClass cls, const Buffer& data) {
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

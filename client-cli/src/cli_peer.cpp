#include "cli_peer.h"
#include "cli_log.h"
#include <cstring>
#include <algorithm>
#include <random>

extern "C" {
#include "monocypher.h"
}

namespace VLan {

// ======================== CliKcpTunnel ========================

static const char KCP_KEEPALIVE_MARKER = 0x00;

CliKcpTunnel::CliKcpTunnel(uint32_t conv, UdpSendFunc udpSend,
                           uint32_t peerIP, uint16_t peerPort,
                           FecMode fecMode,
                           uint16_t mtu)
    : m_udpSend(udpSend), m_peerIP(peerIP), m_peerPort(peerPort),
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
        hdr.type      = UDP_RELAY_DATA;
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

bool CliKcpTunnel::isAlive() const {
    if (m_dead) return false;
    return (currentTimeMs() - m_lastRecvTime) < static_cast<uint32_t>(KCP_DEAD_TIMEOUT_MS);
}

int CliKcpTunnel::getRttMs() const {
    return m_kcp ? m_kcp->rx_srtt : -1;
}

// ======================== CliRawUdpTunnel ========================

static const char RAW_UDP_KEEPALIVE_MARKER = 0x00;
static const uint8_t RAW_UDP_LATENCY_PING = 0x01;
static const uint8_t RAW_UDP_LATENCY_PONG = 0x02;

CliRawUdpTunnel::CliRawUdpTunnel(UdpSendFunc udpSend,
                                   uint32_t peerIP, uint16_t peerPort,
                                   FecMode fecMode,
                                   uint16_t mtu)
    : m_udpSend(udpSend), m_peerIP(peerIP), m_peerPort(peerPort),
      m_relayMode(false), m_relaySrcPeerId(0), m_relayDstPeerId(0),
      m_nextMsgId(0), m_dead(false), m_rttMs(-1),
      m_fecMode(fecMode), m_fecEncoder(nullptr), m_fecDecoder(nullptr),
      m_roomMtu(normalizeRoomMtu(mtu))
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
    uint16_t msgId = m_nextMsgId++;
    int totalLen = static_cast<int>(ipPacket.size());
    int maxPayload = maxFragmentPayload();
    int fragTotal = (totalLen + maxPayload - 1) / maxPayload;
    if (fragTotal > 256) return -1;

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
    fh.msgId     = htons(msgId);
    fh.fragIndex = fragIndex;
    fh.fragTotal = fragTotal;
    fh.totalLen  = htons(totalLen);

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
        hdr.type      = UDP_RAW_RELAY_DATA;
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
    uint16_t msgId     = ntohs(fh->msgId);
    uint8_t  fragIndex = fh->fragIndex;
    uint8_t  fragTotal = fh->fragTotal;
    uint16_t totalLen  = ntohs(fh->totalLen);

    const char* payload = data + sizeof(FragHeader);
    int payloadLen = len - sizeof(FragHeader);
    if (payloadLen <= 0) return;

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
                              (static_cast<uint8_t>(payload[3]) << 8)  |
                               static_cast<uint8_t>(payload[4]);
            m_rttMs = static_cast<int>(currentTimeMs() - sentTs);
            if (m_rttMs < 0) m_rttMs = 0;
            return;
        }
    }

    if (fragTotal == 1) {
        if (m_onData) m_onData(Buffer(payload, payload + payloadLen));
        return;
    }

    ReassemblyEntry& entry = m_reassembly[msgId];
    if (entry.fragments.empty()) {
        entry.totalLen      = totalLen;
        entry.fragTotal     = fragTotal;
        entry.receivedCount = 0;
        entry.createTime    = currentTimeMs();
    }
    if (entry.fragments.find(fragIndex) == entry.fragments.end()) {
        entry.fragments[fragIndex] = Buffer(payload, payload + payloadLen);
        entry.receivedCount++;
    }
    if (entry.receivedCount >= entry.fragTotal) {
        Buffer assembled;
        assembled.reserve(entry.totalLen);
        for (uint8_t i = 0; i < entry.fragTotal; ++i) {
            auto it = entry.fragments.find(i);
            if (it == entry.fragments.end()) {
                m_reassembly.erase(msgId);
                return;
            }
            assembled.insert(assembled.end(), it->second.begin(), it->second.end());
        }
        m_reassembly.erase(msgId);
        if (m_onData) m_onData(assembled);
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
        m_reassembly.erase(id);
}

int CliRawUdpTunnel::maxFragmentPayload() const {
    int payload = static_cast<int>(normalizeRoomMtu(m_roomMtu)) + CIPHER_OVERHEAD;
    if (payload > RAW_UDP_MAX_FRAG_PAYLOAD)
        payload = RAW_UDP_MAX_FRAG_PAYLOAD;
    return payload;
}

void CliRawUdpTunnel::setRelayMode(uint32_t srcPeerId, uint32_t dstPeerId) {
    m_relayMode = true;
    m_relaySrcPeerId = srcPeerId;
    m_relayDstPeerId = dstPeerId;
}

bool CliRawUdpTunnel::isAlive() const {
    if (m_dead) return false;
    return (currentTimeMs() - m_lastRecvTime) < static_cast<uint32_t>(RAW_UDP_DEAD_TIMEOUT_MS);
}

// ======================== CliP2PPeer ========================

CliP2PPeer::CliP2PPeer(uint32_t peerId, uint32_t virtualIP, const std::string& name)
    : m_peerId(peerId), m_virtualIP(virtualIP), m_name(name),
      m_natType(NAT_UNKNOWN), m_transport(TRANSPORT_NONE),
      m_publicIP(0), m_publicPort(0),
      m_kcpTunnel(nullptr), m_rawUdpTunnel(nullptr),
      m_tcpRelayLastRecv(currentTimeMs()),
      m_tcpRelayLastSend(currentTimeMs()),
      m_tcpRtt(-1), m_latencyPingSentTime(0),
      m_hasCipher(false), m_myPeerId(0), m_sendCounter(0),
      m_recvMaxCounter(0), m_replayActive(false)
{
    memset(m_cipherKey, 0, CIPHER_KEY_SIZE);
    memset(m_sessionSeed, 0, CIPHER_SESSION_SEED_SIZE);
    memset(m_replayBitmap, 0, sizeof(m_replayBitmap));
}

CliP2PPeer::~CliP2PPeer() {}

void CliP2PPeer::setKcpTunnel(CliKcpTunnel* tunnel) {
    m_kcpTunnel = tunnel;
    if (tunnel) {
        tunnel->setOnDataReceived([this](const Buffer& data) {
            onTunnelDataReceived(data);
        });
    }
}

void CliP2PPeer::setRawUdpTunnel(CliRawUdpTunnel* tunnel) {
    m_rawUdpTunnel = tunnel;
    if (tunnel) {
        tunnel->setOnDataReceived([this](const Buffer& data) {
            onTunnelDataReceived(data);
        });
    }
}

void CliP2PPeer::setCipherKey(const uint8_t key[CIPHER_KEY_SIZE],
                              uint32_t myPeerId,
                              const uint8_t sessionSeed[CIPHER_SESSION_SEED_SIZE]) {
    memcpy(m_cipherKey, key, CIPHER_KEY_SIZE);
    memcpy(m_sessionSeed, sessionSeed, CIPHER_SESSION_SEED_SIZE);
    m_myPeerId = myPeerId;
    m_sendCounter = 0;
    resetReplayState();
    m_hasCipher = true;
}

void CliP2PPeer::resetReplayState() {
    m_recvMaxCounter = 0;
    m_replayActive = false;
    memset(m_replayBitmap, 0, sizeof(m_replayBitmap));
}

bool CliP2PPeer::checkAndRecordCounter(uint32_t counter) {
    if (!m_replayActive) {
        m_recvMaxCounter = 0;
        m_replayActive = true;
        memset(m_replayBitmap, 0, sizeof(m_replayBitmap));
    }

    if (m_recvMaxCounter >= static_cast<uint32_t>(REPLAY_WINDOW_SIZE) &&
        counter <= m_recvMaxCounter - static_cast<uint32_t>(REPLAY_WINDOW_SIZE))
        return false;

    if (counter > m_recvMaxCounter) {
        uint32_t shift = counter - m_recvMaxCounter;
        if (shift >= static_cast<uint32_t>(REPLAY_WINDOW_SIZE)) {
            memset(m_replayBitmap, 0, sizeof(m_replayBitmap));
        } else {
            for (uint32_t i = 0; i < shift; ++i) {
                uint32_t pos = (m_recvMaxCounter + i + 1) % REPLAY_WINDOW_SIZE;
                uint8_t bit = static_cast<uint8_t>(1u << (pos % 8));
                m_replayBitmap[pos / 8] =
                    static_cast<uint8_t>(m_replayBitmap[pos / 8] & ~bit);
            }
        }
        m_recvMaxCounter = counter;
    }

    uint32_t pos = counter % REPLAY_WINDOW_SIZE;
    uint8_t bit = static_cast<uint8_t>(1u << (pos % 8));
    if (m_replayBitmap[pos / 8] & bit)
        return false;
    m_replayBitmap[pos / 8] =
        static_cast<uint8_t>(m_replayBitmap[pos / 8] | bit);
    return true;
}

static void buildNonce(uint8_t nonce[CIPHER_NONCE_SIZE],
                       uint32_t peerId, uint32_t counter,
                       const uint8_t sessionSeed[CIPHER_SESSION_SEED_SIZE]) {
    memcpy(nonce,     &peerId,      4);
    memcpy(nonce + 4, &counter,     4);
    memcpy(nonce + 8, sessionSeed, CIPHER_SESSION_SEED_SIZE);
}

static bool parseEncryptedPacket(const Buffer& encPacket,
                                 int& ihl, int& cipherLen,
                                 uint32_t& counter) {
    if (encPacket.size() < 20) return false;
    ihl = (encPacket[0] & 0x0F) * 4;
    if (ihl < 20 || encPacket.size() < static_cast<size_t>(ihl + CIPHER_OVERHEAD))
        return false;
    cipherLen = static_cast<int>(encPacket.size()) - ihl
        - CIPHER_CTR_SIZE - CIPHER_MAC_SIZE;
    if (cipherLen <= 0) return false;
    memcpy(&counter, encPacket.data() + ihl, CIPHER_CTR_SIZE);
    return true;
}

Buffer cliEncryptPacket(const Buffer& ipPacket, uint8_t key[CIPHER_KEY_SIZE],
                            uint32_t myPeerId, uint32_t& sendCounter,
                            const uint8_t sessionSeed[CIPHER_SESSION_SEED_SIZE]) {
    if (sendCounter >= 0xFFFFFFF0u) return Buffer();
    if (ipPacket.size() < 20) return ipPacket;
    int ihl = (ipPacket[0] & 0x0F) * 4;
    if (ihl < 20 || ihl > static_cast<int>(ipPacket.size())) return ipPacket;
    int payloadLen = static_cast<int>(ipPacket.size()) - ihl;
    if (payloadLen <= 0) return ipPacket;

    uint32_t ctr = sendCounter++;
    uint8_t nonce[CIPHER_NONCE_SIZE];
    buildNonce(nonce, myPeerId, ctr, sessionSeed);

    Buffer out(ihl + CIPHER_CTR_SIZE + payloadLen + CIPHER_MAC_SIZE);
    memcpy(out.data(), ipPacket.data(), ihl);
    memcpy(out.data() + ihl, &ctr, CIPHER_CTR_SIZE);

    uint8_t* cipherOut = out.data() + ihl + CIPHER_CTR_SIZE;
    uint8_t* macOut    = cipherOut + payloadLen;

    crypto_aead_lock(cipherOut, macOut,
                     key, nonce,
                     ipPacket.data(), ihl,
                     ipPacket.data() + ihl, payloadLen);
    return out;
}

Buffer cliDecryptPacket(const Buffer& encPacket, uint8_t key[CIPHER_KEY_SIZE],
                            uint32_t senderPeerId,
                            const uint8_t sessionSeed[CIPHER_SESSION_SEED_SIZE]) {
    int ihl = 0;
    int cipherLen = 0;
    uint32_t ctr;
    if (!parseEncryptedPacket(encPacket, ihl, cipherLen, ctr))
        return Buffer();

    uint8_t nonce[CIPHER_NONCE_SIZE];
    buildNonce(nonce, senderPeerId, ctr, sessionSeed);

    const uint8_t* cipherData = encPacket.data() + ihl + CIPHER_CTR_SIZE;
    const uint8_t* mac = cipherData + cipherLen;

    Buffer out(ihl + cipherLen);
    memcpy(out.data(), encPacket.data(), ihl);

    int rc = crypto_aead_unlock(out.data() + ihl, mac,
                                key, nonce,
                                encPacket.data(), ihl,
                                cipherData, cipherLen);
    if (rc != 0) return Buffer();
    return out;
}

Buffer CliP2PPeer::decryptData(const Buffer& data) {
    if (!m_hasCipher || data.size() < 20 || (data[0] & 0xF0) != 0x40)
        return data;

    int ihl = 0;
    int cipherLen = 0;
    uint32_t counter = 0;
    if (!parseEncryptedPacket(data, ihl, cipherLen, counter))
        return Buffer();

    Buffer pkt = cliDecryptPacket(data, m_cipherKey, m_peerId, m_sessionSeed);
    if (pkt.empty()) return Buffer();

    if (!checkAndRecordCounter(counter)) {
        LOG_DBG("Dropped replayed encrypted packet from peer %u counter=%u",
                m_peerId, counter);
        return Buffer();
    }
    return pkt;
}

void CliP2PPeer::onTunnelDataReceived(const Buffer& data) {
    Buffer pkt = decryptData(data);
    if (pkt.empty()) return;
    if (m_onData) m_onData(m_peerId, pkt);
}

int CliP2PPeer::sendData(const Buffer& ipPacket) {
    if (m_transport == TRANSPORT_NONE) return -1;

    Buffer pkt = ipPacket;
    if (m_hasCipher)
        pkt = cliEncryptPacket(pkt, m_cipherKey, m_myPeerId, m_sendCounter, m_sessionSeed);

    if (m_transport == TRANSPORT_RELAY_TCP) {
        if (m_tcpSender) { m_tcpSender(m_peerId, pkt); return static_cast<int>(pkt.size()); }
        return -1;
    }
    if (m_transport == TRANSPORT_RELAY_RAW_UDP) {
        if (!m_rawUdpTunnel) return -1;
        return m_rawUdpTunnel->send(pkt);
    }
    if (!m_kcpTunnel) return -1;
    return m_kcpTunnel->send(pkt);
}

void CliP2PPeer::sendTcpRelayKeepalive() {
    if (m_transport != TRANSPORT_RELAY_TCP || !m_tcpSender) return;
    uint32_t now = currentTimeMs();
    if (now - m_tcpRelayLastSend >= static_cast<uint32_t>(TCP_RELAY_KEEPALIVE_MS)) {
        m_tcpSender(m_peerId, Buffer());
        m_tcpRelayLastSend = now;
    }
}

bool CliP2PPeer::isTcpRelayDead() const {
    if (m_transport != TRANSPORT_RELAY_TCP) return false;
    return (currentTimeMs() - m_tcpRelayLastRecv) > static_cast<uint32_t>(TCP_RELAY_DEAD_MS);
}

int CliP2PPeer::latencyMs() const {
    if (m_transport == TRANSPORT_RELAY_TCP) return m_tcpRtt;
    if (m_transport == TRANSPORT_RELAY_RAW_UDP)
        return m_rawUdpTunnel ? m_rawUdpTunnel->getRttMs() : -1;
    if (m_kcpTunnel) return m_kcpTunnel->getRttMs();
    return -1;
}

void CliP2PPeer::sendLatencyPing() {
    if (m_transport != TRANSPORT_RELAY_TCP || !m_tcpSender) return;
    m_latencyPingSentTime = currentTimeMs();
    Buffer probe(6, 0);
    probe[0] = CLI_LATENCY_PROBE_MARKER;
    probe[1] = CLI_LATENCY_PROBE_PING;
    uint32_t ts = m_latencyPingSentTime;
    probe[2] = (ts >> 24) & 0xFF;
    probe[3] = (ts >> 16) & 0xFF;
    probe[4] = (ts >> 8) & 0xFF;
    probe[5] = ts & 0xFF;
    m_tcpSender(m_peerId, probe);
}

bool CliP2PPeer::handleLatencyProbe(const Buffer& data) {
    if (data.size() < 6) return false;
    if (data[0] != CLI_LATENCY_PROBE_MARKER) return false;

    if (data[1] == CLI_LATENCY_PROBE_PING) {
        Buffer pong(6, 0);
        pong[0] = CLI_LATENCY_PROBE_MARKER;
        pong[1] = CLI_LATENCY_PROBE_PONG;
        pong[2] = data[2]; pong[3] = data[3];
        pong[4] = data[4]; pong[5] = data[5];
        if (m_onLatencyPong) m_onLatencyPong(m_peerId, pong);
        return true;
    }
    if (data[1] == CLI_LATENCY_PROBE_PONG) {
        uint32_t sentTs = (data[2] << 24) | (data[3] << 16) | (data[4] << 8) | data[5];
        m_tcpRtt = static_cast<int>(currentTimeMs() - sentTs);
        if (m_tcpRtt < 0) m_tcpRtt = 0;
        return true;
    }
    return false;
}

} // namespace VLan

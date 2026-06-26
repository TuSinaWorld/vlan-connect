#include "raw_udp_tunnel.h"
#include "fec_codec.h"
#include "net_common.h"
#include "payload_cipher.h"
#include "protocol.h"
#include <cstring>
#include "../ui/log_manager.h"

namespace VLan {

static const char RAW_UDP_KEEPALIVE_MARKER = 0x00;
static const uint8_t RAW_UDP_LATENCY_PING = 0x01;
static const uint8_t RAW_UDP_LATENCY_PONG = 0x02;

RawUdpTunnel::RawUdpTunnel(QUdpSocket* socket,
                             const QHostAddress& peerAddr, quint16 peerPort,
                             FecMode fecMode,
                             uint16_t mtu,
                             QObject* parent)
    : QObject(parent),
      m_socket(socket), m_peerAddr(peerAddr), m_peerPort(peerPort),
      m_relayMode(false), m_relaySrcPeerId(0), m_relayDstPeerId(0),
      m_nextMsgId(0), m_dead(false),
      m_fecMode(fecMode), m_fecEncoder(nullptr), m_fecDecoder(nullptr),
      m_roomMtu(normalizeRoomMtu(mtu)),
      m_rttMs(-1)
{
    uint32_t now = currentTimeMs();
    m_lastRecvTime = now;
    m_lastSendTime = now;

    if (m_fecMode != FEC_NONE) {
        m_fecEncoder = new FecEncoder(m_fecMode,
            [this](const QByteArray& pkt) { sendRawPacket(pkt); });
        m_fecDecoder = new FecDecoder(
            [this](const QByteArray& payload) { processFrag(payload.constData(), payload.size()); });
    }
}

RawUdpTunnel::~RawUdpTunnel() {
    delete m_fecEncoder;
    delete m_fecDecoder;
}

int RawUdpTunnel::send(const QByteArray& ipPacket) {
    if (ipPacket.isEmpty()) return 0;

    uint16_t msgId = m_nextMsgId++;
    int totalLen = ipPacket.size();
    int maxPayload = maxFragmentPayload();

    int fragTotal = (totalLen + maxPayload - 1) / maxPayload;
    if (fragTotal > 256) {
        LogManager::instance().logError(QString("[raw_udp] Packet too large to fragment: %1 maxPayload=%2").arg(totalLen).arg(maxPayload));
        return -1;
    }

    if (g_verboseLog)
        LogManager::instance().logDetail(QString("[raw_udp] send packetSize=%1 fragTotal=%2 msgId=%3").arg(totalLen).arg(fragTotal).arg(msgId));

    for (int i = 0; i < fragTotal; ++i) {
        int offset = i * maxPayload;
        int fragLen = qMin(maxPayload, totalLen - offset);
        sendFragment(ipPacket.constData() + offset, fragLen,
                     static_cast<uint8_t>(i),
                     static_cast<uint8_t>(fragTotal),
                     msgId, static_cast<uint16_t>(totalLen));
    }
    return totalLen;
}

void RawUdpTunnel::sendFragment(const char* data, int len,
                                 uint8_t fragIndex, uint8_t fragTotal,
                                 uint16_t msgId, uint16_t totalLen)
{
    FragHeader fh;
    fh.msgId     = htons(msgId);
    fh.fragIndex = fragIndex;
    fh.fragTotal = fragTotal;
    fh.totalLen  = htons(totalLen);

    QByteArray innerPayload;
    innerPayload.reserve(sizeof(fh) + len);
    innerPayload.append(reinterpret_cast<const char*>(&fh), sizeof(fh));
    innerPayload.append(data, len);

    if (m_fecMode != FEC_NONE && m_fecEncoder) {
        m_fecEncoder->addPacket(innerPayload);
    } else {
        sendRawPacket(innerPayload);
    }
}

void RawUdpTunnel::sendRawPacket(const QByteArray& payload) {
    QByteArray pkt;

    if (m_relayMode) {
        UdpRelayHeader hdr;
        hdr.type      = UDP_RAW_RELAY_DATA;
        hdr.srcPeerId = htonl(m_relaySrcPeerId);
        hdr.dstPeerId = htonl(m_relayDstPeerId);

        pkt.reserve(sizeof(hdr) + payload.size());
        pkt.append(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
        pkt.append(payload);
    } else {
        pkt.reserve(1 + payload.size());
        pkt.append(static_cast<char>(UDP_RAW_RELAY_DATA));
        pkt.append(payload);
    }

    m_socket->writeDatagram(pkt, m_peerAddr, m_peerPort);
    m_lastSendTime = currentTimeMs();
}

void RawUdpTunnel::feedInput(const char* data, int len) {
    if (len <= 0) return;

    m_lastRecvTime = currentTimeMs();
    m_dead = false;

    if (g_verboseLog)
        LogManager::instance().logDetail(QString("[raw_udp] feedInput len=%1 fecMode=%2").arg(len).arg(m_fecMode));

    if (m_fecMode != FEC_NONE && m_fecDecoder) {
        m_fecDecoder->addPacket(data, len);
        return;
    }

    processFrag(data, len);
}

void RawUdpTunnel::processFrag(const char* data, int len) {
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
        && totalLen == 1) {
        return;
    }

    if (fragTotal == 1 && payloadLen == 5 && totalLen == 5) {
        uint8_t probeType = static_cast<uint8_t>(payload[0]);
        if (probeType == RAW_UDP_LATENCY_PING) {
            sendLatencyPong(payload + 1);
            return;
        }
        if (probeType == RAW_UDP_LATENCY_PONG) {
            uint32_t sentTs = (static_cast<uint8_t>(payload[1]) << 24) |
                              (static_cast<uint8_t>(payload[2]) << 16) |
                              (static_cast<uint8_t>(payload[3]) << 8) |
                               static_cast<uint8_t>(payload[4]);
            uint32_t now = currentTimeMs();
            m_rttMs = static_cast<int>(now - sentTs);
            if (m_rttMs < 0) m_rttMs = 0;
            return;
        }
    }

    if (fragTotal == 1) {
        if (g_verboseLog)
            LogManager::instance().logDetail(QString("[raw_udp] processFrag single-frag msgId=%1 payloadLen=%2").arg(msgId).arg(payloadLen));
        emit dataReceived(QByteArray(payload, payloadLen));
        return;
    }

    if (g_verboseLog)
        LogManager::instance().logDetail(QString("[raw_udp] processFrag msgId=%1 frag=%2/%3 payloadLen=%4 totalLen=%5").arg(msgId).arg(fragIndex).arg(fragTotal).arg(payloadLen).arg(totalLen));

    ReassemblyEntry& entry = m_reassembly[msgId];
    if (entry.fragments.isEmpty()) {
        entry.totalLen     = totalLen;
        entry.fragTotal    = fragTotal;
        entry.receivedCount = 0;
        entry.createTime   = currentTimeMs();
    }

    if (!entry.fragments.contains(fragIndex)) {
        entry.fragments[fragIndex] = QByteArray(payload, payloadLen);
        entry.receivedCount++;
    }

    if (entry.receivedCount >= entry.fragTotal) {
        QByteArray assembled;
        assembled.reserve(entry.totalLen);
        for (uint8_t i = 0; i < entry.fragTotal; ++i) {
            auto it = entry.fragments.find(i);
            if (it == entry.fragments.end()) {
                m_reassembly.remove(msgId);
                return;
            }
            assembled.append(it.value());
        }
        m_reassembly.remove(msgId);
        emit dataReceived(assembled);
    }
}

void RawUdpTunnel::update() {
    uint32_t now = currentTimeMs();

    if (!m_dead && (now - m_lastRecvTime > static_cast<uint32_t>(RAW_UDP_DEAD_TIMEOUT_MS))) {
        m_dead = true;
        LogManager::instance().logDetail(QString("[raw_udp] Tunnel dead: no data for %1 ms, peer %2").arg(RAW_UDP_DEAD_TIMEOUT_MS).arg(m_peerAddr.toString()));
        emit tunnelDead();
        return;
    }

    if (now - m_lastSendTime > static_cast<uint32_t>(RAW_UDP_KEEPALIVE_MS)) {
        sendKeepalive();
    }

    if (m_fecEncoder) m_fecEncoder->update(now);
    if (m_fecDecoder) m_fecDecoder->cleanup(now);

    cleanupStaleEntries();
}

void RawUdpTunnel::sendKeepalive() {
    sendLatencyPing();
}

void RawUdpTunnel::sendLatencyPing() {
    uint32_t ts = currentTimeMs();
    char payload[5];
    payload[0] = static_cast<char>(RAW_UDP_LATENCY_PING);
    payload[1] = static_cast<char>((ts >> 24) & 0xFF);
    payload[2] = static_cast<char>((ts >> 16) & 0xFF);
    payload[3] = static_cast<char>((ts >> 8) & 0xFF);
    payload[4] = static_cast<char>(ts & 0xFF);
    sendFragment(payload, 5, 0, 1, m_nextMsgId++, 5);
}

void RawUdpTunnel::sendLatencyPong(const char* timestampData) {
    char payload[5];
    payload[0] = static_cast<char>(RAW_UDP_LATENCY_PONG);
    memcpy(payload + 1, timestampData, 4);
    sendFragment(payload, 5, 0, 1, m_nextMsgId++, 5);
}

int RawUdpTunnel::getRttMs() const {
    return m_rttMs;
}

void RawUdpTunnel::cleanupStaleEntries() {
    uint32_t now = currentTimeMs();
    QList<uint16_t> stale;
    for (auto it = m_reassembly.constBegin(); it != m_reassembly.constEnd(); ++it) {
        if (now - it.value().createTime > static_cast<uint32_t>(RAW_UDP_FRAG_TIMEOUT_MS)) {
            stale.append(it.key());
        }
    }
    if (!stale.isEmpty())
        LogManager::instance().logDetail(QString("[raw_udp] cleanupStaleEntries: removing %1 entries").arg(stale.size()));
    for (uint16_t id : stale) {
        m_reassembly.remove(id);
    }
}

int RawUdpTunnel::maxFragmentPayload() const {
    int payload = static_cast<int>(normalizeRoomMtu(m_roomMtu)) + CIPHER_OVERHEAD;
    if (payload > RAW_UDP_MAX_FRAG_PAYLOAD)
        payload = RAW_UDP_MAX_FRAG_PAYLOAD;
    return payload;
}

void RawUdpTunnel::setPeerEndpoint(const QHostAddress& addr, quint16 port) {
    m_peerAddr = addr;
    m_peerPort = port;
}

void RawUdpTunnel::setRelayMode(uint32_t srcPeerId, uint32_t dstPeerId) {
    m_relayMode      = true;
    m_relaySrcPeerId = srcPeerId;
    m_relayDstPeerId = dstPeerId;
}

bool RawUdpTunnel::isAlive() const {
    if (m_dead) return false;
    return (currentTimeMs() - m_lastRecvTime) < static_cast<uint32_t>(RAW_UDP_DEAD_TIMEOUT_MS);
}

} // namespace VLan

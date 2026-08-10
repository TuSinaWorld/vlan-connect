#include "fec_codec.h"
#include "net_common.h"
#include "../ui/log_manager.h"
#include <QStringList>
#include <cstring>

namespace VLan {

CM256& sharedCM256() {
    static CM256 instance;
    return instance;
}

// ─────────────────────── FecEncoder ───────────────────────

FecEncoder::FecEncoder(FecMode mode, OutputFunc output)
    : m_cm256(sharedCM256()), m_mode(mode), m_output(output), m_groupId(0),
      m_maxBlockSize(0), m_bufferStartTime(0)
{
}

void FecEncoder::addPacket(const QByteArray& payload) {
    if (m_buffer.isEmpty())
        m_bufferStartTime = currentTimeMs();

    m_buffer.append(payload);
    if (payload.size() > m_maxBlockSize)
        m_maxBlockSize = payload.size();

    if (g_verboseLog)
        LogManager::instance().logDetail(QString("[fec-enc] addPacket payloadSize=%1 bufferCount=%2 maxBlockSize=%3").arg(payload.size()).arg(m_buffer.size()).arg(m_maxBlockSize));

    if (m_buffer.size() >= FEC_GROUP_SIZE)
        flush();
}

void FecEncoder::flush() {
    if (m_buffer.isEmpty()) return;

    int N = m_buffer.size();
    int M = fecParityCount(m_mode, N);
    if (M <= 0) M = 1;

    int blockBytes = 2 + m_maxBlockSize;
    uint32_t bufferDuration = currentTimeMs() - m_bufferStartTime;

    if (g_verboseLog)
        LogManager::instance().logDetail(QString("[fec-enc] flush groupId=%1 N=%2 M=%3 blockBytes=%4 bufferMs=%5").arg(m_groupId).arg(N).arg(M).arg(blockBytes).arg(bufferDuration));

    // Build padded data blocks: [uint16_t len][payload][padding]
    QVector<QByteArray> padded(N);
    for (int i = 0; i < N; ++i) {
        padded[i].fill('\0', blockBytes);
        uint16_t plen = static_cast<uint16_t>(m_buffer[i].size());
        uint16_t netLen = htons(plen);
        memcpy(padded[i].data(), &netLen, 2);
        memcpy(padded[i].data() + 2, m_buffer[i].constData(), m_buffer[i].size());
    }

    // RS encode
    QVector<CM256::cm256_block> originals(N);
    for (int i = 0; i < N; ++i) {
        originals[i].Block = padded[i].data();
        originals[i].Index = static_cast<unsigned char>(i);
    }

    QByteArray recoveryData(M * blockBytes, '\0');
    CM256::cm256_encoder_params params;
    params.OriginalCount = N;
    params.RecoveryCount = M;
    params.BlockBytes    = blockBytes;

    int encResult = m_cm256.cm256_encode(params, originals.data(), recoveryData.data());
    if (encResult != 0) {
        LogManager::instance().logError(QString("[fec-enc] cm256_encode failed: result=%1 N=%2 M=%3 blockBytes=%4 - sending data without parity").arg(encResult).arg(N).arg(M).arg(blockBytes));
        uint8_t gid = m_groupId++;
        for (int i = 0; i < N; ++i) {
            FecHeader fh;
            fh.groupId    = gid;
            fh.index      = static_cast<uint8_t>(i);
            fh.dataCount  = static_cast<uint8_t>(N);
            fh.totalCount = static_cast<uint8_t>(N);

            QByteArray pkt;
            pkt.reserve(sizeof(FecHeader) + blockBytes);
            pkt.append(reinterpret_cast<const char*>(&fh), sizeof(fh));
            pkt.append(padded[i]);
            m_output(pkt);
        }
        m_buffer.clear();
        m_maxBlockSize = 0;
        return;
    }

    uint8_t gid = m_groupId++;

    // Emit data packets: [FecHeader][padded block]
    for (int i = 0; i < N; ++i) {
        FecHeader fh;
        fh.groupId    = gid;
        fh.index      = static_cast<uint8_t>(i);
        fh.dataCount  = static_cast<uint8_t>(N);
        fh.totalCount = static_cast<uint8_t>(N + M);

        QByteArray pkt;
        pkt.reserve(sizeof(FecHeader) + blockBytes);
        pkt.append(reinterpret_cast<const char*>(&fh), sizeof(fh));
        pkt.append(padded[i]);
        m_output(pkt);
    }

    // Emit parity packets: [FecHeader][parity block]
    for (int i = 0; i < M; ++i) {
        FecHeader fh;
        fh.groupId    = gid;
        fh.index      = static_cast<uint8_t>(N + i);
        fh.dataCount  = static_cast<uint8_t>(N);
        fh.totalCount = static_cast<uint8_t>(N + M);

        QByteArray pkt;
        pkt.reserve(sizeof(FecHeader) + blockBytes);
        pkt.append(reinterpret_cast<const char*>(&fh), sizeof(fh));
        pkt.append(recoveryData.constData() + i * blockBytes, blockBytes);
        m_output(pkt);
    }

    m_buffer.clear();
    m_maxBlockSize = 0;
}

void FecEncoder::update(uint32_t) {
    if (!m_buffer.isEmpty())
        flush();
}

// ─────────────────────── FecDecoder ───────────────────────

FecDecoder::FecDecoder(OutputFunc output)
    : m_cm256(sharedCM256()), m_output(output), m_bufferedBytes(0)
{
}

void FecDecoder::emitOriginalBlock(const QByteArray& block) {
    if (block.size() < 2) return;
    uint16_t netLen;
    memcpy(&netLen, block.constData(), 2);
    uint16_t payloadLen = ntohs(netLen);
    if (payloadLen > 0 && 2 + payloadLen <= block.size())
        m_output(QByteArray(block.constData() + 2, payloadLen));
}

void FecDecoder::addPacket(const char* data, int len) {
    if (len < static_cast<int>(sizeof(FecHeader))) return;

    const FecHeader* fh = reinterpret_cast<const FecHeader*>(data);
    uint8_t gid   = fh->groupId;
    uint8_t idx   = fh->index;
    uint8_t N     = fh->dataCount;
    uint8_t total = fh->totalCount;

    if (N < 1 || N > 10 || total < N || total > 3 * N || idx >= total)
        return;

    const char* blockData = data + sizeof(FecHeader);
    int blockLen = len - sizeof(FecHeader);
    if (blockLen <= 0) return;

    QMap<uint8_t, FecGroup>::iterator groupIt = m_groups.find(gid);
    if (groupIt != m_groups.end() &&
        (groupIt->dataCount != N || groupIt->totalCount != total ||
         groupIt->blockBytes != blockLen)) {
        removeGroup(gid);
        return;
    }
    if (groupIt == m_groups.end()) {
        if (!ensureCapacity(static_cast<size_t>(blockLen), true)) return;
        FecGroup group;
        group.blocks.clear();
        group.emittedOriginals.clear();
        group.dataCount  = N;
        group.totalCount = total;
        group.blockBytes = blockLen;
        group.createTime = currentTimeMs();
        group.decoded    = false;
        m_groups.insert(gid, group);
        groupIt = m_groups.find(gid);
        FecGroup& g = groupIt.value();
        if (g_verboseLog)
            LogManager::instance().logDetail(QString("[fec-dec] New group gid=%1 N=%2 total=%3 blockBytes=%4").arg(gid).arg(N).arg(total).arg(blockLen));
    }
    if (groupIt->decoded) return;

    if (!groupIt->blocks.contains(idx)) {
        if (!ensureCapacity(static_cast<size_t>(blockLen), false)) return;
        groupIt = m_groups.find(gid);
        if (groupIt == m_groups.end()) return;
        QByteArray block(blockData, blockLen);
        groupIt->blocks[idx] = block;
        m_bufferedBytes += static_cast<size_t>(block.size());

        if (idx < N) {
            emitOriginalBlock(block);
            groupIt->emittedOriginals.insert(idx);
        }
    }

    groupIt = m_groups.find(gid);
    if (groupIt == m_groups.end()) return;
    FecGroup& g = groupIt.value();

    if (g_verboseLog)
        LogManager::instance().logDetail(QString("[fec-dec] addPacket gid=%1 idx=%2 received=%3/%4 emitted=%5").arg(gid).arg(idx).arg(g.blocks.size()).arg(g.dataCount).arg(g.emittedOriginals.size()));

    if (g.blocks.size() >= g.dataCount)
        tryDecode(gid);
}

void FecDecoder::tryDecode(uint8_t groupId) {
    auto it = m_groups.find(groupId);
    if (it == m_groups.end()) return;
    FecGroup& g = it.value();
    if (g.decoded) return;

    int N = g.dataCount;
    int blockBytes = g.blockBytes;

    bool allOriginal = (g.emittedOriginals.size() >= N);

    if (allOriginal) {
        if (g_verboseLog)
            LogManager::instance().logDetail(QString("[fec-dec] tryDecode gid=%1 allOriginal=true N=%2 (already emitted)").arg(groupId).arg(N));
        g.decoded = true;
        return;
    }

    int M = g.totalCount - N;
    QStringList missingIndices;
    for (int i = 0; i < N; ++i) {
        if (!g.emittedOriginals.contains(static_cast<uint8_t>(i)))
            missingIndices << QString::number(i);
    }
    if (g_verboseLog)
        LogManager::instance().logDetail(QString("[fec-dec] tryDecode gid=%1 RS recovery N=%2 M=%3 missing=[%4] haveBlocks=%5").arg(groupId).arg(N).arg(M).arg(missingIndices.join(",")).arg(g.blocks.size()));

    QVector<QByteArray> blockStorage(N);
    QVector<CM256::cm256_block> blocks(N);
    int filled = 0;

    for (auto bit = g.blocks.begin(); bit != g.blocks.end() && filled < N; ++bit) {
        blockStorage[filled] = bit.value();
        if (blockStorage[filled].size() < blockBytes)
            blockStorage[filled].append(QByteArray(blockBytes - blockStorage[filled].size(), '\0'));
        blocks[filled].Block = blockStorage[filled].data();
        blocks[filled].Index = bit.key();
        ++filled;
    }

    if (filled < N) return;

    CM256::cm256_encoder_params params;
    params.OriginalCount = N;
    params.RecoveryCount = M;
    params.BlockBytes    = blockBytes;

    int decResult = m_cm256.cm256_decode(params, blocks.data());
    if (decResult != 0) {
        if (g_verboseLog)
            LogManager::instance().logDetail(QString("[fec-dec] cm256_decode failed gid=%1 (originals already emitted, relying on KCP retransmit)").arg(groupId));
        g.decoded = true;
        return;
    }

    QMap<uint8_t, const char*> origMap;
    for (int i = 0; i < N; ++i) {
        origMap[blocks[i].Index] = static_cast<const char*>(blocks[i].Block);
    }

    int recovered = 0;
    for (int i = 0; i < N; ++i) {
        uint8_t idx = static_cast<uint8_t>(i);
        if (g.emittedOriginals.contains(idx)) continue;
        auto found = origMap.find(idx);
        if (found == origMap.end()) continue;
        const char* blk = found.value();
        uint16_t netLen;
        memcpy(&netLen, blk, 2);
        uint16_t payloadLen = ntohs(netLen);
        if (payloadLen > 0 && 2 + static_cast<int>(payloadLen) <= blockBytes) {
            m_output(QByteArray(blk + 2, payloadLen));
            ++recovered;
        }
    }

    if (g_verboseLog)
        LogManager::instance().logDetail(QString("[fec-dec] tryDecode gid=%1 RS recovered %2 missing blocks").arg(groupId).arg(recovered));

    g.decoded = true;
}

void FecDecoder::cleanup(uint32_t nowMs) {
    QList<uint8_t> stale;
    for (auto it = m_groups.constBegin(); it != m_groups.constEnd(); ++it) {
        if (nowMs - it.value().createTime > GROUP_TIMEOUT_MS)
            stale.append(it.key());
    }

    if (!stale.isEmpty() && g_verboseLog)
        LogManager::instance().logDetail(QString("[fec-dec] cleanup: expiring %1 stale groups").arg(stale.size()));

    for (uint8_t gid : stale) {
        if (g_verboseLog) {
            const FecGroup& g = m_groups[gid];
            if (!g.decoded)
                LogManager::instance().logDetail(QString("[fec-dec] cleanup gid=%1 incomplete: received=%2/%3 emitted=%4").arg(gid).arg(g.blocks.size()).arg(g.dataCount).arg(g.emittedOriginals.size()));
        }
        removeGroup(gid);
    }
}

void FecDecoder::removeGroup(uint8_t groupId) {
    QMap<uint8_t, FecGroup>::iterator it = m_groups.find(groupId);
    if (it == m_groups.end()) return;
    size_t bytes = 0;
    for (QMap<uint8_t, QByteArray>::const_iterator block =
             it->blocks.constBegin(); block != it->blocks.constEnd(); ++block)
        bytes += static_cast<size_t>(block.value().size());
    m_bufferedBytes = bytes > m_bufferedBytes
        ? 0 : m_bufferedBytes - bytes;
    m_groups.erase(it);
}

bool FecDecoder::ensureCapacity(size_t incomingBytes, bool newGroup) {
    if (incomingBytes > FEC_MAX_BUFFERED_BYTES) return false;
    while (!m_groups.isEmpty() &&
           ((newGroup && m_groups.size() >=
               static_cast<int>(FEC_MAX_ACTIVE_GROUPS)) ||
            m_bufferedBytes + incomingBytes > FEC_MAX_BUFFERED_BYTES)) {
        QMap<uint8_t, FecGroup>::const_iterator oldest = m_groups.constBegin();
        for (QMap<uint8_t, FecGroup>::const_iterator it = m_groups.constBegin();
             it != m_groups.constEnd(); ++it) {
            if (it->createTime < oldest->createTime) oldest = it;
        }
        removeGroup(oldest.key());
    }
    return (!newGroup || m_groups.size() <
                static_cast<int>(FEC_MAX_ACTIVE_GROUPS)) &&
           m_bufferedBytes + incomingBytes <= FEC_MAX_BUFFERED_BYTES;
}

} // namespace VLan

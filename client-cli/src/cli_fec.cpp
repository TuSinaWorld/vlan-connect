#include "cli_fec.h"
#include "cli_log.h"
#include <cstring>
#include <algorithm>

namespace VLan {

CM256& sharedCM256() {
    static CM256 instance;
    return instance;
}

// ======================== FecEncoder ========================

CliFecEncoder::CliFecEncoder(FecMode mode, OutputFunc output)
    : m_cm256(sharedCM256()), m_mode(mode), m_output(output),
      m_groupId(0), m_maxBlockSize(0), m_bufferStartTime(0)
{}

void CliFecEncoder::addPacket(const Buffer& payload) {
    if (m_buffer.empty())
        m_bufferStartTime = currentTimeMs();

    m_buffer.push_back(payload);
    if (static_cast<int>(payload.size()) > m_maxBlockSize)
        m_maxBlockSize = static_cast<int>(payload.size());

    if (static_cast<int>(m_buffer.size()) >= FEC_GROUP_SIZE)
        flush();
}

void CliFecEncoder::flush() {
    if (m_buffer.empty()) return;

    int N = static_cast<int>(m_buffer.size());
    int M = fecParityCount(m_mode, N);
    if (M <= 0) M = 1;

    int blockBytes = 2 + m_maxBlockSize;

    std::vector<Buffer> padded(N);
    for (int i = 0; i < N; ++i) {
        padded[i].resize(blockBytes, 0);
        uint16_t plen = static_cast<uint16_t>(m_buffer[i].size());
        uint16_t netLen = htons(plen);
        memcpy(padded[i].data(), &netLen, 2);
        memcpy(padded[i].data() + 2, m_buffer[i].data(), m_buffer[i].size());
    }

    std::vector<CM256::cm256_block> originals(N);
    for (int i = 0; i < N; ++i) {
        originals[i].Block = padded[i].data();
        originals[i].Index = static_cast<unsigned char>(i);
    }

    Buffer recoveryData(M * blockBytes, 0);
    CM256::cm256_encoder_params params;
    params.OriginalCount = N;
    params.RecoveryCount = M;
    params.BlockBytes    = blockBytes;

    int encResult = m_cm256.cm256_encode(params, originals.data(), recoveryData.data());

    uint8_t gid = m_groupId++;
    uint8_t totalCount = (encResult == 0) ? static_cast<uint8_t>(N + M) : static_cast<uint8_t>(N);

    for (int i = 0; i < N; ++i) {
        FecHeader fh;
        fh.groupId    = gid;
        fh.index      = static_cast<uint8_t>(i);
        fh.dataCount  = static_cast<uint8_t>(N);
        fh.totalCount = totalCount;

        Buffer pkt;
        pkt.reserve(sizeof(FecHeader) + blockBytes);
        pkt.insert(pkt.end(), reinterpret_cast<const uint8_t*>(&fh),
                   reinterpret_cast<const uint8_t*>(&fh) + sizeof(fh));
        pkt.insert(pkt.end(), padded[i].begin(), padded[i].end());
        m_output(pkt);
    }

    if (encResult == 0) {
        for (int i = 0; i < M; ++i) {
            FecHeader fh;
            fh.groupId    = gid;
            fh.index      = static_cast<uint8_t>(N + i);
            fh.dataCount  = static_cast<uint8_t>(N);
            fh.totalCount = static_cast<uint8_t>(N + M);

            Buffer pkt;
            pkt.reserve(sizeof(FecHeader) + blockBytes);
            pkt.insert(pkt.end(), reinterpret_cast<const uint8_t*>(&fh),
                       reinterpret_cast<const uint8_t*>(&fh) + sizeof(fh));
            pkt.insert(pkt.end(),
                       recoveryData.begin() + i * blockBytes,
                       recoveryData.begin() + (i + 1) * blockBytes);
            m_output(pkt);
        }
    }

    m_buffer.clear();
    m_maxBlockSize = 0;
}

void CliFecEncoder::update(uint32_t) {
    if (!m_buffer.empty())
        flush();
}

// ======================== FecDecoder ========================

CliFecDecoder::CliFecDecoder(OutputFunc output)
    : m_cm256(sharedCM256()), m_output(output), m_bufferedBytes(0)
{}

void CliFecDecoder::emitOriginalBlock(const Buffer& block) {
    if (block.size() < 2) return;
    uint16_t netLen;
    memcpy(&netLen, block.data(), 2);
    uint16_t payloadLen = ntohs(netLen);
    if (payloadLen > 0 && 2 + payloadLen <= static_cast<int>(block.size()))
        m_output(Buffer(block.begin() + 2, block.begin() + 2 + payloadLen));
}

void CliFecDecoder::addPacket(const char* data, int len) {
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

    std::map<uint8_t, FecGroup>::iterator groupIt = m_groups.find(gid);
    if (groupIt != m_groups.end() &&
        (groupIt->second.dataCount != N ||
         groupIt->second.totalCount != total ||
         groupIt->second.blockBytes != blockLen)) {
        removeGroup(gid);
        return;
    }
    if (groupIt == m_groups.end()) {
        if (!ensureCapacity(static_cast<size_t>(blockLen), true)) return;
        FecGroup group;
        group.dataCount = N;
        group.totalCount = total;
        group.blockBytes = blockLen;
        group.createTime = currentTimeMs();
        group.decoded = false;
        m_groups[gid] = group;
        groupIt = m_groups.find(gid);
    }

    if (groupIt->second.decoded) return;

    if (groupIt->second.blocks.find(idx) == groupIt->second.blocks.end()) {
        if (!ensureCapacity(static_cast<size_t>(blockLen), false)) return;
        groupIt = m_groups.find(gid);
        if (groupIt == m_groups.end()) return;
        Buffer block(blockData, blockData + blockLen);
        groupIt->second.blocks[idx] = block;
        m_bufferedBytes += block.size();

        if (idx < N) {
            emitOriginalBlock(block);
            groupIt->second.emittedOriginals.insert(idx);
        }
    }

    groupIt = m_groups.find(gid);
    if (groupIt == m_groups.end()) return;
    FecGroup& g = groupIt->second;
    if (static_cast<int>(g.blocks.size()) >= g.dataCount)
        tryDecode(gid);
}

void CliFecDecoder::tryDecode(uint8_t groupId) {
    auto it = m_groups.find(groupId);
    if (it == m_groups.end()) return;
    FecGroup& g = it->second;
    if (g.decoded) return;

    int N = g.dataCount;
    int blockBytes = g.blockBytes;

    if (static_cast<int>(g.emittedOriginals.size()) >= N) {
        g.decoded = true;
        return;
    }

    int M = g.totalCount - N;

    std::vector<Buffer> blockStorage(N);
    std::vector<CM256::cm256_block> blocks(N);
    int filled = 0;

    for (auto bit = g.blocks.begin(); bit != g.blocks.end() && filled < N; ++bit) {
        blockStorage[filled] = bit->second;
        if (static_cast<int>(blockStorage[filled].size()) < blockBytes)
            blockStorage[filled].resize(blockBytes, 0);
        blocks[filled].Block = blockStorage[filled].data();
        blocks[filled].Index = bit->first;
        ++filled;
    }

    if (filled < N) return;

    CM256::cm256_encoder_params params;
    params.OriginalCount = N;
    params.RecoveryCount = M;
    params.BlockBytes    = blockBytes;

    int decResult = m_cm256.cm256_decode(params, blocks.data());
    if (decResult != 0) {
        g.decoded = true;
        return;
    }

    std::map<uint8_t, const char*> origMap;
    for (int i = 0; i < N; ++i)
        origMap[blocks[i].Index] = static_cast<const char*>(blocks[i].Block);

    for (int i = 0; i < N; ++i) {
        uint8_t idx = static_cast<uint8_t>(i);
        if (g.emittedOriginals.count(idx)) continue;
        auto found = origMap.find(idx);
        if (found == origMap.end()) continue;
        const char* blk = found->second;
        uint16_t netLen;
        memcpy(&netLen, blk, 2);
        uint16_t payloadLen = ntohs(netLen);
        if (payloadLen > 0 && 2 + static_cast<int>(payloadLen) <= blockBytes)
            m_output(Buffer(blk + 2, blk + 2 + payloadLen));
    }

    g.decoded = true;
}

void CliFecDecoder::cleanup(uint32_t nowMs) {
    std::vector<uint8_t> stale;
    for (auto it = m_groups.begin(); it != m_groups.end(); ++it) {
        if (nowMs - it->second.createTime > GROUP_TIMEOUT_MS)
            stale.push_back(it->first);
    }
    for (uint8_t gid : stale)
        removeGroup(gid);
}

void CliFecDecoder::removeGroup(uint8_t groupId) {
    std::map<uint8_t, FecGroup>::iterator it = m_groups.find(groupId);
    if (it == m_groups.end()) return;
    size_t bytes = 0;
    for (std::map<uint8_t, Buffer>::const_iterator block =
             it->second.blocks.begin(); block != it->second.blocks.end(); ++block)
        bytes += block->second.size();
    m_bufferedBytes = bytes > m_bufferedBytes
        ? 0 : m_bufferedBytes - bytes;
    m_groups.erase(it);
}

bool CliFecDecoder::ensureCapacity(size_t incomingBytes, bool newGroup) {
    if (incomingBytes > FEC_MAX_BUFFERED_BYTES) return false;
    while (!m_groups.empty() &&
           ((newGroup && m_groups.size() >= FEC_MAX_ACTIVE_GROUPS) ||
            m_bufferedBytes + incomingBytes > FEC_MAX_BUFFERED_BYTES)) {
        std::map<uint8_t, FecGroup>::const_iterator oldest = m_groups.begin();
        for (std::map<uint8_t, FecGroup>::const_iterator it = m_groups.begin();
             it != m_groups.end(); ++it) {
            if (it->second.createTime < oldest->second.createTime) oldest = it;
        }
        removeGroup(oldest->first);
    }
    return (!newGroup || m_groups.size() < FEC_MAX_ACTIVE_GROUPS) &&
           m_bufferedBytes + incomingBytes <= FEC_MAX_BUFFERED_BYTES;
}

} // namespace VLan

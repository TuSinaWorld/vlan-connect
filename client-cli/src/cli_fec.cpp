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
    : m_cm256(sharedCM256()), m_output(output)
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

    if (N == 0 || total < N) return;

    const char* blockData = data + sizeof(FecHeader);
    int blockLen = len - sizeof(FecHeader);
    if (blockLen <= 0) return;

    FecGroup& g = m_groups[gid];
    if (g.blocks.empty()) {
        g.dataCount  = N;
        g.totalCount = total;
        g.blockBytes = blockLen;
        g.createTime = currentTimeMs();
        g.decoded    = false;
    }

    if (g.decoded) return;

    if (g.blocks.find(idx) == g.blocks.end()) {
        Buffer block(blockData, blockData + blockLen);
        if (static_cast<int>(block.size()) < g.blockBytes)
            block.resize(g.blockBytes, 0);
        g.blocks[idx] = block;

        if (idx < N) {
            emitOriginalBlock(block);
            g.emittedOriginals.insert(idx);
        }
    }

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
        m_groups.erase(gid);
}

} // namespace VLan

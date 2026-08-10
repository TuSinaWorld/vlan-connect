#ifndef VLAN_CLI_FEC_H
#define VLAN_CLI_FEC_H

#include "cli_common.h"
#include "cm256.h"
#include <functional>
#include <map>
#include <set>

namespace VLan {

CM256& sharedCM256();

class CliFecEncoder {
public:
    typedef std::function<void(const Buffer&)> OutputFunc;

    CliFecEncoder(FecMode mode, OutputFunc output);

    void addPacket(const Buffer& payload);
    void flush();
    void update(uint32_t nowMs);

private:
    CM256&           m_cm256;
    FecMode          m_mode;
    OutputFunc       m_output;
    uint8_t          m_groupId;
    int              m_maxBlockSize;
    uint32_t         m_bufferStartTime;
    std::vector<Buffer> m_buffer;
};

class CliFecDecoder {
public:
    typedef std::function<void(const Buffer&)> OutputFunc;

    CliFecDecoder(OutputFunc output);

    void addPacket(const char* data, int len);
    void cleanup(uint32_t nowMs);

private:
    struct FecGroup {
        uint8_t  dataCount;
        uint8_t  totalCount;
        int      blockBytes;
        uint32_t createTime;
        bool     decoded;
        std::map<uint8_t, Buffer> blocks;
        std::set<uint8_t> emittedOriginals;
    };

    void tryDecode(uint8_t groupId);
    void emitOriginalBlock(const Buffer& block);
    void removeGroup(uint8_t groupId);
    bool ensureCapacity(size_t incomingBytes, bool newGroup);

    CM256&    m_cm256;
    OutputFunc m_output;
    std::map<uint8_t, FecGroup> m_groups;
    size_t m_bufferedBytes;

    static const uint32_t GROUP_TIMEOUT_MS = 500;
};

} // namespace VLan
#endif // VLAN_CLI_FEC_H

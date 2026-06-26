#ifndef VLAN_FEC_CODEC_H
#define VLAN_FEC_CODEC_H

#include <QByteArray>
#include <QVector>
#include <QMap>
#include <QSet>
#include <functional>
#include <cstring>
#include "protocol.h"
#include "cm256.h"

namespace VLan {

CM256& sharedCM256();

class FecEncoder {
public:
    using OutputFunc = std::function<void(const QByteArray&)>;

    FecEncoder(FecMode mode, OutputFunc output);

    void addPacket(const QByteArray& payload);
    void flush();
    void update(uint32_t nowMs);

private:
    CM256&      m_cm256;
    FecMode     m_mode;
    OutputFunc  m_output;
    uint8_t     m_groupId;
    int         m_maxBlockSize;
    uint32_t    m_bufferStartTime;
    QVector<QByteArray> m_buffer;
};

class FecDecoder {
public:
    using OutputFunc = std::function<void(const QByteArray&)>;

    FecDecoder(OutputFunc output);

    void addPacket(const char* data, int len);
    void cleanup(uint32_t nowMs);

private:
    struct FecGroup {
        uint8_t  dataCount;
        uint8_t  totalCount;
        int      blockBytes;
        uint32_t createTime;
        bool     decoded;
        QMap<uint8_t, QByteArray> blocks;
        QSet<uint8_t> emittedOriginals;
    };

    void tryDecode(uint8_t groupId);
    void emitOriginalBlock(const QByteArray& block);

    CM256&     m_cm256;
    OutputFunc m_output;
    QMap<uint8_t, FecGroup> m_groups;

    static const uint32_t GROUP_TIMEOUT_MS = 500;
};

} // namespace VLan
#endif // VLAN_FEC_CODEC_H

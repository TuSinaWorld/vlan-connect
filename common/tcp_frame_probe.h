#ifndef VLAN_TCP_FRAME_PROBE_H
#define VLAN_TCP_FRAME_PROBE_H

#include "protocol.h"
#include <cstddef>
#include <cstdint>

namespace VLan {

enum class TcpFrameProbeStatus {
    NeedMore,
    Ready,
    Malformed
};

struct TcpFrameProbeResult {
    TcpFrameProbeStatus status;
    uint8_t msgType;
    uint16_t payloadLength;
    size_t frameLength;

    TcpFrameProbeResult()
        : status(TcpFrameProbeStatus::NeedMore),
          msgType(0), payloadLength(0), frameLength(0) {}
};

inline TcpFrameProbeResult probeTcpFrame(
    const uint8_t* data, size_t len)
{
    static const size_t HEADER_SIZE = 3;
    TcpFrameProbeResult result;
    if (len < HEADER_SIZE)
        return result;
    if (!data) {
        result.status = TcpFrameProbeStatus::Malformed;
        return result;
    }

    result.msgType = data[0];
    result.payloadLength = static_cast<uint16_t>(
        (static_cast<uint16_t>(data[1]) << 8) |
         static_cast<uint16_t>(data[2]));
    if (result.payloadLength > MAX_TCP_MSG_PAYLOAD) {
        result.status = TcpFrameProbeStatus::Malformed;
        return result;
    }
    result.frameLength = HEADER_SIZE + result.payloadLength;
    result.status = len < result.frameLength
        ? TcpFrameProbeStatus::NeedMore
        : TcpFrameProbeStatus::Ready;
    return result;
}

} // namespace VLan

#endif // VLAN_TCP_FRAME_PROBE_H

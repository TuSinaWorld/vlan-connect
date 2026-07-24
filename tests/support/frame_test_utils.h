#ifndef VLAN_TEST_FRAME_TEST_UTILS_H
#define VLAN_TEST_FRAME_TEST_UTILS_H

#include "../../common/tcp_frame_probe.h"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace VLanTest {

inline std::vector<uint8_t> makeTcpFrame(
    uint8_t type, const std::vector<uint8_t>& payload)
{
    std::vector<uint8_t> frame;
    frame.reserve(3 + payload.size());
    frame.push_back(type);
    frame.push_back(static_cast<uint8_t>((payload.size() >> 8) & 0xff));
    frame.push_back(static_cast<uint8_t>(payload.size() & 0xff));
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

inline bool popTcpFrame(std::vector<uint8_t>* stream,
                        uint8_t* type,
                        std::vector<uint8_t>* payload)
{
    if (!stream || !type || !payload)
        return false;
    const VLan::TcpFrameProbeResult frame = VLan::probeTcpFrame(
        stream->empty() ? nullptr : stream->data(), stream->size());
    if (frame.status != VLan::TcpFrameProbeStatus::Ready)
        return false;
    *type = frame.msgType;
    payload->assign(stream->begin() + 3,
                    stream->begin() + frame.frameLength);
    stream->erase(stream->begin(),
                  stream->begin() + frame.frameLength);
    return true;
}

} // namespace VLanTest

#endif // VLAN_TEST_FRAME_TEST_UTILS_H

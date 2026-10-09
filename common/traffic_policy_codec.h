#ifndef VLAN_TRAFFIC_POLICY_CODEC_H
#define VLAN_TRAFFIC_POLICY_CODEC_H

#include "byte_buffer.h"
#include "protocol.h"

namespace VLan {

inline RoomTrafficPolicy readTrafficPolicy(
    ByteBuffer& buffer, const RoomTrafficPolicy& fallback)
{
    // Each read advances the cursor. Keep them in separate statements:
    // function argument evaluation order is unspecified in C++11.
    const uint8_t transport = buffer.readU8();
    const uint8_t fec = buffer.readU8();
    const uint8_t profile = buffer.readU8();
    return normalizeTrafficPolicy(transport, fec, profile, fallback);
}

} // namespace VLan

#endif // VLAN_TRAFFIC_POLICY_CODEC_H

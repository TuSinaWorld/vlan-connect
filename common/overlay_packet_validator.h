#ifndef VLAN_OVERLAY_PACKET_VALIDATOR_H
#define VLAN_OVERLAY_PACKET_VALIDATOR_H

#include "protocol.h"
#include <cstddef>
#include <cstdint>

namespace VLan {

enum class OverlayPacketError : uint8_t {
    None,
    Truncated,
    NotIpv4,
    InvalidHeaderLength,
    InvalidTotalLength,
    ExceedsMtu,
    InvalidSource,
    InvalidDestination
};

struct OverlayPacketResult {
    OverlayPacketError error;
    uint32_t source;
    uint32_t destination;

    OverlayPacketResult(OverlayPacketError e = OverlayPacketError::None,
                        uint32_t src = 0, uint32_t dst = 0)
        : error(e), source(src), destination(dst) {}

    bool isValid() const { return error == OverlayPacketError::None; }
};

inline uint32_t readOverlayAddress(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
            static_cast<uint32_t>(p[3]);
}

inline OverlayPacketResult validateOverlayIpv4Structure(
    const uint8_t* packet, size_t length, uint16_t roomMtu)
{
    if (!packet || length < 20)
        return OverlayPacketResult(OverlayPacketError::Truncated);
    if ((packet[0] >> 4) != 4)
        return OverlayPacketResult(OverlayPacketError::NotIpv4);
    const size_t headerLength = static_cast<size_t>(packet[0] & 0x0f) * 4;
    if (headerLength < 20 || headerLength > length)
        return OverlayPacketResult(OverlayPacketError::InvalidHeaderLength);
    const size_t totalLength =
        (static_cast<size_t>(packet[2]) << 8) | packet[3];
    if (totalLength < headerLength || totalLength != length)
        return OverlayPacketResult(OverlayPacketError::InvalidTotalLength);
    if (length > normalizeRoomMtu(roomMtu))
        return OverlayPacketResult(OverlayPacketError::ExceedsMtu);
    return OverlayPacketResult(OverlayPacketError::None,
                               readOverlayAddress(packet + 12),
                               readOverlayAddress(packet + 16));
}

inline OverlayPacketResult validateInboundOverlayIpv4(
    const uint8_t* packet, size_t length, uint16_t roomMtu,
    uint32_t expectedSource, uint32_t localAddress)
{
    OverlayPacketResult result =
        validateOverlayIpv4Structure(packet, length, roomMtu);
    if (!result.isValid()) return result;
    if (expectedSource == 0 || result.source != expectedSource)
        return OverlayPacketResult(OverlayPacketError::InvalidSource,
                                   result.source, result.destination);
    if (localAddress == 0 ||
        (result.destination != localAddress &&
         result.destination != VNET_BROADCAST &&
         result.destination != 0xffffffffu)) {
        return OverlayPacketResult(OverlayPacketError::InvalidDestination,
                                   result.source, result.destination);
    }
    return result;
}

inline OverlayPacketResult validateOutboundOverlayIpv4(
    const uint8_t* packet, size_t length, uint16_t roomMtu,
    uint32_t localAddress)
{
    OverlayPacketResult result =
        validateOverlayIpv4Structure(packet, length, roomMtu);
    if (!result.isValid()) return result;
    if (localAddress == 0 || result.source != localAddress)
        return OverlayPacketResult(OverlayPacketError::InvalidSource,
                                   result.source, result.destination);
    return result;
}

} // namespace VLan

#endif // VLAN_OVERLAY_PACKET_VALIDATOR_H

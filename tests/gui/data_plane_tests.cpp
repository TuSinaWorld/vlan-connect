#include "../../common/protocol.h"
#include "../../common/secure_frame.h"
#include "../../common/overlay_packet_validator.h"
#include <cassert>
#include <cstdint>
#include <vector>

using namespace VLan;

static void testStateMatrix() {
    assert(!dataPlaneAllowsTraffic(
        DataPlaneState::Stopped,
        DataPlaneSecurityMode::Unconfigured));
    assert(!dataPlaneAllowsTraffic(
        DataPlaneState::Stopped,
        DataPlaneSecurityMode::Secure));
    assert(!dataPlaneAllowsTraffic(
        DataPlaneState::Running,
        DataPlaneSecurityMode::Unconfigured));
    assert(dataPlaneAllowsTraffic(
        DataPlaneState::Running,
        DataPlaneSecurityMode::Secure));

    assert(dataPlaneCanReconfigure(DataPlaneState::Stopped));
    assert(!dataPlaneCanReconfigure(DataPlaneState::Running));
}

static std::vector<uint8_t> ipv4Packet(uint32_t source, uint32_t destination) {
    std::vector<uint8_t> packet(20, 0);
    packet[0] = 0x45;
    packet[2] = 0;
    packet[3] = 20;
    packet[12] = static_cast<uint8_t>(source >> 24);
    packet[13] = static_cast<uint8_t>(source >> 16);
    packet[14] = static_cast<uint8_t>(source >> 8);
    packet[15] = static_cast<uint8_t>(source);
    packet[16] = static_cast<uint8_t>(destination >> 24);
    packet[17] = static_cast<uint8_t>(destination >> 16);
    packet[18] = static_cast<uint8_t>(destination >> 8);
    packet[19] = static_cast<uint8_t>(destination);
    return packet;
}

static void testOverlayValidation() {
    const uint32_t local = VNET_SUBNET | 2;
    const uint32_t remote = VNET_SUBNET | 3;
    std::vector<uint8_t> packet = ipv4Packet(remote, local);
    assert(validateInboundOverlayIpv4(
        packet.data(), packet.size(), ROOM_MTU_DEFAULT,
        remote, local).isValid());

    packet[12] = 10;
    packet[15] = 4;
    assert(!validateInboundOverlayIpv4(
        packet.data(), packet.size(), ROOM_MTU_DEFAULT,
        remote, local).isValid());

    packet = ipv4Packet(remote, VNET_BROADCAST);
    assert(validateInboundOverlayIpv4(
        packet.data(), packet.size(), ROOM_MTU_DEFAULT,
        remote, local).isValid());
    packet = ipv4Packet(remote, 0xffffffffu);
    assert(validateInboundOverlayIpv4(
        packet.data(), packet.size(), ROOM_MTU_DEFAULT,
        remote, local).isValid());

    packet = ipv4Packet(local, remote);
    assert(validateOutboundOverlayIpv4(
        packet.data(), packet.size(), ROOM_MTU_DEFAULT,
        local).isValid());
    packet[0] = 0x65;
    assert(!validateOutboundOverlayIpv4(
        packet.data(), packet.size(), ROOM_MTU_DEFAULT,
        local).isValid());
    packet = ipv4Packet(local, remote);
    packet[3] = 21;
    assert(!validateOutboundOverlayIpv4(
        packet.data(), packet.size(), ROOM_MTU_DEFAULT,
        local).isValid());
    assert(!validateOutboundOverlayIpv4(
        packet.data(), packet.size(), 19, local).isValid());
}

static void testCipherResetRestartsCounters() {
    uint8_t master[SECURE_KEY_SIZE] = {};
    master[0] = 1;
    const uint8_t plain[] = { 1, 2, 3 };

    SecureFrameCipher cipher;
    cipher.init(master, true, "data-plane-test");
    std::vector<uint8_t> first =
        cipher.encrypt(plain, sizeof(plain));
    std::vector<uint8_t> second =
        cipher.encrypt(plain, sizeof(plain));
    assert(readU64BE(first.data()) == 0);
    assert(readU64BE(second.data()) == 1);

    cipher.reset();
    cipher.init(master, true, "data-plane-test");
    std::vector<uint8_t> afterReset =
        cipher.encrypt(plain, sizeof(plain));
    assert(readU64BE(afterReset.data()) == 0);
}

int main() {
    testStateMatrix();
    testOverlayValidation();
    testCipherResetRestartsCounters();
    return 0;
}

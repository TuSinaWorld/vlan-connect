#include "../../common/protocol.h"
#include "../../common/secure_frame.h"
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
        DataPlaneSecurityMode::Plaintext));
    assert(!dataPlaneAllowsTraffic(
        DataPlaneState::Stopped,
        DataPlaneSecurityMode::Secure));
    assert(!dataPlaneAllowsTraffic(
        DataPlaneState::Running,
        DataPlaneSecurityMode::Unconfigured));
    assert(dataPlaneAllowsTraffic(
        DataPlaneState::Running,
        DataPlaneSecurityMode::Plaintext));
    assert(dataPlaneAllowsTraffic(
        DataPlaneState::Running,
        DataPlaneSecurityMode::Secure));

    assert(dataPlaneCanReconfigure(DataPlaneState::Stopped));
    assert(!dataPlaneCanReconfigure(DataPlaneState::Running));
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
    testCipherResetRestartsCounters();
    return 0;
}

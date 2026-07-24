#include "../../common/signal_message_validator.h"
#include "../../common/secure_frame.h"
#include <cassert>
#include <cstdint>
#include <vector>

using namespace VLan;

struct ChannelIsolationState {
    bool signalConnected;
    bool dataConnected;
};

static void applyDataValidation(
    ChannelIsolationState* state,
    const MessageValidationResult& validation)
{
    if (state &&
        validation.status == MessageValidationStatus::Malformed) {
        state->dataConnected = false;
    }
}

static void testEncryptedInnerValidation() {
    uint8_t master[SECURE_KEY_SIZE] = {};
    master[0] = 0x42;
    SecureFrameCipher serverCipher;
    SecureFrameCipher clientCipher;
    serverCipher.init(master, false, "data");
    clientCipher.init(master, true, "data");

    const uint8_t validInner[] = { MSG_DATA_CHANNEL_ACK };
    std::vector<uint8_t> encrypted =
        serverCipher.encrypt(validInner, sizeof(validInner));
    assert(validateServerDataPayload(
        MSG_ENCRYPTED, encrypted.data(), encrypted.size()).isValid());

    std::vector<uint8_t> plain;
    assert(clientCipher.decrypt(
        encrypted.data(), encrypted.size(), &plain));
    assert(plain.size() == 1);
    assert(validateServerDataPayload(
        plain[0], nullptr, 0).isValid());

    const uint8_t malformedInner[] = {
        MSG_DATA_CHANNEL_ACK, 0x01
    };
    encrypted = serverCipher.encrypt(
        malformedInner, sizeof(malformedInner));
    assert(clientCipher.decrypt(
        encrypted.data(), encrypted.size(), &plain));
    const MessageValidationResult malformed =
        validateServerDataPayload(
            plain[0], plain.data() + 1, plain.size() - 1);
    assert(malformed.status == MessageValidationStatus::Malformed);

    ChannelIsolationState state = { true, true };
    applyDataValidation(&state, malformed);
    assert(state.signalConnected);
    assert(!state.dataConnected);
}

static void testEveryEncryptedPrefixIsRejected() {
    std::vector<uint8_t> frame(
        SECURE_COUNTER_SIZE + 1 + SECURE_MAC_SIZE, 0);
    for (size_t len = 0; len < frame.size(); ++len) {
        const MessageValidationResult result =
            validateServerDataPayload(
                MSG_ENCRYPTED,
                len == 0 ? nullptr : frame.data(), len);
        assert(result.status == MessageValidationStatus::Malformed);
    }
}

int main() {
    testEncryptedInnerValidation();
    testEveryEncryptedPrefixIsRejected();
    return 0;
}

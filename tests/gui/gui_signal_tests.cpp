#include "../../common/byte_buffer.h"
#include "../../common/signal_message_validator.h"
#include "../support/frame_test_utils.h"
#include <cassert>
#include <vector>

using namespace VLan;
using namespace VLanTest;

static std::vector<uint8_t> serverHelloPayload() {
    std::vector<uint8_t> payload;
    payload.push_back(static_cast<uint8_t>(
        (PROTOCOL_VERSION >> 8) & 0xff));
    payload.push_back(static_cast<uint8_t>(
        PROTOCOL_VERSION & 0xff));
    payload.push_back(0);
    return payload;
}

static void testFragmentedAndBatchedFrames() {
    const std::vector<uint8_t> hello =
        makeTcpFrame(MSG_SERVER_HELLO, serverHelloPayload());
    for (size_t len = 0; len < hello.size(); ++len) {
        const TcpFrameProbeResult probe =
            probeTcpFrame(hello.data(), len);
        assert(probe.status == TcpFrameProbeStatus::NeedMore);
    }
    assert(probeTcpFrame(hello.data(), hello.size()).status ==
           TcpFrameProbeStatus::Ready);

    std::vector<uint8_t> stream = hello;
    const std::vector<uint8_t> pong =
        makeTcpFrame(MSG_PONG, std::vector<uint8_t>());
    stream.insert(stream.end(), pong.begin(), pong.end());

    uint8_t type = 0;
    std::vector<uint8_t> payload;
    assert(popTcpFrame(&stream, &type, &payload));
    assert(type == MSG_SERVER_HELLO);
    assert(validateServerSignalPayload(
        type, payload.data(), payload.size()).isValid());
    assert(popTcpFrame(&stream, &type, &payload));
    assert(type == MSG_PONG);
    assert(payload.empty());
    assert(stream.empty());
}

static void testCallbackDisconnectStopsBatch() {
    std::vector<uint8_t> stream =
        makeTcpFrame(MSG_PONG, std::vector<uint8_t>());
    const std::vector<uint8_t> second =
        makeTcpFrame(MSG_PONG, std::vector<uint8_t>());
    stream.insert(stream.end(), second.begin(), second.end());

    bool connected = true;
    int callbacks = 0;
    while (connected) {
        uint8_t type = 0;
        std::vector<uint8_t> payload;
        if (!popTcpFrame(&stream, &type, &payload))
            break;
        assert(validateServerSignalPayload(
            type, payload.empty() ? nullptr : payload.data(),
            payload.size()).isValid());
        ++callbacks;
        connected = false;
    }
    assert(callbacks == 1);
    assert(!stream.empty());
}

static void testKnownMessageTailIsMalformed() {
    const uint8_t tail = 1;
    const MessageValidationResult result =
        validateServerSignalPayload(MSG_LOGOUT_ACK, &tail, 1);
    assert(result.status == MessageValidationStatus::Malformed);
    assert(result.error == MessageValidationError::TrailingData);
}

static void testSignalRelayFallbackPayload() {
    ByteBuffer body;
    body.writeU32(7);
    body.writeU32(8);
    body.writeU8(TRAFFIC_TCP);
    const uint8_t packet[] = { 0x45, 0x00, 0x00, 0x14 };
    body.writeBytes(packet, sizeof(packet));

    assert(validateServerSignalPayload(
        MSG_TCP_RELAY_DATA, body.data(), body.size()).isValid());
    ByteBuffer parsed(body.data(), body.size());
    assert(parsed.readU32() == 7);
    assert(parsed.readU32() == 8);
    assert(parsed.readU8() == TRAFFIC_TCP);
    assert(parsed.remaining() == sizeof(packet));
}

int main() {
    testFragmentedAndBatchedFrames();
    testCallbackDisconnectStopsBatch();
    testKnownMessageTailIsMalformed();
    testSignalRelayFallbackPayload();
    return 0;
}

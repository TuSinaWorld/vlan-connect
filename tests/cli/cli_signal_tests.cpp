#include "../../common/byte_buffer.h"
#include "../../common/signal_message_validator.h"
#include "cli_common.h"
#include "../support/frame_test_utils.h"
#include <cassert>
#include <vector>

using namespace VLan;
using namespace VLanTest;

static void testVersionEightOnly() {
    std::vector<uint8_t> v8(2 + 1 + 16 + 32, 0);
    v8[0] = static_cast<uint8_t>(PROTOCOL_VERSION >> 8);
    v8[1] = static_cast<uint8_t>(PROTOCOL_VERSION);
    v8[2] = 1;
    assert(validateServerSignalPayload(
        MSG_SERVER_HELLO, v8.data(), v8.size()).isValid());

    std::vector<uint8_t> v7 = v8;
    v7[1] = static_cast<uint8_t>(PROTOCOL_VERSION - 1);
    const MessageValidationResult old =
        validateServerSignalPayload(
            MSG_SERVER_HELLO, v7.data(), v7.size());
    assert(old.status == MessageValidationStatus::Malformed);
    assert(old.error == MessageValidationError::InvalidVersion);
}

static void testBatchExtractionAndUnknownMessage() {
    std::vector<uint8_t> stream =
        makeTcpFrame(0x7f, std::vector<uint8_t>(4, 0xaa));
    const std::vector<uint8_t> pong =
        makeTcpFrame(MSG_PONG, std::vector<uint8_t>());
    stream.insert(stream.end(), pong.begin(), pong.end());

    uint8_t type = 0;
    std::vector<uint8_t> payload;
    assert(popTcpFrame(&stream, &type, &payload));
    assert(validateServerSignalPayload(
        type, payload.data(), payload.size()).status ==
           MessageValidationStatus::UnknownType);
    assert(popTcpFrame(&stream, &type, &payload));
    assert(type == MSG_PONG);
    assert(stream.empty());
}

static void testOversizedHeader() {
    const uint16_t tooLarge =
        static_cast<uint16_t>(MAX_TCP_MSG_PAYLOAD + 1);
    const uint8_t header[] = {
        MSG_ERROR,
        static_cast<uint8_t>((tooLarge >> 8) & 0xff),
        static_cast<uint8_t>(tooLarge & 0xff)
    };
    const TcpFrameProbeResult probe =
        probeTcpFrame(header, sizeof(header));
    assert(probe.status == TcpFrameProbeStatus::Malformed);
}

static void testSignalRelayFallbackPayload() {
    ByteBuffer body;
    body.writeU32(9);
    body.writeU32(10);
    body.writeU8(TRAFFIC_UDP);
    const uint8_t packet[] = { 0x45, 0x00 };
    body.writeBytes(packet, sizeof(packet));

    assert(validateServerSignalPayload(
        MSG_TCP_RELAY_DATA, body.data(), body.size()).isValid());
    ByteBuffer parsed(body.data(), body.size());
    assert(parsed.readU32() == 9);
    assert(parsed.readU32() == 10);
    assert(parsed.readU8() == TRAFFIC_UDP);
    assert(parsed.remaining() == sizeof(packet));
}

static void testProductionTcpSendLimit() {
    TcpConnection connection;
    std::vector<uint8_t> payload(MAX_TCP_MSG_PAYLOAD, 0xaa);
    ByteBuffer atLimit;
    atLimit.writeBytes(payload.data(), payload.size());
    assert(connection.sendTcpMsg(MSG_ERROR, atLimit));
    const size_t queuedAtLimit = connection.sendBuf.size();
    assert(queuedAtLimit == sizeof(TcpMsgHeader) + MAX_TCP_MSG_PAYLOAD);

    payload.push_back(0xbb);
    ByteBuffer oversized;
    oversized.writeBytes(payload.data(), payload.size());
    assert(!connection.sendTcpMsg(MSG_ERROR, oversized));
    assert(connection.sendBuf.size() == queuedAtLimit);
}

int main() {
    testVersionEightOnly();
    testBatchExtractionAndUnknownMessage();
    testOversizedHeader();
    testSignalRelayFallbackPayload();
    testProductionTcpSendLimit();
    return 0;
}

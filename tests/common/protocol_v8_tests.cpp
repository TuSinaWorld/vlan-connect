#include "../../common/byte_buffer.h"
#include "../../common/secure_frame.h"
#include "../../common/signal_message_validator.h"
#include <cstring>
#include <cstdio>
#include <utility>
#include <vector>

using namespace VLan;

namespace {

int g_failures = 0;

const uint8_t* bytes(const ByteBuffer& buffer) {
    return buffer.size() == 0 ? nullptr : buffer.data();
}

void expectStatus(const char* name, const MessageValidationResult& result,
                  MessageValidationStatus expected) {
    if (result.status == expected) return;
    std::fprintf(stderr,
                 "%s: status=%d expected=%d error=%s offset=%zu\n",
                 name, static_cast<int>(result.status),
                 static_cast<int>(expected),
                 messageValidationErrorName(result.error), result.offset);
    ++g_failures;
}

void expectSignalValid(const char* name, uint8_t type,
                       const ByteBuffer& body) {
    expectStatus(name,
                 validateServerSignalPayload(type, bytes(body), body.size()),
                 MessageValidationStatus::Valid);
}

void expectSignalMalformed(const char* name, uint8_t type,
                           const uint8_t* data, size_t len) {
    expectStatus(name, validateServerSignalPayload(type, data, len),
                 MessageValidationStatus::Malformed);
}

void writeDefaultPolicy(ByteBuffer* body, bool tcpPolicy) {
    const RoomTrafficPolicy policy =
        tcpPolicy ? makeDefaultTcpPolicy() : makeDefaultUdpPolicy();
    body->writeU8(static_cast<uint8_t>(policy.transportMode));
    body->writeU8(static_cast<uint8_t>(policy.fecMode));
    body->writeU8(static_cast<uint8_t>(policy.kcpProfile));
}

void testServerHello() {
    ByteBuffer hello;
    hello.writeU16(PROTOCOL_VERSION);
    hello.writeU8(1);
    uint8_t handshake[48] = {};
    hello.writeBytes(handshake, sizeof(handshake));
    expectSignalValid("server hello", MSG_SERVER_HELLO, hello);

    for (size_t len = 0; len < hello.size(); ++len) {
        expectSignalMalformed("truncated server hello", MSG_SERVER_HELLO,
                              bytes(hello), len);
    }

    ByteBuffer oldVersion;
    oldVersion.writeU16(PROTOCOL_VERSION - 1);
    oldVersion.writeU8(1);
    oldVersion.writeBytes(handshake, sizeof(handshake));
    MessageValidationResult oldResult = validateServerSignalPayload(
        MSG_SERVER_HELLO, bytes(oldVersion), oldVersion.size());
    expectStatus("old protocol version", oldResult,
                 MessageValidationStatus::Malformed);
    if (oldResult.error != MessageValidationError::InvalidVersion)
        ++g_failures;

    const uint8_t invalidBoolean[] = {
        static_cast<uint8_t>((PROTOCOL_VERSION >> 8) & 0xff),
        static_cast<uint8_t>(PROTOCOL_VERSION & 0xff),
        2
    };
    expectSignalMalformed("invalid hello boolean", MSG_SERVER_HELLO,
                          invalidBoolean, sizeof(invalidBoolean));

    ByteBuffer authenticated;
    authenticated.writeU16(PROTOCOL_VERSION);
    authenticated.writeU8(1);
    authenticated.writeBytes(handshake, sizeof(handshake));
    expectSignalValid("authenticated server hello",
                      MSG_SERVER_HELLO, authenticated);
}

void testFixedMessages() {
    ByteBuffer login;
    login.writeU32(7);
    login.writeU16(PROTOCOL_VERSION);
    login.writeU8(1);
    expectSignalValid("login response", MSG_LOGIN_RESP, login);
    login.writeU8(0);
    expectSignalMalformed("login trailing data", MSG_LOGIN_RESP,
                          bytes(login), login.size());

    ByteBuffer auth;
    auth.writeU32(0);
    uint8_t proof[32] = {};
    auth.writeBytes(proof, sizeof(proof));
    expectSignalMalformed("zero secure session", MSG_SERVER_AUTH_OK,
                          bytes(auth), auth.size());

    ByteBuffer empty;
    expectSignalValid("logout ack", MSG_LOGOUT_ACK, empty);
    expectSignalValid("pong", MSG_PONG, empty);
}

void testRoomMessages() {
    ByteBuffer created;
    created.writeU32(100);
    created.writeU32(VNET_SUBNET | 2);
    writeDefaultPolicy(&created, true);
    writeDefaultPolicy(&created, false);
    created.writeU8(0);
    created.writeU16(ROOM_MTU_DEFAULT);
    uint8_t token[RECONNECT_TOKEN_SIZE] = {};
    token[0] = 1;
    created.writeBytes(token, sizeof(token));
    expectSignalValid("room created", MSG_ROOM_CREATED, created);

    ByteBuffer joined;
    joined.writeU32(100);
    joined.writeU32(VNET_SUBNET | 2);
    writeDefaultPolicy(&joined, true);
    writeDefaultPolicy(&joined, false);
    joined.writeU8(0);
    joined.writeU16(ROOM_MTU_DEFAULT);
    joined.writeU8(1);
    joined.writeU32(7);
    joined.writeU32(VNET_SUBNET | 2);
    joined.writeString("Alice");
    joined.writeBytes(token, sizeof(token));
    expectSignalValid("join response", MSG_JOIN_RESP, joined);

    std::vector<uint8_t> truncated(joined.data(),
                                   joined.data() + joined.size() - 1);
    expectSignalMalformed("truncated join response", MSG_JOIN_RESP,
                          truncated.data(), truncated.size());

    ByteBuffer roomList;
    roomList.writeU64(9);
    roomList.writeU16(0);
    roomList.writeU16(1);
    roomList.writeU16(1);
    roomList.writeU32(100);
    roomList.writeString("Room");
    roomList.writeU8(1);
    roomList.writeU8(MAX_PLAYERS);
    writeDefaultPolicy(&roomList, true);
    writeDefaultPolicy(&roomList, false);
    roomList.writeU8(0);
    roomList.writeU16(ROOM_MTU_DEFAULT);
    expectSignalValid("room list", MSG_ROOM_LIST, roomList);

    ByteBuffer duplicate;
    duplicate.writeU64(9);
    duplicate.writeU16(0);
    duplicate.writeU16(1);
    duplicate.writeU16(2);
    duplicate.writeBytes(roomList.data() + 14, roomList.size() - 14);
    duplicate.writeBytes(roomList.data() + 14, roomList.size() - 14);
    expectSignalMalformed("duplicate room list id", MSG_ROOM_LIST,
                          bytes(duplicate), duplicate.size());

    ByteBuffer delta;
    delta.writeU64(9);
    delta.writeU64(10);
    delta.writeU16(2);
    delta.writeU8(ROOM_LIST_UPSERT);
    delta.writeBytes(roomList.data() + 14, roomList.size() - 14);
    delta.writeU8(ROOM_LIST_REMOVE);
    delta.writeU32(101);
    expectSignalValid("room list delta", MSG_ROOM_LIST_PUSH, delta);

    ByteBuffer emptyDelta;
    emptyDelta.writeU64(10);
    emptyDelta.writeU64(11);
    emptyDelta.writeU16(0);
    expectSignalMalformed("empty room list delta", MSG_ROOM_LIST_PUSH,
                          bytes(emptyDelta), emptyDelta.size());
}

void testDataMessages() {
    ByteBuffer empty;
    expectStatus("data ack",
                 validateServerDataPayload(MSG_DATA_CHANNEL_ACK,
                                           nullptr, 0),
                 MessageValidationStatus::Valid);

    ByteBuffer relay;
    relay.writeU32(7);
    relay.writeU32(8);
    relay.writeU8(TRAFFIC_TCP);
    expectStatus("relay data",
                 validateServerDataPayload(MSG_TCP_RELAY_DATA,
                                           bytes(relay), relay.size()),
                 MessageValidationStatus::Valid);

    ByteBuffer badClass;
    badClass.writeU32(7);
    badClass.writeU32(8);
    badClass.writeU8(9);
    MessageValidationResult badResult = validateServerDataPayload(
        MSG_TCP_RELAY_DATA, bytes(badClass), badClass.size());
    expectStatus("invalid relay traffic class", badResult,
                 MessageValidationStatus::Malformed);
    if (badResult.error != MessageValidationError::InvalidTrafficClass)
        ++g_failures;

    expectStatus("unknown data message",
                 validateServerDataPayload(0x7f, nullptr, 0),
                 MessageValidationStatus::UnknownType);
}

void testByteBufferReadError() {
    ByteBuffer wide;
    wide.writeU64(UINT64_C(0x0102030405060708));
    if (wide.readU64() != UINT64_C(0x0102030405060708) || !wide.atEnd()) {
        std::fprintf(stderr, "ByteBuffer u64 round trip failed\n");
        ++g_failures;
    }

    ByteBuffer empty(static_cast<const uint8_t*>(nullptr), 0);
    if (empty.size() != 0) {
        std::fprintf(stderr, "Empty null ByteBuffer was not empty\n");
        ++g_failures;
    }

    bool nullCaught = false;
    try {
        ByteBuffer invalid(static_cast<const uint8_t*>(nullptr), 1);
    } catch (const ByteBufferReadError&) {
        nullCaught = true;
    }
    if (!nullCaught) {
        std::fprintf(stderr, "Null ByteBuffer input was not rejected\n");
        ++g_failures;
    }

    ByteBuffer body;
    body.writeU8(1);
    bool caught = false;
    try {
        body.readU16();
    } catch (const ByteBufferReadError&) {
        caught = true;
    }
    if (!caught) {
        std::fprintf(stderr, "ByteBufferReadError was not raised\n");
        ++g_failures;
    }
}

void* failAllocation(size_t) {
    return nullptr;
}

void ignoreDeallocation(void*) {}

bool failRandom(uint8_t* out, size_t len) {
    if (out && len) out[0] = 0xff;
    return false;
}

void testInjectedCryptoFailures() {
    uint8_t output[CIPHER_KEY_SIZE];
    std::memset(output, 0xff, sizeof(output));
    const uint8_t password[] = { '1','2','3','4','5','6','7','8' };
    if (computeIntermediateWithAllocator(
            password, sizeof(password), output,
            &failAllocation, &ignoreDeallocation)) {
        std::fprintf(stderr, "Injected KDF allocation failure was ignored\n");
        ++g_failures;
    }
    for (size_t i = 0; i < sizeof(output); ++i) {
        if (output[i] != 0) {
            std::fprintf(stderr, "KDF failure did not clear output\n");
            ++g_failures;
            break;
        }
    }

    std::memset(output, 0xff, sizeof(output));
    if (secureRandomBytesWithProvider(
            output, sizeof(output), &failRandom)) {
        std::fprintf(stderr, "Injected RNG failure was ignored\n");
        ++g_failures;
    }
    for (size_t i = 0; i < sizeof(output); ++i) {
        if (output[i] != 0) {
            std::fprintf(stderr, "RNG failure did not clear output\n");
            ++g_failures;
            break;
        }
    }
}

void testRoomListPayloadBoundaries() {
    ByteBuffer page;
    page.writeU64(1);
    page.writeU16(0);
    page.writeU16(1);
    page.writeU16(750);
    for (uint32_t i = 0; i < 750; ++i) {
        page.writeU32(i + 1);
        page.writeString(std::string(i < 749 ? 63 : 49, 'R'));
        page.writeU8(1);
        page.writeU8(MAX_PLAYERS);
        writeDefaultPolicy(&page, true);
        writeDefaultPolicy(&page, false);
        page.writeU8(0);
        page.writeU16(ROOM_MTU_DEFAULT);
    }
    if (page.size() != ROOM_LIST_PAGE_PAYLOAD) {
        std::fprintf(stderr, "Room-list boundary fixture has size %zu\n",
                     page.size());
        ++g_failures;
        return;
    }
    const MessageValidationResult atLimitResult =
        validateServerSignalPayload(MSG_ROOM_LIST,
                                    page.data(), page.size());
    if (atLimitResult.status != MessageValidationStatus::Valid) {
        std::fprintf(stderr, "Valid 60,000-byte room-list page was rejected\n");
        ++g_failures;
    }

    std::vector<uint8_t> oversized(page.data(), page.data() + page.size());
    oversized.push_back(0);
    const MessageValidationResult oversizedResult =
        validateServerSignalPayload(MSG_ROOM_LIST, oversized.data(),
                                    oversized.size());
    if (oversizedResult.status != MessageValidationStatus::Malformed ||
        oversizedResult.offset != ROOM_LIST_PAGE_PAYLOAD) {
        std::fprintf(stderr, "Room-list payload limit was not enforced\n");
        ++g_failures;
    }
}

void expectAllPrefixesAndTailMalformed(
    const char* name, uint8_t type, const ByteBuffer& body)
{
    expectSignalValid(name, type, body);
    for (size_t len = 0; len < body.size(); ++len) {
        expectSignalMalformed(name, type, bytes(body), len);
    }
    std::vector<uint8_t> withTail;
    if (body.size() > 0)
        withTail.assign(body.data(), body.data() + body.size());
    withTail.push_back(0);
    expectSignalMalformed(name, type, withTail.data(), withTail.size());
}

void testAllKnownSignalShapes() {
    std::vector<std::pair<uint8_t, ByteBuffer> > messages;

    ByteBuffer hello;
    hello.writeU16(PROTOCOL_VERSION);
    hello.writeU8(1);
    uint8_t helloHandshake[48] = {};
    hello.writeBytes(helloHandshake, sizeof(helloHandshake));
    messages.push_back(std::make_pair(MSG_SERVER_HELLO, hello));

    ByteBuffer authOk;
    authOk.writeU32(1);
    uint8_t proof[32] = {};
    authOk.writeBytes(proof, sizeof(proof));
    messages.push_back(std::make_pair(MSG_SERVER_AUTH_OK, authOk));

    ByteBuffer login;
    login.writeU32(1);
    login.writeU16(PROTOCOL_VERSION);
    login.writeU8(0);
    messages.push_back(std::make_pair(MSG_LOGIN_RESP, login));

    uint8_t token[RECONNECT_TOKEN_SIZE] = {};
    token[0] = 1;
    ByteBuffer created;
    created.writeU32(1);
    created.writeU32(VNET_SUBNET | 2);
    writeDefaultPolicy(&created, true);
    writeDefaultPolicy(&created, false);
    created.writeU8(0);
    created.writeU16(ROOM_MTU_DEFAULT);
    created.writeBytes(token, sizeof(token));
    messages.push_back(std::make_pair(MSG_ROOM_CREATED, created));

    ByteBuffer joined;
    joined.writeU32(1);
    joined.writeU32(VNET_SUBNET | 2);
    writeDefaultPolicy(&joined, true);
    writeDefaultPolicy(&joined, false);
    joined.writeU8(0);
    joined.writeU16(ROOM_MTU_DEFAULT);
    joined.writeU8(1);
    joined.writeU32(1);
    joined.writeU32(VNET_SUBNET | 2);
    joined.writeString("Alice");
    joined.writeBytes(token, sizeof(token));
    messages.push_back(std::make_pair(MSG_JOIN_RESP, joined));

    ByteBuffer peer;
    peer.writeU32(2);
    peer.writeU32(VNET_SUBNET | 3);
    peer.writeString("Alice");
    messages.push_back(std::make_pair(MSG_PEER_JOINED, peer));
    messages.push_back(std::make_pair(MSG_PEER_RESUMED, peer));

    ByteBuffer peerId;
    peerId.writeU32(2);
    messages.push_back(std::make_pair(MSG_PEER_LEFT, peerId));
    messages.push_back(std::make_pair(MSG_RELAY_READY, peerId));

    ByteBuffer empty;
    messages.push_back(std::make_pair(MSG_LOGOUT_ACK, empty));
    messages.push_back(std::make_pair(MSG_PONG, empty));

    ByteBuffer roomList;
    roomList.writeU64(1);
    roomList.writeU16(0);
    roomList.writeU16(1);
    roomList.writeU16(1);
    roomList.writeU32(1);
    roomList.writeString("Room");
    roomList.writeU8(1);
    roomList.writeU8(MAX_PLAYERS);
    writeDefaultPolicy(&roomList, true);
    writeDefaultPolicy(&roomList, false);
    roomList.writeU8(0);
    roomList.writeU16(ROOM_MTU_DEFAULT);
    messages.push_back(std::make_pair(MSG_ROOM_LIST, roomList));

    ByteBuffer roomPush;
    roomPush.writeU64(1);
    roomPush.writeU64(2);
    roomPush.writeU16(1);
    roomPush.writeU8(ROOM_LIST_UPSERT);
    roomPush.writeBytes(roomList.data() + 14, roomList.size() - 14);
    messages.push_back(std::make_pair(MSG_ROOM_LIST_PUSH, roomPush));

    ByteBuffer challenge;
    challenge.writeBytes(proof, sizeof(proof));
    messages.push_back(std::make_pair(MSG_AUTH_CHALLENGE, challenge));

    ByteBuffer error;
    error.writeString("error");
    messages.push_back(std::make_pair(MSG_ERROR, error));

    for (size_t i = 0; i < messages.size(); ++i) {
        expectAllPrefixesAndTailMalformed(
            "known signal shape",
            messages[i].first, messages[i].second);
    }

    ByteBuffer relay;
    relay.writeU32(1);
    relay.writeU32(2);
    relay.writeU8(TRAFFIC_TCP);
    expectSignalValid("signal relay", MSG_TCP_RELAY_DATA, relay);
    for (size_t len = 0; len < relay.size(); ++len) {
        expectSignalMalformed(
            "signal relay prefix", MSG_TCP_RELAY_DATA,
            bytes(relay), len);
    }

    const size_t encryptedMinimum =
        SECURE_COUNTER_SIZE + 1 + SECURE_MAC_SIZE;
    std::vector<uint8_t> encrypted(encryptedMinimum, 0);
    expectStatus("encrypted envelope",
                 validateServerSignalPayload(
                     MSG_ENCRYPTED, encrypted.data(), encrypted.size()),
                 MessageValidationStatus::Valid);
    for (size_t len = 0; len < encrypted.size(); ++len) {
        expectSignalMalformed(
            "encrypted prefix", MSG_ENCRYPTED,
            len == 0 ? nullptr : encrypted.data(), len);
    }
}

} // namespace

int main() {
    testServerHello();
    testFixedMessages();
    testRoomMessages();
    testDataMessages();
    testByteBufferReadError();
    testInjectedCryptoFailures();
    testRoomListPayloadBoundaries();
    testAllKnownSignalShapes();

    if (g_failures != 0) {
        std::fprintf(stderr, "protocol_v8_tests: %d failure(s)\n",
                     g_failures);
        return 1;
    }
    std::fprintf(stdout, "protocol_v8_tests: ok\n");
    return 0;
}

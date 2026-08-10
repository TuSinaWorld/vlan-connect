#ifndef _WIN32

#include "cli_peer.h"
#include "cli_fec.h"
#include <cassert>
#include <cstring>
#include <vector>

using namespace VLan;

namespace {

Buffer makeFragment(uint16_t messageId, uint8_t index, uint8_t total,
                    uint16_t totalLength, size_t payloadLength,
                    uint8_t fill = 0x5a) {
    FragHeader header;
    header.msgId = htons(messageId);
    header.fragIndex = index;
    header.fragTotal = total;
    header.totalLen = htons(totalLength);
    Buffer packet(sizeof(header) + payloadLength, fill);
    std::memcpy(packet.data(), &header, sizeof(header));
    return packet;
}

Buffer makeFecPacket(uint8_t group, uint8_t index, uint8_t dataCount,
                     uint8_t totalCount, const Buffer& block) {
    FecHeader header;
    header.groupId = group;
    header.index = index;
    header.dataCount = dataCount;
    header.totalCount = totalCount;
    Buffer packet(sizeof(header) + block.size());
    std::memcpy(packet.data(), &header, sizeof(header));
    if (!block.empty())
        std::memcpy(packet.data() + sizeof(header), block.data(), block.size());
    return packet;
}

void testRawRoundTripAndMalformedFragments() {
    std::vector<Buffer> datagrams;
    UdpSendFunc capture = [&datagrams](const uint8_t* data, size_t len,
                                       uint32_t, uint16_t) {
        datagrams.push_back(Buffer(data, data + len));
    };
    CliRawUdpTunnel sender(capture, 0, 0, FEC_NONE,
                           ROOM_MTU_DEFAULT, TRAFFIC_UDP, true);
    CliRawUdpTunnel receiver(UdpSendFunc(), 0, 0, FEC_NONE,
                             ROOM_MTU_DEFAULT, TRAFFIC_UDP, true);
    std::vector<Buffer> received;
    receiver.setOnDataReceived(
        [&received](const Buffer& packet) { received.push_back(packet); });

    Buffer original(ROOM_MTU_DEFAULT, 0x31);
    assert(sender.send(original) == static_cast<int>(original.size()));
    assert(datagrams.size() >= 2);
    for (size_t i = 0; i < datagrams.size(); ++i) {
        assert(!datagrams[i].empty());
        assert(datagrams[i][0] == UDP_RAW_RELAY_DATA);
        receiver.feedInput(
            reinterpret_cast<const char*>(datagrams[i].data() + 1),
            static_cast<int>(datagrams[i].size() - 1));
    }
    assert(received.size() == 1 && received[0] == original);

    const size_t before = received.size();
    const Buffer zeroTotal = makeFragment(100, 0, 0, 1, 1);
    receiver.feedInput(reinterpret_cast<const char*>(zeroTotal.data()),
                       static_cast<int>(zeroTotal.size()));
    const Buffer badIndex = makeFragment(101, 1, 1, 1, 1);
    receiver.feedInput(reinterpret_cast<const char*>(badIndex.data()),
                       static_cast<int>(badIndex.size()));
    const Buffer overMtu = makeFragment(
        102, 0, 1, ROOM_MTU_DEFAULT + 1, ROOM_MTU_DEFAULT + 1);
    receiver.feedInput(reinterpret_cast<const char*>(overMtu.data()),
                       static_cast<int>(overMtu.size()));
    const Buffer wrongCount = makeFragment(103, 0, 2, 1, 1);
    receiver.feedInput(reinterpret_cast<const char*>(wrongCount.data()),
                       static_cast<int>(wrongCount.size()));

    int fragmentPayload = transportPayloadBudget(
        ROOM_MTU_DEFAULT, MODE_RELAY_RAW_UDP, false, true);
    if (fragmentPayload > RAW_UDP_MAX_FRAG_PAYLOAD)
        fragmentPayload = RAW_UDP_MAX_FRAG_PAYLOAD;
    const Buffer metadataA = makeFragment(
        104, 0, 2, static_cast<uint16_t>(fragmentPayload + 1),
        static_cast<size_t>(fragmentPayload));
    const Buffer metadataB = makeFragment(
        104, 0, 2, static_cast<uint16_t>(fragmentPayload + 2),
        static_cast<size_t>(fragmentPayload));
    const Buffer originalTail = makeFragment(
        104, 1, 2, static_cast<uint16_t>(fragmentPayload + 1), 1);
    receiver.feedInput(reinterpret_cast<const char*>(metadataA.data()),
                       static_cast<int>(metadataA.size()));
    receiver.feedInput(reinterpret_cast<const char*>(metadataB.data()),
                       static_cast<int>(metadataB.size()));
    receiver.feedInput(reinterpret_cast<const char*>(originalTail.data()),
                       static_cast<int>(originalTail.size()));
    assert(received.size() == before);
}

void testRawOldestMessageEviction() {
    CliRawUdpTunnel receiver(UdpSendFunc(), 0, 0, FEC_NONE,
                             ROOM_MTU_DEFAULT, TRAFFIC_UDP, true);
    size_t delivered = 0;
    receiver.setOnDataReceived(
        [&delivered](const Buffer&) { ++delivered; });
    int fragmentPayload = transportPayloadBudget(
        ROOM_MTU_DEFAULT, MODE_RELAY_RAW_UDP, false, true);
    if (fragmentPayload > RAW_UDP_MAX_FRAG_PAYLOAD)
        fragmentPayload = RAW_UDP_MAX_FRAG_PAYLOAD;
    assert(fragmentPayload > 0 && fragmentPayload < ROOM_MTU_DEFAULT);
    const uint16_t totalLength =
        static_cast<uint16_t>(fragmentPayload + 1);

    for (uint16_t id = 0; id <= RAW_UDP_MAX_ACTIVE_MESSAGES; ++id) {
        const Buffer first = makeFragment(
            id, 0, 2, totalLength, static_cast<size_t>(fragmentPayload));
        receiver.feedInput(reinterpret_cast<const char*>(first.data()),
                           static_cast<int>(first.size()));
    }

    const Buffer newestTail = makeFragment(
        static_cast<uint16_t>(RAW_UDP_MAX_ACTIVE_MESSAGES),
        1, 2, totalLength, 1);
    receiver.feedInput(reinterpret_cast<const char*>(newestTail.data()),
                       static_cast<int>(newestTail.size()));
    assert(delivered == 1);

    const Buffer evictedTail = makeFragment(0, 1, 2, totalLength, 1);
    receiver.feedInput(reinterpret_cast<const char*>(evictedTail.data()),
                       static_cast<int>(evictedTail.size()));
    assert(delivered == 1);
}

void testFecValidation() {
    size_t delivered = 0;
    CliFecDecoder decoder(
        [&delivered](const Buffer&) { ++delivered; });
    Buffer block(3, 0);
    block[1] = 1;
    block[2] = 0x42;

    const Buffer valid = makeFecPacket(1, 0, 1, 1, block);
    decoder.addPacket(reinterpret_cast<const char*>(valid.data()),
                      static_cast<int>(valid.size()));
    assert(delivered == 1);

    const Buffer invalidPackets[] = {
        makeFecPacket(2, 0, 0, 1, block),
        makeFecPacket(3, 0, 11, 11, block),
        makeFecPacket(4, 0, 2, 1, block),
        makeFecPacket(5, 0, 2, 7, block),
        makeFecPacket(6, 3, 2, 3, block)
    };
    for (size_t i = 0; i < sizeof(invalidPackets) / sizeof(invalidPackets[0]); ++i) {
        decoder.addPacket(
            reinterpret_cast<const char*>(invalidPackets[i].data()),
            static_cast<int>(invalidPackets[i].size()));
    }
    assert(delivered == 1);

    const Buffer metadataA = makeFecPacket(7, 2, 2, 3, block);
    const Buffer metadataB = makeFecPacket(7, 2, 3, 3, block);
    decoder.addPacket(reinterpret_cast<const char*>(metadataA.data()),
                      static_cast<int>(metadataA.size()));
    decoder.addPacket(reinterpret_cast<const char*>(metadataB.data()),
                      static_cast<int>(metadataB.size()));
    assert(delivered == 1);
}

void buildFecGroup(CliFecEncoder* encoder, size_t payloadSize,
                   std::vector<Buffer>* packets) {
    packets->clear();
    Buffer first(payloadSize, 0x11);
    Buffer second(payloadSize, 0x22);
    encoder->addPacket(first);
    encoder->addPacket(second);
    encoder->flush();
    assert(packets->size() == 3);
    assert((*packets)[0][1] == 0);
    assert((*packets)[1][1] == 1);
    assert((*packets)[2][1] == 2);
}

void testFecActiveGroupEviction() {
    std::vector<Buffer> encoded;
    CliFecEncoder encoder(FEC_50,
        [&encoded](const Buffer& packet) { encoded.push_back(packet); });
    size_t delivered = 0;
    CliFecDecoder decoder(
        [&delivered](const Buffer&) { ++delivered; });
    Buffer firstOriginal;
    for (size_t i = 0; i <= FEC_MAX_ACTIVE_GROUPS; ++i) {
        buildFecGroup(&encoder, 1, &encoded);
        if (i == 0) firstOriginal = encoded[0];
        decoder.addPacket(reinterpret_cast<const char*>(encoded[2].data()),
                          static_cast<int>(encoded[2].size()));
    }
    decoder.addPacket(reinterpret_cast<const char*>(firstOriginal.data()),
                      static_cast<int>(firstOriginal.size()));
    assert(delivered == 1);
}

void testFecMemoryEviction() {
    std::vector<Buffer> encoded;
    CliFecEncoder encoder(FEC_50,
        [&encoded](const Buffer& packet) { encoded.push_back(packet); });
    size_t delivered = 0;
    CliFecDecoder decoder(
        [&delivered](const Buffer&) { ++delivered; });
    Buffer firstOriginal;
    const size_t payloadSize = 59998;
    for (size_t i = 0; i < 5; ++i) {
        buildFecGroup(&encoder, payloadSize, &encoded);
        assert(encoded[2].size() == sizeof(FecHeader) + 60000);
        if (i == 0) firstOriginal = encoded[0];
        decoder.addPacket(reinterpret_cast<const char*>(encoded[2].data()),
                          static_cast<int>(encoded[2].size()));
    }
    decoder.addPacket(reinterpret_cast<const char*>(firstOriginal.data()),
                      static_cast<int>(firstOriginal.size()));
    assert(delivered == 1);
}

} // namespace

int main() {
    testRawRoundTripAndMalformedFragments();
    testRawOldestMessageEviction();
    testFecValidation();
    testFecActiveGroupEviction();
    testFecMemoryEviction();
    return 0;
}

#endif

#include "cli_common.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace VLan;

int main() {
    TcpConnection connection;
    const uint8_t marker = 0x5a;
    assert(connection.queueSend(nullptr, 0));
    assert(!connection.queueSend(nullptr, 1));
    assert(!connection.queueSend(&marker, static_cast<size_t>(-1)));
    assert(connection.sendBuf.empty());

    // Fill the bounded queue exactly, then reject without modifying old bytes.
    std::vector<uint8_t> full(MAX_TCP_SEND_BUF, marker);
    assert(connection.queueSend(full.data(), full.size()));
    assert(connection.sendBuf == full);
    assert(!connection.queueSend(&marker, 1));
    assert(!connection.sendTcpMsg(MSG_PING));
    assert(connection.sendBuf == full);

    ByteBuffer payload;
    const uint8_t data[] = {0x10, 0x20, 0x30, 0x40};
    payload.writeBytes(data, sizeof(data));
    // Space for a header alone is insufficient: never append a partial frame.
    connection.sendBuf.resize(MAX_TCP_SEND_BUF - sizeof(TcpMsgHeader));
    const Buffer before = connection.sendBuf;
    assert(!connection.sendTcpMsg(MSG_PING, payload));
    assert(connection.sendBuf == before);
    assert(connection.sendTcpMsg(MSG_PING));
    assert(connection.sendBuf.size() == MAX_TCP_SEND_BUF);

    // A complete frame fits at the exact boundary and preserves wire format.
    connection.sendBuf.resize(MAX_TCP_SEND_BUF - sizeof(TcpMsgHeader) - sizeof(data));
    const size_t offset = connection.sendBuf.size();
    assert(connection.sendTcpMsg(MSG_TCP_RELAY_DATA, payload));
    assert(connection.sendBuf.size() == MAX_TCP_SEND_BUF);
    assert(connection.sendBuf[offset] == MSG_TCP_RELAY_DATA);
    assert(connection.sendBuf[offset + 1] == 0);
    assert(connection.sendBuf[offset + 2] == sizeof(data));
    assert(std::memcmp(connection.sendBuf.data() + offset + sizeof(TcpMsgHeader),
                       data, sizeof(data)) == 0);

    // Oversized messages are rejected even when the queue is empty.
    connection.reset();
    assert(connection.sendBuf.empty());
    ByteBuffer oversized;
    std::vector<uint8_t> large(MAX_TCP_MSG_PAYLOAD + 1, marker);
    oversized.writeBytes(large.data(), large.size());
    assert(!connection.sendTcpMsg(MSG_TCP_RELAY_DATA, oversized));
    assert(connection.sendBuf.empty());
    assert(connection.sendTcpMsg(MSG_PING, payload));
    assert(connection.sendBuf.size() == sizeof(TcpMsgHeader) + sizeof(data));
    std::puts("tcp_backpressure_tests: ok");
    return 0;
}

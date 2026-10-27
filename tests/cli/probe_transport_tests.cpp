// User-mode production transport regression. Only loopback UDP sockets are
// opened; loss/delay are injected here. No TUN, routes, firewall or elevation.
#include "cli_peer.h"
#include "cli_log.h"
#include <cassert>
#include <chrono>
#include <memory>
#include <thread>

bool VLan::g_verboseLog = false;
using namespace VLan;

namespace {
socket_t udpSocket() {
    socket_t fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    assert(fd != SOCK_INVALID);
    sockaddr_in address = makeAddr(0x7f000001u, 0);
    assert(bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    assert(setNonBlocking(fd));
    return fd;
}
uint16_t port(socket_t fd) {
    sockaddr_in address = {}; socklen_t size = sizeof(address);
    assert(getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size) == 0);
    return ntohs(address.sin_port);
}
struct Delayed {
    uint32_t due;
    TrafficClass cls;
    Buffer bytes;
};

class UdpPair {
public:
    CliPeerConnection a, b;
    bool blocked;
    unsigned dropped;
    UdpPair() : a(2, VNET_SUBNET | 3, "B"), b(1, VNET_SUBNET | 2, "A"),
                blocked(false), dropped(0) {
        sockets[0] = udpSocket(); sockets[1] = udpSocket();
        for (int side = 0; side < 2; ++side) {
            UdpSendFunc output = [this, side](const uint8_t* data, size_t length, uint32_t, uint16_t) {
                if (blocked) { ++dropped; return; }
                Buffer tagged(1, data[0] == UDP_RAW_RELAY_DATA ? TRAFFIC_TCP : TRAFFIC_UDP);
                tagged.insert(tagged.end(), data, data + length);
                sockaddr_in target = makeAddr(0x7f000001u, port(sockets[1 - side]));
                assert(sendto(sockets[side], reinterpret_cast<const char*>(tagged.data()),
                              static_cast<int>(tagged.size()), 0,
                              reinterpret_cast<sockaddr*>(&target), sizeof(target)) == static_cast<int>(tagged.size()));
            };
            raw[side].reset(new CliRawUdpTunnel(output, 0, 0, FEC_NONE, ROOM_MTU_DEFAULT, TRAFFIC_TCP));
            kcp[side].reset(new CliKcpTunnel(0x12345, output, 0, 0, FEC_NONE,
                ROOM_MTU_DEFAULT, KCP_PROFILE_REALTIME, TRAFFIC_UDP));
            peer(side).setRawUdpTunnel(TRAFFIC_TCP, raw[side].get());
            peer(side).setKcpTunnel(TRAFFIC_UDP, kcp[side].get());
            peer(side).setTransport(TRAFFIC_TCP, TRANSPORT_RELAY_RAW_UDP);
            peer(side).setTransport(TRAFFIC_UDP, TRANSPORT_RELAY_KCP);
            peer(side).setOnDataReceived([](uint32_t, const Buffer&) { assert(!"probe leaked into IP data"); });
        }
    }
    ~UdpPair() { sock_close(sockets[0]); sock_close(sockets[1]); }
    CliPeerConnection& peer(int side) { return side ? b : a; }
    void send() { a.sendLatencyPing(); b.sendLatencyPing(); }
    void pump() {
        for (int side = 0; side < 2; ++side) {
            uint8_t bytes[65536];
            for (;;) {
                const int count = recv(sockets[side], reinterpret_cast<char*>(bytes), sizeof(bytes), 0);
                if (count <= 0) break;
                assert(count >= 2);
                // Distinct deterministic receive delays model unlike paths.
                Delayed packet = {currentTimeMs() + (side ? 17u : 43u),
                    static_cast<TrafficClass>(bytes[0]), Buffer(bytes + 2, bytes + count)};
                queues[side].push_back(packet);
            }
            for (size_t i = 0; i < queues[side].size();) {
                Delayed& packet = queues[side][i];
                if (static_cast<int32_t>(currentTimeMs() - packet.due) < 0) { ++i; continue; }
                if (packet.cls == TRAFFIC_TCP)
                    raw[side]->feedInput(reinterpret_cast<const char*>(packet.bytes.data()), static_cast<int>(packet.bytes.size()));
                else
                    kcp[side]->feedInput(reinterpret_cast<const char*>(packet.bytes.data()), static_cast<int>(packet.bytes.size()));
                queues[side].erase(queues[side].begin() + i);
            }
            raw[side]->update(); kcp[side]->update();
        }
    }
private:
    socket_t sockets[2];
    std::unique_ptr<CliRawUdpTunnel> raw[2];
    std::unique_ptr<CliKcpTunnel> kcp[2];
    std::vector<Delayed> queues[2];
};

void legacyEchoAndCancellation() {
    CliPeerConnection p(2, VNET_SUBNET | 3, "legacy-echo");
    p.setTransport(TRAFFIC_TCP, TRANSPORT_RELAY_TCP);
    Buffer old;
    p.setTcpRelaySender([&](uint32_t, TrafficClass cls, const Buffer& ping) {
        assert(ping.size() == 6 && ping[0] == 0xfe && ping[1] == 1);
        Buffer pong = ping; pong[1] = 2; old = pong;
        // Old peers echo the four bytes verbatim, even when they are IDs.
        assert(p.handleLatencyProbe(cls, pong));
    });
    for (int i = 0; i < 6; ++i) p.sendLatencyPing(TRAFFIC_TCP);
    ProbeQualitySnapshot q = p.qualitySnapshot(TRAFFIC_TCP);
    assert(q.replies == 6 && !q.sampling && q.timeoutPercent == 0 && q.rttMs >= 0);
    p.handleLatencyProbe(TRAFFIC_TCP, old);
    assert(p.qualitySnapshot(TRAFFIC_TCP).replies == 6);
    p.clearTransport(TRAFFIC_TCP); p.setTransport(TRAFFIC_TCP, TRANSPORT_RELAY_TCP);
    assert(p.qualitySnapshot(TRAFFIC_TCP).replies == 6);
    p.resetQuality(); p.handleLatencyProbe(TRAFFIC_TCP, old);
    assert(p.qualitySnapshot(TRAFFIC_TCP).replies == 0);
    p.setTcpRelaySender(TcpRelaySendFunc()); p.sendLatencyPing(TRAFFIC_TCP);
    q = p.qualitySnapshot(TRAFFIC_TCP);
    assert(q.localSendFailure && q.sendFailures == 1 && q.timeouts == 0 && !q.outage);
    std::puts("legacy echo, synchronous response, deduplication, rebuild, reset and local failure: PASS");
}

void realUdpBlockAndRecovery() {
    UdpPair udp;
    CliPeerConnection tcpA(2, VNET_SUBNET | 3, "TCP B"), tcpB(1, VNET_SUBNET | 2, "TCP A");
    tcpA.setTcpRelaySender([&](uint32_t, TrafficClass cls, const Buffer& data) { tcpB.handleLatencyProbe(cls, data); });
    tcpB.setTcpRelaySender([&](uint32_t, TrafficClass cls, const Buffer& data) { tcpA.handleLatencyProbe(cls, data); });
    for (TrafficClass cls : {TRAFFIC_TCP, TRAFFIC_UDP}) {
        tcpA.setTransport(cls, TRANSPORT_RELAY_TCP); tcpB.setTransport(cls, TRANSPORT_RELAY_TCP);
    }
    const uint32_t start = currentTimeMs(); uint32_t sentAt = start - 1000;
    bool cleanChecked = false, blockedChecked = false, recovered = false;
    while (currentTimeMs() - start < 30000u) {
        const uint32_t now = currentTimeMs(); const uint32_t elapsed = now - start;
        if (!cleanChecked && elapsed >= 5600) {
            for (TrafficClass cls : {TRAFFIC_TCP, TRAFFIC_UDP}) {
                ProbeQualitySnapshot q = udp.a.qualitySnapshot(cls);
                assert(q.replies >= 5 && !q.sampling && q.timeouts == 0 && !q.outage);
            }
            udp.blocked = true; cleanChecked = true;
            std::puts("Raw UDP and KCP delayed probes over real loopback sockets: PASS");
        }
        if (now - sentAt >= 1000u) {
            sentAt = now; udp.send(); tcpA.sendLatencyPing(); tcpB.sendLatencyPing();
        }
        udp.pump();
        if (!blockedChecked && elapsed >= 16900u) {
            for (TrafficClass cls : {TRAFFIC_TCP, TRAFFIC_UDP}) {
                ProbeQualitySnapshot q = udp.a.qualitySnapshot(cls);
                assert(q.outage && q.timeouts >= 1 && q.timeoutPercent > 0 && !q.localSendFailure);
                ProbeQualitySnapshot tcp = tcpA.qualitySnapshot(cls);
                assert(!tcp.outage && tcp.timeouts == 0 && tcp.replies >= 16);
            }
            assert(udp.dropped > 0);
            blockedChecked = true; udp.blocked = false;
            std::puts("100% UDP blackhole detected for both classes; TCP Relay stays healthy: PASS");
        }
        if (blockedChecked && !udp.a.qualitySnapshot(TRAFFIC_TCP).outage &&
            !udp.a.qualitySnapshot(TRAFFIC_UDP).outage) { recovered = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    assert(cleanChecked && blockedChecked && recovered);
    std::puts("Raw UDP and KCP resume timely replies and clear outage: PASS");
}
} // namespace

int main() {
    sock_init(); CliLog::instance().setLogFile("");
    legacyEchoAndCancellation(); realUdpBlockAndRecovery();
    sock_cleanup(); return 0;
}

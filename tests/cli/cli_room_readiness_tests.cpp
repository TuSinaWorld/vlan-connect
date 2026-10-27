#include "../../client-cli/src/cli_app.h"
#include <cassert>
#include <cstdio>
#include <functional>

namespace VLan {

class FakeCliApp : public CliApp {
public:
    WindowsReadinessState state = WindowsReadinessState::Pending;
    bool prepareOk = true;
    bool startOk = true;
    int prepares = 0;
    int polls = 0;
    int starts = 0;
    int cancels = 0;
    std::function<void()> duringPrepare;
    std::function<void()> duringPoll;
    std::function<void()> duringStart;
protected:
    TunSetupResult setupTun() override {
        ++prepares;
        if (duringPrepare) duringPrepare();
        return TunSetupResult(prepareOk, TunSetupStage::Session,
                              prepareOk ? "" : "fake session failure");
    }
    TunSetupResult startTunDataPlane() override {
        ++starts;
        if (duringStart) duringStart();
        return TunSetupResult(startOk, TunSetupStage::DataPlane,
                              startOk ? "" : "fake data-plane failure");
    }
    WindowsReadinessResult readTunReadiness() override {
        ++polls;
        if (duringPoll) duringPoll();
        WindowsReadinessResult result(state);
        if (state == WindowsReadinessState::Failed)
            result.error = "fake readiness timeout: DadState=Tentative";
        return result;
    }
    void cancelTunReadiness() override { ++cancels; }
};

class CliRoomReadinessTests {
    static PeerInfo peer(uint32_t id, const char* name) {
        PeerInfo result;
        result.peerId = id;
        result.virtualIP = VNET_SUBNET | id;
        result.name = name;
        result.transport = TRANSPORT_NONE;
        return result;
    }
    static RoomTrafficPolicy tcp() {
        RoomTrafficPolicy result = makeDefaultTcpPolicy();
        result.transportMode = MODE_RELAY_TCP;
        return result;
    }
    static void begin(FakeCliApp& app, bool created,
                      const std::vector<PeerInfo>& members = std::vector<PeerInfo>()) {
        if (created)
            app.onRoomCreated(10, VNET_SUBNET | 1, tcp(), tcp(), ROOM_MTU_DEFAULT, false, Buffer());
        else
            app.onJoinResponse(10, VNET_SUBNET | 1, tcp(), tcp(), ROOM_MTU_DEFAULT, false, members, Buffer());
    }
    static void delayedReadyUsesCurrentMembersAndRelays() {
        FakeCliApp app;
        std::vector<PeerInfo> members;
        members.push_back(peer(2, "old"));
        members.push_back(peer(3, "left"));
        members.push_back(peer(6, "stable"));
        begin(app, false, members);
        app.onRelayReady(2);
        app.onPeerResumed(peer(2, "resumed"));
        assert(!app.m_pendingRoomRelays.count(2));
        app.onPeerLeft(3);
        app.onPeerJoined(peer(4, "new"));
        app.onRelayReady(4);
        app.onRelayReady(6);
        app.onPeerJoined(peer(5, "brief"));
        app.onRelayReady(5);
        app.onPeerLeft(5);
        app.pollTunReadiness();
        app.pollTunReadiness();
        assert(app.starts == 0 && !app.m_roomReady && app.m_tunSetupPending);
        assert(app.m_tunnel.allPeers().empty());
        app.state = WindowsReadinessState::Ready;
        app.pollTunReadiness();
        assert(app.starts == 1 && app.m_roomReady && !app.m_tunSetupPending);
        assert(app.m_tunnel.allPeers().size() == 3);
        assert(app.m_tunnel.peerById(2)->name() == "resumed");
        assert(app.m_tunnel.peerById(2)->transport(TRAFFIC_TCP) == TRANSPORT_NONE);
        assert(app.m_tunnel.peerById(4)->transport(TRAFFIC_TCP) == TRANSPORT_RELAY_TCP);
        assert(app.m_tunnel.peerById(6)->transport(TRAFFIC_UDP) == TRANSPORT_RELAY_TCP);
        assert(!app.m_tunnel.peerById(3) && !app.m_tunnel.peerById(5));
        app.pollTunReadiness();
        assert(app.starts == 1);
        app.doLeaveRoom();
        assert(!app.m_roomReady && app.m_tunnel.allPeers().empty());
    }
    static void createdOnlyAfterReadiness() {
        FakeCliApp app;
        begin(app, true);
        assert(app.m_tunSetupIsCreate && app.m_tunSetupPending && !app.m_roomReady);
        app.pollTunReadiness();
        assert(app.starts == 0 && !app.m_roomReady);
        app.state = WindowsReadinessState::Ready;
        app.pollTunReadiness();
        assert(app.starts == 1 && app.m_roomReady && !app.m_tunSetupPending);
        app.doLeaveRoom();
    }
    static void terminalFailuresRollbackOnce() {
        for (int stage = 0; stage < 3; ++stage) {
            FakeCliApp app;
            app.prepareOk = stage != 0;
            app.startOk = stage != 2;
            app.state = stage == 1 ? WindowsReadinessState::Failed : WindowsReadinessState::Ready;
            begin(app, true);
            app.pollTunReadiness();
            app.pollTunReadiness();
            assert(app.cancels == 1);
            assert(!app.m_tunSetupPending && !app.m_roomReady);
            assert(app.m_currentRoomId == 0 && app.m_myVirtualIP == 0);
            assert(app.m_pendingRoomMembers.empty() && app.m_pendingRoomRelays.empty());
            assert(app.starts == (stage == 2 ? 1 : 0));
            assert(app.m_tunnel.allPeers().empty());
        }
    }
    static void cancelLeaveDisconnectAndNewSetup() {
        for (int action = 0; action < 3; ++action) {
            FakeCliApp app;
            begin(app, true);
            app.onPeerJoined(peer(2, "cancelled"));
            app.onRelayReady(2);
            const uint64_t before = app.m_tunSetupGeneration;
            if (action == 0) app.doLeaveRoom();
            else if (action == 1) app.beginGracefulDisconnect(false);
            else app.onSignalDisconnected();
            assert(app.m_tunSetupGeneration != before);
            app.state = WindowsReadinessState::Ready;
            app.pollTunReadiness();
            assert(app.polls == 0 && app.starts == 0);
            assert(!app.m_roomReady && !app.m_tunSetupPending);
            assert(app.m_pendingRoomMembers.empty() && app.m_pendingRoomRelays.empty());
        }
    }
    static void oldPollResultCannotFinishNewRoom() {
        FakeCliApp app;
        begin(app, true);
        app.state = WindowsReadinessState::Ready;
        app.duringPoll = [&app]() {
            app.doLeaveRoom();
            begin(app, true);
        };
        app.pollTunReadiness();
        assert(app.polls == 1 && app.starts == 0 && app.m_tunSetupPending && !app.m_roomReady);
        app.duringPoll = std::function<void()>();
        app.pollTunReadiness();
        assert(app.polls == 2 && app.starts == 1 && app.m_roomReady);
        app.doLeaveRoom();
    }
    static void cancellationDuringPrepareOrStartWins() {
        for (int stage = 0; stage < 2; ++stage) {
            FakeCliApp app;
            app.state = WindowsReadinessState::Ready;
            if (stage == 0) app.duringPrepare = [&app]() { app.doLeaveRoom(); };
            else app.duringStart = [&app]() { app.doLeaveRoom(); };
            begin(app, true);
            app.pollTunReadiness();
            assert(!app.m_tunSetupPending && !app.m_roomReady && app.m_currentRoomId == 0);
            assert(app.cancels == 1 && app.starts == (stage == 1 ? 1 : 0));
        }
    }
    static void pendingRuntimeFailureIsTerminalOnce() {
        FakeCliApp app;
        begin(app, true);
        app.handleTunRuntimeFailure("fake pending TUN read failure");
        app.handleTunRuntimeFailure("duplicate failure");
        app.state = WindowsReadinessState::Ready;
        app.pollTunReadiness();
        assert(app.cancels == 1 && app.starts == 0 && app.polls == 0);
        assert(!app.m_roomReady && !app.m_tunSetupPending);
    }
    static void qualitySoloMembershipAndResume() {
        FakeCliApp app;
        app.state = WindowsReadinessState::Ready;
        begin(app, true);
        app.pollTunReadiness();
        for (int i = 0; i < 20; ++i) app.updatePeerQuality();
        assert(app.m_qualityLogStates.empty());
        app.printStatus();
        app.printPeers();
        app.onPeerJoined(peer(2, "quality peer"));
        CliPeerConnection* remote = app.m_tunnel.peerById(2);
        assert(remote);
        app.updatePeerQuality();
        assert(!remote->qualitySnapshot(TRAFFIC_TCP).active);
        assert(!remote->qualitySnapshot(TRAFFIC_TCP).outage);
        remote->setTcpRelaySender([remote](uint32_t, TrafficClass cls, const Buffer& ping) {
            Buffer pong = ping;
            assert(pong.size() == 6);
            pong[1] = CLI_LATENCY_PROBE_PONG;
            assert(remote->handleLatencyProbe(cls, pong));
        });
        remote->setTransport(TRAFFIC_TCP, TRANSPORT_RELAY_TCP);
        remote->setTransport(TRAFFIC_UDP, TRANSPORT_RELAY_TCP);
        for (int i = 0; i < 6; ++i) app.updatePeerQuality();
        assert(remote->qualitySnapshot(TRAFFIC_TCP).replies == 6);
        assert(!remote->qualitySnapshot(TRAFFIC_TCP).sampling);
        assert(!remote->qualitySnapshot(TRAFFIC_UDP).outage);
        remote->setTcpRelayControlReady([]() { return false; });
        app.updatePeerQuality();
        app.updatePeerQuality();
        assert(remote->qualitySnapshot(TRAFFIC_TCP).sendFailures == 2);
        assert(remote->qualitySnapshot(TRAFFIC_TCP).timeouts == 0);
        assert(!remote->qualitySnapshot(TRAFFIC_TCP).outage);
        assert(app.m_qualityLogStates[app.transportKey(2, TRAFFIC_TCP)].anomaly == 2);
        remote->setTcpRelayControlReady([]() { return true; });
        app.updatePeerQuality();
        assert(app.m_qualityLogStates[app.transportKey(2, TRAFFIC_TCP)].anomaly == 0);
        app.printStatus();
        app.printPeers();
        app.onPeerResumed(peer(2, "quality peer"));
        assert(remote->qualitySnapshot(TRAFFIC_TCP).replies == 0);
        assert(remote->qualitySnapshot(TRAFFIC_UDP).timeouts == 0);
        assert(app.m_qualityLogStates.empty());
        app.onPeerLeft(2);
        for (int i = 0; i < 20; ++i) app.updatePeerQuality();
        assert(app.m_qualityLogStates.empty());
        app.onPeerJoined(peer(2, "new session"));
        assert(app.m_tunnel.peerById(2)->qualitySnapshot(TRAFFIC_TCP).replies == 0);
        app.doLeaveRoom();
        assert(app.m_qualityLogStates.empty());
    }
    static void udpSocketFailuresAreLocal() {
        CliTunnelManager manager;
        manager.setMyPeerId(1);
        assert(manager.initUdpSocket());
        const int noBroadcast = 0;
        assert(setsockopt(manager.udpFd(), SOL_SOCKET, SO_BROADCAST,
                          reinterpret_cast<const char*>(&noBroadcast), sizeof(noBroadcast)) == 0);
        assert(manager.installSecureSession(1, Buffer(SECURE_KEY_SIZE, 'q')));
        manager.m_dataPlaneState = DataPlaneState::Running; // Packet edge only, no TUN.
        CliPeerConnection* remote = manager.addPeer(2, VNET_SUBNET | 2, "socket errors");
        CliPeerConnection* other = manager.addPeer(3, VNET_SUBNET | 3, "unaffected");
        CliRawUdpTunnel* raw = manager.createRawUdpTunnel(remote, 0xffffffffu, 54321,
            FEC_NONE, ROOM_MTU_DEFAULT, TRAFFIC_TCP);
        raw->setRelayMode(1, 2);
        remote->sendLatencyPing(TRAFFIC_TCP); // SO_BROADCAST disabled: local access denied.
        ProbeQualitySnapshot quality = remote->qualitySnapshot(TRAFFIC_TCP);
        assert(quality.localSendFailure && quality.sendFailures == 1);
        assert(!quality.outage && quality.timeouts == 0 && quality.pending == 0);
        assert(!other->qualitySnapshot(TRAFFIC_TCP).localSendFailure);
        assert(!remote->qualitySnapshot(TRAFFIC_UDP).localSendFailure);

        CliKcpTunnel* kcp = manager.createKcpTunnel(remote, 0xffffffffu, 54321,
            TRANSPORT_RELAY_KCP, FEC_NONE, ROOM_MTU_DEFAULT, KCP_PROFILE_REALTIME, TRAFFIC_UDP);
        kcp->setRelayMode(1, 2);
        remote->sendLatencyPing(TRAFFIC_UDP);
        kcp->update();
        quality = remote->qualitySnapshot(TRAFFIC_UDP);
        assert(quality.localSendFailure && quality.sendFailures == 1 && quality.timeouts == 0);
        remote->sendLatencyPing(TRAFFIC_UDP);
        quality = remote->qualitySnapshot(TRAFFIC_UDP);
        assert(quality.localSendFailure && quality.sendFailures == 2 && quality.pending == 0);

        socket_t receiver = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        assert(receiver != SOCK_INVALID);
        sockaddr_in address = makeAddr(0x7f000001u, 0);
        assert(bind(receiver, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
        socklen_t addressLen = sizeof(address);
        assert(getsockname(receiver, reinterpret_cast<sockaddr*>(&address), &addressLen) == 0);
        UdpRelayHeader header = {};
        header.type = UDP_RELAY_DATA;
        header.trafficClass = TRAFFIC_UDP;
        header.srcPeerId = htonl(1);
        header.dstPeerId = htonl(2);
        manager.udpSend(reinterpret_cast<const uint8_t*>(&header), sizeof(header),
                        0x7f000001u, ntohs(address.sin_port));
        assert(!remote->qualitySnapshot(TRAFFIC_UDP).localSendFailure);
        assert(remote->qualitySnapshot(TRAFFIC_TCP).localSendFailure); // Other path stays failed.
        header.type = UDP_RAW_RELAY_DATA;
        header.trafficClass = TRAFFIC_TCP;
        manager.udpSend(reinterpret_cast<const uint8_t*>(&header), sizeof(header),
                        0x7f000001u, ntohs(address.sin_port));
        assert(!remote->qualitySnapshot(TRAFFIC_TCP).localSendFailure);
        assert(remote->qualitySnapshot(TRAFFIC_TCP).timeouts == 0);
        sock_close(receiver);
        manager.stopDataPlane();
    }
public:
    static void run() {
        delayedReadyUsesCurrentMembersAndRelays();
        createdOnlyAfterReadiness();
        terminalFailuresRollbackOnce();
        cancelLeaveDisconnectAndNewSetup();
        oldPollResultCannotFinishNewRoom();
        cancellationDuringPrepareOrStartWins();
        pendingRuntimeFailureIsTerminalOnce();
        qualitySoloMembershipAndResume();
        udpSocketFailuresAreLocal();
    }
};

} // namespace VLan

int main() {
#ifdef _WIN32
    WSADATA winsock;
    assert(WSAStartup(MAKEWORD(2, 2), &winsock) == 0);
#endif
    VLan::CliRoomReadinessTests::run();
#ifdef _WIN32
    WSACleanup();
#endif
    std::puts("CLI room readiness orchestration: PASS");
    return 0;
}

#include "../../client-cli/src/cli_app.h"
#include <cassert>
#include <cstdio>
#include <functional>
#include <chrono>
#include <thread>

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
    bool inlineTunSetupForTesting() const override { return true; }
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

class ControlledCliLifecycleBackend : public CliTunLifecycleBackend {
public:
    std::atomic<bool> entered{false}, release{false}, ready{true};
    std::atomic<unsigned> cleaned{0}, starts{0};
    std::thread::id owner;
    bool initialize() override {
        owner = std::this_thread::get_id(); entered = true;
        while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return true;
    }
    bool configure(uint32_t, uint32_t, int) override { assert(owner == std::this_thread::get_id()); return true; }
    bool start() override { ++starts; return true; }
    void beginReadiness() override {}
    WindowsReadinessResult pollReadiness() override { return WindowsReadinessResult(ready ? WindowsReadinessState::Ready : WindowsReadinessState::Pending); }
    CliTunAdapter* adapter() override { return nullptr; }
    std::string error() const override { return "controlled native call"; }
    void cleanup() override { assert(owner == std::this_thread::get_id()); ++cleaned; }
};

class AsyncFakeCliApp : public CliApp {
public:
    std::shared_ptr<ControlledCliLifecycleBackend> backend = std::make_shared<ControlledCliLifecycleBackend>();
    int dataStarts = 0;
protected:
    std::shared_ptr<CliTunLifecycleBackend> createTunLifecycleBackend(const std::shared_ptr<RoomTaskControl>&) override { return backend; }
    TunSetupResult startTunDataPlane() override { ++dataStarts; return TunSetupResult(true); }
};

class CliRoomReadinessTests {
    template<typename Predicate> static void until(Predicate condition, const std::function<void()>& pump = std::function<void()>()) {
        const uint64_t deadline = roomMonotonicMs() + 3000;
        while (!condition() && roomMonotonicMs() < deadline) {
            if (pump) pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        assert(condition());
    }
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
        assert(app.beginRoomRequest());
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
    static void asynchronousOwnerRemainsResponsiveAndCancelsOnce() {
        AsyncFakeCliApp app;
        assert(app.beginRoomRequest());
        app.onRoomCreated(42, VNET_SUBNET | 2, tcp(), tcp(), ROOM_MTU_DEFAULT, false, Buffer());
        until([&]() { return app.backend->entered.load(); });
        assert(app.backend->owner != std::this_thread::get_id());
        const uint64_t before = roomMonotonicMs();
        for (int i = 0; i < 20; ++i) { app.pollRoomOperation(); app.printStatus(); }
        assert(roomMonotonicMs() - before < 1000 && !app.m_roomReady);
        app.doLeaveRoom(); app.doLeaveRoom();
        assert(app.m_tunJob && app.m_roomOperation.phase() == RoomOperationPhase::CleaningUp);
        assert(!app.beginRoomRequest());
        app.backend->release = true;
        until([&]() { return !app.m_tunJob; }, [&]() { app.pollRoomOperation(); });
        assert(app.backend->cleaned == 1 && app.backend->starts == 0);
        assert(!app.m_roomReady && app.dataStarts == 0 && app.m_roomOperation.canStart());
    }
    static void asynchronousReadyAndLateResults() {
        AsyncFakeCliApp app;
        app.backend->release = true;
        assert(app.beginRoomRequest());
        app.onRoomCreated(43, VNET_SUBNET | 2, tcp(), tcp(), ROOM_MTU_DEFAULT, false, Buffer());
        until([&]() { return app.m_roomReady; }, [&]() { app.pollRoomOperation(); });
        assert(app.dataStarts == 1 && app.m_roomOperation.phase() == RoomOperationPhase::InRoom);
        app.onRoomCreated(99, VNET_SUBNET | 3, tcp(), tcp(), ROOM_MTU_DEFAULT, false, Buffer());
        assert(app.m_currentRoomId == 43 && app.dataStarts == 1);
        app.doLeaveRoom();
        until([&]() { return !app.m_tunJob; }, [&]() { app.pollRoomOperation(); });
        assert(app.backend->cleaned == 1 && app.m_roomOperation.canStart());
    }
    static void requestDeadlineAndAmbiguousErrorRejectLateSuccess() {
        FakeCliApp app;
        assert(app.m_roomOperation.begin(roomMonotonicMs() - 15001, 1));
        app.pollRoomOperation();
        assert(app.m_roomOperation.canStart() && app.m_currentRoomId == 0);
        app.onRoomCreated(99, VNET_SUBNET | 3, tcp(), tcp(), ROOM_MTU_DEFAULT, false, Buffer());
        assert(app.prepares == 0 && !app.m_roomReady);
        assert(app.beginRoomRequest());
        app.setupCallbacks();
        app.m_signal.onRoomOperationFailure("unrelated General error before success");
        app.onRoomCreated(100, VNET_SUBNET | 3, tcp(), tcp(), ROOM_MTU_DEFAULT, false, Buffer());
        assert(app.prepares == 0 && !app.m_roomReady && app.m_currentRoomId == 0);
        app.doDisconnect();
        assert(!app.m_roomRecoveryScheduled && !app.m_roomRecoveryAttempt);
    }
    static void heartbeatsCannotExtendRoomRequestDeadline() {
        CliSignalClient signal;
        signal.m_conn.connected = true;
        signal.m_conn.lastRecvTime = currentTimeMs(); // Active heartbeat/list traffic.
        signal.m_secureReady = true;
        signal.m_roomOperationPending = true;
        signal.m_roomRequestStarted = roomMonotonicMs() - 15001;
        unsigned failures = 0;
        signal.onRoomOperationFailure = [&](const std::string&) { ++failures; };
        signal.checkTimeouts(); signal.checkTimeouts();
        assert(failures == 1 && !signal.roomOperationPending() && signal.m_discardRoomResponses);
    }
    static void initializationTimeoutCannotAcceptLateReady() {
        AsyncFakeCliApp app;
        assert(app.beginRoomRequest());
        app.onRoomCreated(44, VNET_SUBNET | 2, tcp(), tcp(), ROOM_MTU_DEFAULT, false, Buffer());
        until([&]() { return app.backend->entered.load(); });
        // Exercise a delayed deadline callback without a 30-second test sleep.
        assert(app.m_roomOperation.beginReadiness(roomMonotonicMs() - 10001));
        app.pollRoomOperation();
        assert(!app.m_roomReady && app.m_roomOperation.phase() == RoomOperationPhase::CleaningUp);
        app.backend->release = true;
        until([&]() { return !app.m_tunJob; }, [&]() { app.pollRoomOperation(); });
        assert(app.dataStarts == 0 && app.backend->cleaned == 1);
    }
    static void encryptedErrorThenLateSuccessCannotCreateRoom() {
        CliSignalClient signal;
        const Buffer key(SECURE_KEY_SIZE, 0x53);
        signal.m_secureReady = true;
        signal.m_conn.connected = true;
        signal.m_secureCipher.init(key.data(), true, "signal");
        SecureFrameCipher server;
        server.init(key.data(), false, "signal");
        signal.m_roomOperationPending = true;
        signal.m_roomRequestStarted = roomMonotonicMs();
        unsigned failed = 0, created = 0, fatal = 0;
        signal.onRoomOperationFailure = [&](const std::string&) { ++failed; };
        signal.onFatalAuthError = [&](uint16_t, const std::string&) { ++fatal; };
        signal.onRoomCreated = [&](uint32_t, uint32_t, RoomTrafficPolicy, RoomTrafficPolicy, uint16_t, bool, const Buffer&) { ++created; };
        auto deliver = [&](uint8_t type, const ByteBuffer& body) {
            Buffer plain(1, type);
            plain.insert(plain.end(), body.data(), body.data() + body.size());
            const Buffer wire = server.encrypt(plain.data(), plain.size());
            assert(signal.processMessage(MSG_ENCRYPTED, wire.data(), wire.size()));
        };
        ByteBuffer error;
        error.writeU16(static_cast<uint16_t>(AuthErrorCode::General)); error.writeString("earlier unrelated request failed");
        deliver(MSG_ERROR, error);
        assert(failed == 1 && signal.m_discardRoomResponses && !signal.roomOperationPending());
        ByteBuffer positive;
        positive.writeU32(42); positive.writeU32(VNET_SUBNET | 2);
        positive.writeU8(MODE_RELAY_RAW_UDP); positive.writeU8(FEC_NONE); positive.writeU8(KCP_PROFILE_REALTIME);
        positive.writeU8(MODE_RELAY_KCP); positive.writeU8(FEC_NONE); positive.writeU8(KCP_PROFILE_REALTIME);
        positive.writeU8(0); positive.writeU16(ROOM_MTU_DEFAULT);
        Buffer lease(RECONNECT_TOKEN_SIZE, 0x74); positive.writeBytes(lease.data(), lease.size());
        deliver(MSG_ROOM_CREATED, positive);
        assert(created == 0 && failed == 1);
        ByteBuffer challenge;
        challenge.writeU8(AUTH_KDF_PROFILE);
        Buffer salt(AUTH_SALT_SIZE, 1), nonce(32, 2);
        challenge.writeBytes(salt.data(), salt.size()); challenge.writeBytes(nonce.data(), nonce.size());
        deliver(MSG_AUTH_CHALLENGE, challenge);
        ByteBuffer latePassword;
        latePassword.writeU16(static_cast<uint16_t>(AuthErrorCode::RoomPassword)); latePassword.writeString("late room password rejection");
        deliver(MSG_ERROR, latePassword);
        assert(fatal == 0 && signal.m_authOperation == 0 && created == 0);
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
        asynchronousOwnerRemainsResponsiveAndCancelsOnce();
        asynchronousReadyAndLateResults();
        requestDeadlineAndAmbiguousErrorRejectLateSuccess();
        heartbeatsCannotExtendRoomRequestDeadline();
        initializationTimeoutCannotAcceptLateReady();
        encryptedErrorThenLateSuccessCannotCreateRoom();
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

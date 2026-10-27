#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include "../../client/src/network/room_manager.h"
#include "../../client/src/core/tunnel_manager.h"
#include "../../client/src/core/peer_connection.h"
#include "../../client/src/core/tun_adapter.h"
#include "../../client/src/core/raw_udp_tunnel.h"
#include "../../client/src/core/kcp_tunnel.h"
#include <cassert>
#include <cstdio>
#include <functional>
#include <thread>

namespace VLan {

bool g_verboseLog = false;

class FakeRoomManager : public RoomManager {
public:
    WindowsReadinessState state = WindowsReadinessState::Pending;
    bool prepareOk = true;
    bool startOk = true;
    int prepares = 0;
    int polls = 0;
    int starts = 0;
    int cancels = 0;
    std::function<void()> duringPoll;

protected:
    TunSetupResult setupTun() override {
        ++prepares;
        return TunSetupResult(prepareOk, TunSetupStage::Session,
                              prepareOk ? QString() : QStringLiteral("fake session failure"));
    }
    WindowsReadinessResult pollTunReadiness() override {
        ++polls;
        if (duringPoll) duringPoll();
        WindowsReadinessResult result(state);
        if (state == WindowsReadinessState::Failed) {
            result.error = "fake readiness timeout: DadState=Tentative";
            result.diagnostic = "fake index=7 dad=1 route=unavailable";
        }
        return result;
    }
    TunSetupResult startTunDataPlane() override {
        ++starts;
        return TunSetupResult(startOk, TunSetupStage::DataPlane,
                              startOk ? QString() : QStringLiteral("fake data-plane failure"));
    }
    void cancelTunReadiness() override { ++cancels; }
};

// Exercise the production orchestration with only adapter lifecycle effects
// replaced. No server login, Wintun handle or real network configuration occurs.
class RoomReadinessTests {
    static PeerInfo peer(uint32_t id, const char* name) {
        PeerInfo result;
        result.peerId = id;
        result.virtualIP = VNET_SUBNET | id;
        result.name = name;
        result.transport = TRANSPORT_NONE;
        return result;
    }
    static RoomTrafficPolicy tcp() {
        RoomTrafficPolicy policy = makeDefaultTcpPolicy();
        policy.transportMode = MODE_RELAY_TCP;
        return policy;
    }
    static void begin(FakeRoomManager& manager, bool created,
                      const QList<PeerInfo>& members = QList<PeerInfo>()) {
        manager.setRoomOperationPending(true);
        if (created) {
            manager.onRoomCreated(10, VNET_SUBNET | 1, tcp(), tcp(),
                                  ROOM_MTU_DEFAULT, false, QByteArray());
        } else {
            manager.onJoinResponse(10, VNET_SUBNET | 1, tcp(), tcp(),
                                   ROOM_MTU_DEFAULT, false, members, QByteArray());
        }
    }
    static void poll(FakeRoomManager& manager) {
        manager.pollRoomReadiness(manager.m_readinessGeneration,
                                  manager.m_connectionGeneration);
    }
    static void delayedReadyKeepsCurrentMembership() {
        FakeRoomManager manager;
        int joined = 0;
        int created = 0;
        int connected = 0;
        QList<QString> events;
        QObject::connect(&manager, &RoomManager::roomJoined, [&]() {
            ++joined;
            events.append("joined");
        });
        QObject::connect(&manager, &RoomManager::roomCreated, [&]() { ++created; });
        QObject::connect(&manager, &RoomManager::peerConnected,
                         [&](uint32_t, uint32_t, const QString&) {
            ++connected;
            events.append("peer");
        });
        begin(manager, false, QList<PeerInfo>() << peer(2, "old") << peer(3, "left"));
        assert(manager.roomOperationPending() && !manager.inRoom());
        assert(!manager.m_latencyTimer->isActive());
        manager.onPeerResumed(peer(2, "resumed"));
        manager.onPeerJoined(peer(4, "new"));
        manager.onPeerLeft(3);
        manager.onRelayReady(2);
        manager.onPeerJoined(peer(5, "brief"));
        manager.onRelayReady(5);
        manager.onPeerLeft(5);
        poll(manager);
        poll(manager);
        assert(manager.starts == 0 && joined == 0 && connected == 0);
        assert(manager.m_tunnel->allPeers().isEmpty());
        manager.state = WindowsReadinessState::Ready;
        poll(manager);
        assert(manager.inRoom() && !manager.roomOperationPending());
        assert(manager.starts == 1 && joined == 1 && created == 0 && connected == 2);
        assert(events.first() == "joined");
        assert(manager.m_latencyTimer->isActive());
        assert(manager.m_tunnel->peerById(2)->name() == "resumed");
        assert(manager.m_tunnel->peerById(2)->transport(TRAFFIC_TCP) == TRANSPORT_RELAY_TCP);
        assert(manager.m_tunnel->peerById(4)->transport(TRAFFIC_TCP) == TRANSPORT_NONE);
        assert(!manager.m_tunnel->peerById(3) && !manager.m_tunnel->peerById(5));
        poll(manager);
        assert(manager.starts == 1 && joined == 1);
        manager.leaveRoom();
        assert(!manager.inRoom() && manager.m_tunnel->allPeers().isEmpty());
    }
    static void createdOnlyAfterReady() {
        FakeRoomManager manager;
        int created = 0;
        QObject::connect(&manager, &RoomManager::roomCreated, [&]() { ++created; });
        begin(manager, true);
        poll(manager);
        assert(created == 0 && manager.starts == 0);
        manager.state = WindowsReadinessState::Ready;
        poll(manager);
        assert(created == 1 && manager.starts == 1 && manager.inRoom());
        manager.leaveRoom();
    }
    static void failuresRollbackExactlyOnce() {
        for (int stage = 0; stage < 3; ++stage) {
            FakeRoomManager manager;
            int errors = 0;
            int successes = 0;
            QObject::connect(&manager, &RoomManager::errorOccurred, [&]() { ++errors; });
            QObject::connect(&manager, &RoomManager::roomJoined, [&]() { ++successes; });
            manager.prepareOk = stage != 0;
            manager.startOk = stage != 2;
            manager.state = stage == 1 ? WindowsReadinessState::Failed : WindowsReadinessState::Ready;
            begin(manager, false, QList<PeerInfo>() << peer(2, "never visible"));
            poll(manager);
            poll(manager);
            assert(errors == 1 && successes == 0);
            assert(!manager.inRoom() && !manager.roomOperationPending());
            assert(manager.currentRoomId() == 0 && manager.myVirtualIP() == 0);
            assert(manager.m_pendingMembers.isEmpty() && manager.m_pendingRelayReady.isEmpty());
            assert(manager.m_tunnel->allPeers().isEmpty());
            assert(!manager.m_readinessTimer && !manager.m_latencyTimer->isActive());
            assert(manager.starts == (stage == 2 ? 1 : 0));
        }
    }
    static void cancellationDiscardsOldCallbacks() {
        for (int action = 0; action < 3; ++action) {
            FakeRoomManager manager;
            int created = 0;
            int errors = 0;
            QObject::connect(&manager, &RoomManager::roomCreated, [&]() { ++created; });
            QObject::connect(&manager, &RoomManager::errorOccurred, [&]() { ++errors; });
            begin(manager, true);
            const quint64 roomGeneration = manager.m_readinessGeneration;
            const quint64 connectionGeneration = manager.m_connectionGeneration;
            manager.onPeerJoined(peer(2, "stale"));
            manager.onRelayReady(2);
            if (action == 0) manager.leaveRoom();
            else if (action == 1) manager.disconnectFromServer();
            else manager.cancelConnectionAttempt();
            manager.state = WindowsReadinessState::Ready;
            manager.pollRoomReadiness(roomGeneration, connectionGeneration);
            assert(manager.polls == 0 && manager.starts == 0 && created == 0 && errors == 0);
            assert(!manager.roomOperationPending() && !manager.inRoom());
            assert(manager.m_pendingMembers.isEmpty() && manager.m_pendingRelayReady.isEmpty());
            assert(manager.cancels > 0 && !manager.m_readinessTimer);
        }
    }
    static void oldGenerationCannotFinishNewRoom() {
        FakeRoomManager manager;
        begin(manager, true);
        const quint64 oldRoom = manager.m_readinessGeneration;
        const quint64 oldConnection = manager.m_connectionGeneration;
        manager.leaveRoom();
        begin(manager, true);
        manager.state = WindowsReadinessState::Ready;
        manager.pollRoomReadiness(oldRoom, oldConnection);
        assert(manager.polls == 0 && manager.starts == 0);
        poll(manager);
        assert(manager.starts == 1 && manager.inRoom());
        manager.leaveRoom();
    }
    static void runtimeErrorDuringWaitRollsBackOnce() {
        FakeRoomManager manager;
        int errors = 0;
        QObject::connect(&manager, &RoomManager::errorOccurred, [&]() { ++errors; });
        begin(manager, true);
        manager.onTunRuntimeError("fake read failure");
        manager.onTunRuntimeError("duplicate fake read failure");
        QCoreApplication::processEvents();
        assert(errors == 1 && !manager.inRoom() && !manager.roomOperationPending());
        assert(manager.starts == 0 && !manager.m_readinessTimer);
    }
    static void queuedOldAdapterSignalsCannotFailNewRoom() {
        FakeRoomManager manager;
        int errors = 0;
        int statuses = 0;
        QObject::connect(&manager, &RoomManager::errorOccurred, [&]() { ++errors; });
        QObject::connect(&manager, &RoomManager::statusMessage, [&]() { ++statuses; });
        begin(manager, true);
        // This QObject has no initialized driver/adapter/session. Signals emitted
        // from a worker queue the exact production bindings on the manager.
        TunAdapter* oldAdapter = new TunAdapter(&manager);
        manager.m_tun = oldAdapter;
        manager.bindTunAdapterSignals(oldAdapter);
        std::thread worker([oldAdapter]() {
            oldAdapter->errorOccurred("old adapter queued failure");
            oldAdapter->firewallRuleChanged(true, false);
        });
        worker.join();
        manager.leaveRoom();
        begin(manager, true);
        const int statusBeforeDelivery = statuses;
        QCoreApplication::processEvents();
        assert(errors == 0 && statuses == statusBeforeDelivery);
        assert(manager.roomOperationPending() && !manager.inRoom() && manager.starts == 0);
        manager.leaveRoom();
    }
    static void pendingRuntimeErrorWinsOverReadyBeforeDeferredDelivery() {
        FakeRoomManager manager;
        int errors = 0;
        int successes = 0;
        QObject::connect(&manager, &RoomManager::errorOccurred, [&]() { ++errors; });
        QObject::connect(&manager, &RoomManager::roomCreated, [&]() { ++successes; });
        begin(manager, true);
        manager.onTunRuntimeError("read thread stopped before readiness");
        manager.state = WindowsReadinessState::Ready;
        poll(manager); // Run readiness before the deferred teardown callback.
        QCoreApplication::processEvents();
        assert(errors == 1 && successes == 0 && manager.starts == 0);
        assert(!manager.inRoom() && !manager.roomOperationPending() && !manager.m_readinessTimer);
    }
    static void cancellationDuringPollWins() {
        FakeRoomManager manager;
        begin(manager, true);
        manager.state = WindowsReadinessState::Ready;
        manager.duringPoll = [&manager]() { manager.leaveRoom(); };
        poll(manager);
        assert(manager.polls == 1 && manager.starts == 0 && !manager.inRoom());
    }
    static void pendingTimerDoesNotBlockEventLoop() {
        FakeRoomManager manager;
        begin(manager, true);
        int ticks = 0;
        QTimer heartbeat;
        QObject::connect(&heartbeat, &QTimer::timeout, [&]() { ++ticks; });
        heartbeat.start(5);
        QEventLoop loop;
        QTimer::singleShot(260, &loop, SLOT(quit()));
        loop.exec();
        assert(ticks >= 5 && manager.polls >= 2 && manager.starts == 0);
        manager.leaveRoom();
    }
    static void qualitySoloMembershipAndResume() {
        FakeRoomManager manager;
        manager.state = WindowsReadinessState::Ready;
        begin(manager, true);
        poll(manager);
        assert(manager.m_latencyTimer->interval() == 1000);
        int updates = 0;
        QObject::connect(&manager, &RoomManager::peerQualityUpdated,
                         [&](uint32_t, TrafficClass, ProbeQualitySnapshot) { ++updates; });
        for (int i = 0; i < 20; ++i) manager.onLatencyUpdate();
        assert(updates == 0 && manager.m_qualityLogStates.isEmpty());

        manager.onPeerJoined(peer(2, "quality peer"));
        PeerConnection* remote = manager.m_tunnel->peerById(2);
        assert(remote);
        manager.onLatencyUpdate();
        assert(updates == 2);
        assert(!remote->qualitySnapshot(TRAFFIC_TCP).active);
        assert(!remote->qualitySnapshot(TRAFFIC_TCP).outage);
        remote->setTcpRelaySender([remote](uint32_t, TrafficClass cls, const QByteArray& ping) {
            QByteArray pong = ping;
            assert(pong.size() == 6);
            pong[1] = static_cast<char>(LATENCY_PROBE_PONG);
            assert(remote->handleLatencyProbe(cls, pong));
        });
        remote->setTransport(TRAFFIC_TCP, TRANSPORT_RELAY_TCP);
        remote->setTransport(TRAFFIC_UDP, TRANSPORT_RELAY_TCP);
        for (int i = 0; i < 6; ++i) manager.onLatencyUpdate();
        assert(remote->qualitySnapshot(TRAFFIC_TCP).replies == 6);
        assert(!remote->qualitySnapshot(TRAFFIC_TCP).sampling);
        assert(!remote->qualitySnapshot(TRAFFIC_UDP).outage);
        int qualityMessages = 0;
        QObject::connect(&manager, &RoomManager::statusMessage,
                         [&](const QString&) { ++qualityMessages; });
        remote->setTcpRelayControlReady([]() { return false; });
        manager.onLatencyUpdate();
        manager.onLatencyUpdate();
        assert(qualityMessages == 2); // One local-failure event per path.
        assert(remote->qualitySnapshot(TRAFFIC_TCP).sendFailures == 2);
        assert(remote->qualitySnapshot(TRAFFIC_TCP).timeouts == 0);
        assert(!remote->qualitySnapshot(TRAFFIC_TCP).outage);
        remote->setTcpRelayControlReady([]() { return true; });
        manager.onLatencyUpdate();
        assert(qualityMessages == 4); // One local recovery event per path.
        manager.onPeerResumed(peer(2, "quality peer"));
        assert(remote->qualitySnapshot(TRAFFIC_TCP).replies == 0);
        assert(remote->qualitySnapshot(TRAFFIC_UDP).timeouts == 0);
        assert(manager.m_qualityLogStates.isEmpty());
        manager.onPeerLeft(2);
        const int afterRemoval = updates;
        for (int i = 0; i < 20; ++i) manager.onLatencyUpdate();
        assert(updates == afterRemoval && manager.m_qualityLogStates.isEmpty());
        manager.onPeerJoined(peer(2, "new session"));
        assert(manager.m_tunnel->peerById(2)->qualitySnapshot(TRAFFIC_TCP).replies == 0);
        manager.leaveRoom();
        assert(manager.m_qualityLogStates.isEmpty());
    }
    static void udpSocketFailuresAreLocal() {
        TunnelManager manager;
        manager.setMyPeerId(1);
        assert(manager.installSecureSession(1, QByteArray(SECURE_KEY_SIZE, 'q')));
        manager.m_dataPlaneState = DataPlaneState::Running; // Packet edge only, no TUN.
        PeerConnection* remote = manager.addPeer(2, VNET_SUBNET | 2, "socket errors");
        PeerConnection* other = manager.addPeer(3, VNET_SUBNET | 3, "unaffected");
        RawUdpTunnel* raw = manager.createRawUdpTunnel(remote, QHostAddress::LocalHostIPv6, 54321,
            FEC_NONE, ROOM_MTU_DEFAULT, TRAFFIC_TCP);
        raw->setRelayMode(1, 2);
        // IPv6 destination on the manager's IPv4 socket is a local failure.
        // Port zero itself is accepted by Windows, so cannot serve as an error fixture.
        remote->sendLatencyPing(TRAFFIC_TCP);
        ProbeQualitySnapshot quality = remote->qualitySnapshot(TRAFFIC_TCP);
        assert(quality.localSendFailure && quality.sendFailures == 1);
        assert(!quality.outage && quality.timeouts == 0 && quality.pending == 0);
        assert(!other->qualitySnapshot(TRAFFIC_TCP).localSendFailure);
        assert(!remote->qualitySnapshot(TRAFFIC_UDP).localSendFailure);

        QUdpSocket receiver;
        assert(receiver.bind(QHostAddress(QHostAddress::LocalHost), quint16(0)));
        raw->setPeerEndpoint(QHostAddress::LocalHost, receiver.localPort());
        remote->sendLatencyPing(TRAFFIC_TCP);
        quality = remote->qualitySnapshot(TRAFFIC_TCP);
        assert(!quality.localSendFailure && quality.sendFailures == 1 && quality.pending == 1);
        // Local submission success is the contract under test; delivery and
        // remote responses are covered by the transport tests separately.

        KcpTunnel* kcp = manager.createKcpTunnel(remote, QHostAddress::LocalHostIPv6, 54321,
            TRANSPORT_RELAY_KCP, FEC_NONE, ROOM_MTU_DEFAULT, KCP_PROFILE_REALTIME, TRAFFIC_UDP);
        kcp->setRelayMode(1, 2);
        remote->sendLatencyPing(TRAFFIC_UDP);
        kcp->update(); // Accepted KCP enqueue later reaches a failing OS send.
        quality = remote->qualitySnapshot(TRAFFIC_UDP);
        assert(quality.localSendFailure && quality.sendFailures == 1 && quality.timeouts == 0);
        remote->sendLatencyPing(TRAFFIC_UDP);
        quality = remote->qualitySnapshot(TRAFFIC_UDP);
        assert(quality.localSendFailure && quality.sendFailures == 2 && quality.pending == 0);
        assert(!remote->qualitySnapshot(TRAFFIC_TCP).localSendFailure);

        UdpRelayHeader header = {};
        header.type = UDP_RELAY_DATA;
        header.trafficClass = TRAFFIC_UDP;
        writeU32BE(reinterpret_cast<uint8_t*>(&header.srcPeerId), 1);
        writeU32BE(reinterpret_cast<uint8_t*>(&header.dstPeerId), 2);
        manager.sendUdpDatagram(QByteArray(reinterpret_cast<const char*>(&header), sizeof(header)),
                                QHostAddress::LocalHost, receiver.localPort());
        assert(!remote->qualitySnapshot(TRAFFIC_UDP).localSendFailure);
        assert(remote->qualitySnapshot(TRAFFIC_UDP).timeouts == 0);
        manager.stopDataPlane();
    }
public:
    static void run() {
        delayedReadyKeepsCurrentMembership();
        createdOnlyAfterReady();
        failuresRollbackExactlyOnce();
        cancellationDiscardsOldCallbacks();
        oldGenerationCannotFinishNewRoom();
        runtimeErrorDuringWaitRollsBackOnce();
        queuedOldAdapterSignalsCannotFailNewRoom();
        pendingRuntimeErrorWinsOverReadyBeforeDeferredDelivery();
        cancellationDuringPollWins();
        pendingTimerDoesNotBlockEventLoop();
        qualitySoloMembershipAndResume();
        udpSocketFailuresAreLocal();
    }
};

} // namespace VLan

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    VLan::RoomReadinessTests::run();
    std::puts("GUI room readiness orchestration: PASS");
    return 0;
}

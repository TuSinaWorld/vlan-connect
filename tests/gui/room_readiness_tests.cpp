#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include "../../client/src/network/room_manager.h"
#include "../../client/src/core/tunnel_manager.h"
#include "../../client/src/core/peer_connection.h"
#include "../../client/src/core/tun_adapter.h"
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
    }
};

} // namespace VLan

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    VLan::RoomReadinessTests::run();
    std::puts("GUI room readiness orchestration: PASS");
    return 0;
}

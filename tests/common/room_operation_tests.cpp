#include "room_operation.h"
#include <cassert>
#include <cstdio>

using namespace VLan;

int main() {
    RoomOperationTracker task;
    assert(task.canStart() && !task.pending());
    assert(task.begin(1000, 7));
    const uint64_t first = task.id();
    assert(task.pending() && !task.canStart());
    assert(task.phase() == RoomOperationPhase::WaitingServer);
    assert(task.deadlineMs() == 16000 && task.totalDeadlineMs() == 46000);
    assert(!task.begin(1001, 7));
    // Arbitrary heartbeat/list activity has no API that extends this deadline.
    assert(!task.expired(15999) && task.expired(16000));
    assert(!task.beginPreparation(16000));
    assert(task.beginCancel(16000));
    assert(!task.beginCancel(16001));
    task.beginCleanup(16002);
    assert(!task.cleanupSlow(26001) && task.cleanupSlow(26002));
    assert(task.pending() && !task.canStart());
    task.finishCleanup();
    assert(task.canStart());

    assert(task.begin(30000, 8));
    assert(task.id() != first && !task.matches(first, 7));
    assert(task.beginPreparation(44000));
    assert(task.localDeadlineMs() == 74000);
    assert(task.beginReadiness(73000));
    assert(task.deadlineMs() == 74000); // Readiness cannot extend local budget.
    assert(!task.markInRoom(74000));
    task.beginCancel(74000); task.beginCleanup(74000); task.finishCleanup();

    assert(task.begin(80000, 9));
    assert(task.beginPreparation(81000));
    assert(task.beginReadiness(81500));
    assert(task.deadlineMs() == 91500);
    assert(task.markInRoom(82000));
    assert(!task.pending() && !task.canStart() && !task.expired(999999));
    assert(task.beginCancel(83000));
    task.beginCleanup(83000); task.finishCleanup();

    RoomTaskControl worker(10, 12, roomMonotonicMs() + 1000);
    assert(worker.allowed() && worker.remainingMs(10000) <= 1000);
    assert(worker.remainingMs(10) <= 10);
    worker.cancelRequested = true;
    assert(!worker.allowed() && worker.remainingMs(10000) == 0);
    worker.cancelRequested = false;
    worker.deadlineMs = roomMonotonicMs() - 1;
    assert(worker.expired() && !worker.allowed() && worker.remainingMs(10000) == 0);
    worker.deadlineMs = 0;
    assert(worker.allowed() && worker.remainingMs(10000) == 10000);
    std::string stage;
    worker.progress = [&](const std::string& value) { stage = value; };
    worker.report("address-set");
    assert(stage == "address-set");
    std::puts("room operation deadlines, cancellation, generation and worker control: PASS");
    return 0;
}

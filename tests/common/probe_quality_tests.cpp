#include "probe_quality.h"
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>

using namespace VLan;

namespace {

uint32_t send(ProbeQualityTracker& tracker, uint32_t now, bool success = true) {
    const uint32_t id = tracker.beginProbe(now);
    tracker.completeSend(id, now, success);
    return id;
}

void defaultsAndNoPeer() {
    ProbeQualitySnapshot empty;
    assert(!empty.active && !empty.outage && !empty.localSendFailure);
    assert(empty.sampling && empty.rttMs == -1 && empty.timeoutPercent == 0.0);
    assert(empty.replies == 0 && empty.timeouts == 0 && empty.pending == 0);
    ProbeQualityTracker tracker;
    assert(!tracker.snapshot(120000).outage);
    tracker.setPath(1);
    assert(!tracker.snapshot(240000).active);
    assert(!tracker.snapshot(240000).outage);
}

void normalWindowAndSampling() {
    ProbeQualityTracker tracker;
    tracker.setPath(1);
    for (uint32_t i = 0; i < 5; ++i) {
        const uint32_t id = send(tracker, i * 1000);
        assert(tracker.reply(id, i * 1000 + 20));
        const ProbeQualitySnapshot q = tracker.snapshot(i * 1000 + 20);
        assert(q.active && !q.outage && q.replies == i + 1);
        assert(q.rttMs == 20 && q.sampling == (i < 4));
        assert(q.timeoutPercent == 0.0);
    }
    const uint32_t waiting = send(tracker, 5000);
    (void)waiting;
    ProbeQualitySnapshot q = tracker.snapshot(5001);
    assert(q.replies == 5 && q.pending == 1 && !q.sampling);
    assert(q.timeoutPercent == 0.0); // Pending never dilutes the denominator.
    q = tracker.snapshot(60000);
    assert(q.replies == 4 && q.timeouts == 1 && q.pending == 0);
    assert(std::fabs(q.timeoutPercent - 20.0) < 0.001);
    q = tracker.snapshot(64000);
    assert(q.replies == 0 && q.timeouts == 1 && q.sampling);
    assert(q.timeoutPercent == 100.0);
    q = tracker.snapshot(65000);
    assert(q.replies == 0 && q.timeouts == 0 && q.sampling);
}

void deadlineAndUnknownReplies() {
    ProbeQualityTracker tracker;
    tracker.setPath(1);
    const uint32_t id = send(tracker, 0);
    assert(!tracker.reply(id + 1, 500));
    assert(tracker.snapshot(9999).pending == 1);
    assert(tracker.snapshot(9999).rttMs == -1);
    ProbeQualitySnapshot q = tracker.snapshot(10000);
    assert(q.timeouts == 1 && q.pending == 0 && q.outage);
    assert(!tracker.reply(id, 10000));
    assert(!tracker.reply(id, 10001));
    assert(tracker.snapshot(10001).replies == 0);
    assert(tracker.snapshot(10001).rttMs == -1);

    tracker.reset();
    const uint32_t timely = send(tracker, 20000);
    assert(tracker.reply(timely, 29999));
    assert(!tracker.reply(timely, 29999));
    assert(!tracker.reply(timely, 30000));
    q = tracker.snapshot(30000);
    assert(q.replies == 1 && q.timeouts == 0 && q.rttMs == 9999);
    assert(tracker.snapshot(39999).rttMs == -1);
}

void failuresAndSynchronousEcho() {
    ProbeQualityTracker tracker;
    tracker.setPath(1);
    const uint32_t failed = send(tracker, 0, false);
    ProbeQualitySnapshot q = tracker.snapshot(15000);
    assert(!q.active && !q.outage && q.localSendFailure);
    assert(q.sendFailures == 1 && q.pending == 0 && q.timeouts == 0);
    assert(!tracker.reply(failed, 15000));
    const uint32_t sync = tracker.beginProbe(16000);
    assert(tracker.reply(sync, 16001)); // Happens inside the send callback.
    assert(!tracker.reply(sync, 16001));
    q = tracker.snapshot(16001);
    assert(q.replies == 0 && q.pending == 0);
    tracker.completeSend(sync, 16002, true);
    q = tracker.snapshot(16002);
    assert(q.active && !q.localSendFailure && !q.outage);
    assert(q.replies == 1 && q.rttMs == 1 && q.sendFailures == 1);

    const uint32_t inconsistent = tracker.beginProbe(17000);
    assert(tracker.reply(inconsistent, 17001));
    tracker.completeSend(inconsistent, 17002, false);
    q = tracker.snapshot(17002);
    assert(q.replies == 1 && q.sendFailures == 2 && q.rttMs == 1);

    ProbeQualityTracker failedOnly;
    failedOnly.setPath(1);
    for (uint32_t now = 0; now <= 120000; now += 1000)
        send(failedOnly, now, false);
    q = failedOnly.snapshot(120000);
    assert(!q.active && !q.outage && q.sendFailures == 60);
    assert(q.timeouts == 0 && q.replies == 0 && q.pending == 0);
}

void mixedOutcomes() {
    ProbeQualityTracker tracker;
    tracker.setPath(1);
    for (uint32_t i = 0; i < 10; ++i) {
        const uint32_t id = send(tracker, i * 1000);
        if ((i % 2) == 0)
            assert(tracker.reply(id, i * 1000 + 10));
    }
    send(tracker, 10000, false);
    const ProbeQualitySnapshot q = tracker.snapshot(19000);
    assert(q.replies == 5 && q.timeouts == 5 && q.pending == 0);
    assert(q.sendFailures == 1 && !q.sampling && q.timeoutPercent == 50.0);
}

void asynchronousLocalTransportFailure() {
    ProbeQualityTracker tracker;
    tracker.setPath(1);
    const uint32_t first = send(tracker, 1000);
    send(tracker, 2000);
    tracker.reportTransportSend(2500, false);
    ProbeQualitySnapshot q = tracker.snapshot(2500);
    assert(q.localSendFailure && q.sendFailures == 2 && q.pending == 0);
    assert(q.timeouts == 0 && !q.outage);
    assert(!tracker.reply(first, 2600));
    // KCP enqueue success must not clear a known socket send failure.
    send(tracker, 3000);
    q = tracker.snapshot(3000);
    assert(q.localSendFailure && q.sendFailures == 3 && q.pending == 0);
    q = tracker.snapshot(30000);
    assert(q.timeouts == 0 && !q.outage && q.localSendFailure);
    tracker.reportTransportSend(31000, true);
    q = tracker.snapshot(31000);
    assert(!q.localSendFailure && !q.outage && q.pending == 0);
    const uint32_t good = send(tracker, 32000);
    assert(tracker.reply(good, 32050));
    q = tracker.snapshot(32050);
    assert(!q.localSendFailure && !q.outage && q.replies == 1);

    // Socket recovery does not hide a separate queue submission failure.
    send(tracker, 33000, false);
    tracker.reportTransportSend(33001, false);
    tracker.reportTransportSend(33002, true);
    assert(tracker.snapshot(33002).localSendFailure);
    send(tracker, 34000);
    assert(!tracker.snapshot(34000).localSendFailure);

    // Raw UDP can report the failure inside send(), before completeSend().
    ProbeQualityTracker synchronous;
    synchronous.setPath(1);
    const uint32_t failed = synchronous.beginProbe(0);
    synchronous.reportTransportSend(0, false);
    synchronous.completeSend(failed, 0, true);
    q = synchronous.snapshot(20000);
    assert(!q.active && !q.outage && q.localSendFailure);
    assert(q.sendFailures == 1 && q.timeouts == 0);
}

void outageAndRecovery() {
    ProbeQualityTracker tracker;
    tracker.setPath(1);
    for (uint32_t i = 0; i <= 10; ++i)
        send(tracker, i * 1000);
    ProbeQualitySnapshot q = tracker.snapshot(10000);
    assert(q.outage && q.timeouts == 1 && q.pending == 10);
    const uint32_t first = send(tracker, 10100);
    assert(tracker.reply(first, 10150));
    assert(tracker.snapshot(10150).outage); // One reply does not clear a warning.
    assert(!tracker.reply(first, 10151));
    assert(tracker.snapshot(11000).outage); // Old lost probes keep expiring.
    const uint32_t second = send(tracker, 11100);
    assert(tracker.reply(second, 11150));
    assert(!tracker.snapshot(11150).outage);
    assert(!tracker.snapshot(21149).outage);
    assert(tracker.snapshot(21150).outage);
}

void cancellationAndTransportChanges() {
    ProbeQualityTracker tracker;
    tracker.setPath(1);
    const uint32_t good = send(tracker, 1000);
    assert(tracker.reply(good, 1020));
    const uint32_t cancelled = send(tracker, 2000);
    tracker.setPath(0);
    ProbeQualitySnapshot q = tracker.snapshot(15000);
    assert(!q.active && q.replies == 1 && q.timeouts == 0 && q.pending == 0);
    assert(q.rttMs == -1 && !tracker.reply(cancelled, 15000));
    tracker.setPath(1);
    q = tracker.snapshot(15000);
    assert(q.active && q.replies == 1 && q.outage);
    tracker.setPath(2);
    q = tracker.snapshot(15000);
    assert(!q.active && q.replies == 0 && !q.outage);
    assert(!tracker.reply(good, 15000));
    send(tracker, 16000);
    tracker.reset(); // Member resumed / new peer session.
    q = tracker.snapshot(40000);
    assert(!q.active && q.timeouts == 0 && !q.outage);
    send(tracker, 40000);
    assert(tracker.snapshot(40001).pending == 1); // reset preserves the path.
}

void clockAndIdentifierWrap() {
    ProbeQualityTracker tracker;
    tracker.setPath(1);
    const uint32_t start = 0xfffffff0u;
    const uint32_t id = tracker.beginProbe(start, 0xffffffffu);
    tracker.completeSend(id, start, true);
    assert(tracker.reply(0xffffffffu, 4));
    ProbeQualitySnapshot q = tracker.snapshot(4);
    assert(q.rttMs == 20 && q.replies == 1 && !q.outage);
    const uint32_t zero = tracker.beginProbe(5, 0);
    tracker.completeSend(zero, 5, true);
    assert(tracker.reply(0, 25));
    assert(tracker.snapshot(25).replies == 2);
    assert(!tracker.snapshot(10024).outage);
    assert(tracker.snapshot(10025).outage);
    assert(tracker.snapshot(60000u - 16u).replies == 1); // start + 60 seconds after wrap.
    assert(tracker.snapshot(60005).replies == 0);

    const uint32_t a = nextProbeEchoId();
    const uint32_t b = nextProbeEchoId();
    assert(b - a == 1u);
}

void reorderedRepliesAndBoundedHistory() {
    ProbeQualityTracker tracker;
    tracker.setPath(1);
    const uint32_t a = send(tracker, 1000);
    const uint32_t b = send(tracker, 2000);
    assert(tracker.reply(b, 2100));
    assert(tracker.reply(a, 2200));
    assert(tracker.snapshot(2200).replies == 2);
    assert(tracker.snapshot(2200).rttMs == 1200);
    for (uint32_t now = 3000; now < 300000; now += 1000) {
        const uint32_t id = send(tracker, now);
        assert(tracker.reply(id, now + 50));
        const ProbeQualitySnapshot q = tracker.snapshot(now + 50);
        assert(q.replies <= 60 && q.pending == 0 && !q.outage);
    }
}

} // namespace

int main() {
    defaultsAndNoPeer();
    normalWindowAndSampling();
    deadlineAndUnknownReplies();
    failuresAndSynchronousEcho();
    mixedOutcomes();
    asynchronousLocalTransportFailure();
    outageAndRecovery();
    cancellationAndTransportChanges();
    clockAndIdentifierWrap();
    reorderedRepliesAndBoundedHistory();
    std::puts("probe quality tests passed");
    return 0;
}

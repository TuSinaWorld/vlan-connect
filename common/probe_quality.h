#ifndef VLAN_PROBE_QUALITY_H
#define VLAN_PROBE_QUALITY_H

#include <atomic>
#include <cstdint>
#include <deque>

namespace VLan {

struct ProbeQualitySnapshot {
    bool active;
    bool outage;
    bool localSendFailure;
    bool sampling;
    unsigned replies;
    unsigned timeouts;
    unsigned pending;
    unsigned sendFailures;
    int rttMs;
    double timeoutPercent;

    ProbeQualitySnapshot()
        : active(false), outage(false), localSendFailure(false), sampling(true),
          replies(0), timeouts(0), pending(0), sendFailures(0), rttMs(-1),
          timeoutPercent(0.0) {}
};

// A process-wide echo identifier, rather than a timestamp, prevents replies
// belonging to removed peers or an earlier room from matching new probes.
// Unsigned wrap is intentional; at one probe/second per path the bounded
// pending window cannot contain another use of the same identifier.
inline uint32_t nextProbeEchoId() {
    static std::atomic<uint32_t> sequence(0);
    return sequence.fetch_add(1, std::memory_order_relaxed);
}

class ProbeQualityTracker {
public:
    static const uint32_t WINDOW_MS = 60000u;
    static const uint32_t TIMEOUT_MS = 10000u;
    static const unsigned MIN_SAMPLES = 5u;

    ProbeQualityTracker()
        : m_pathKind(0), m_enabled(false), m_started(false), m_outage(false),
          m_queueSendFailure(false), m_transportSendFailure(false),
          m_hasReply(false), m_reference(0),
          m_lastReply(0), m_rtt(-1), m_recoveryReplies(0) {}

    // Zero is the disconnected/temporarily rebuilding path. Keep completed
    // history across rebuilding the same transport, but never keep pending
    // probes: their callbacks belong to the discarded transport generation.
    void setPath(int kind) {
        if (kind == 0) {
            suspend();
            return;
        }
        if (m_pathKind != kind) {
            reset();
            m_pathKind = kind;
        }
        m_enabled = true;
    }

    void reset() {
        m_records.clear();
        m_started = false;
        m_outage = false;
        m_queueSendFailure = false;
        m_transportSendFailure = false;
        m_hasReply = false;
        m_reference = 0;
        m_lastReply = 0;
        m_rtt = -1;
        m_recoveryReplies = 0;
    }

    void suspend() {
        m_enabled = false;
        for (std::deque<Record>::iterator it = m_records.begin(); it != m_records.end();) {
            if (it->state == Pending)
                it = m_records.erase(it);
            else
                ++it;
        }
    }

    // Register before handing data to a transport: an in-process transport or
    // test double may synchronously deliver the PONG inside its send callback.
    uint32_t beginProbe(uint32_t now, uint32_t id = nextProbeEchoId()) {
        expire(now);
        if (m_enabled)
            m_records.push_back(Record(id, now));
        return id;
    }

    void completeSend(uint32_t id, uint32_t now, bool success) {
        Record* record = findPending(id);
        if (!record)
            return;
        const bool wasLocalFailure = m_queueSendFailure || m_transportSendFailure;
        m_queueSendFailure = !success;
        if (!success || m_transportSendFailure) {
            record->state = SendFailed;
            record->earlyReply = false;
            return;
        }
        if (wasLocalFailure && !m_outage)
            m_reference = record->sent;
        record->committed = true;
        if (!m_started) {
            m_started = true;
            m_reference = record->sent;
        }
        if (record->earlyReply)
            acceptReply(*record, record->replyAt);
        expire(now);
    }

    // A tunnel accepting bytes is not proof that the local socket sent them.
    // KCP may submit datagrams later, so keep OS failure separate from queue
    // acceptance. Cancel outstanding probes as local failures instead of
    // allowing them to become misleading remote timeouts.
    void reportTransportSend(uint32_t now, bool success) {
        expire(now);
        const bool wasTransportFailure = m_transportSendFailure;
        m_transportSendFailure = !success;
        if (success) {
            if (wasTransportFailure && !m_outage)
                m_reference = now;
            return;
        }
        for (std::deque<Record>::iterator it = m_records.begin(); it != m_records.end(); ++it) {
            if (it->state == Pending) {
                it->state = SendFailed;
                it->earlyReply = false;
            }
        }
    }

    bool reply(uint32_t id, uint32_t now) {
        expire(now);
        Record* record = findPending(id);
        if (!m_enabled || !record || now - record->sent >= TIMEOUT_MS)
            return false;
        if (!record->committed) {
            if (record->earlyReply)
                return false;
            record->earlyReply = true;
            record->replyAt = now;
            return true;
        }
        acceptReply(*record, now);
        return true;
    }

    ProbeQualitySnapshot snapshot(uint32_t now) {
        expire(now);
        ProbeQualitySnapshot result;
        result.active = m_enabled && m_started;
        result.outage = m_outage;
        result.localSendFailure = m_queueSendFailure || m_transportSendFailure;
        result.rttMs = latencyMs(now);
        for (std::deque<Record>::const_iterator it = m_records.begin(); it != m_records.end(); ++it) {
            switch (it->state) {
            case Replied: ++result.replies; break;
            case TimedOut: ++result.timeouts; break;
            case SendFailed: ++result.sendFailures; break;
            case Pending: if (it->committed) ++result.pending; break;
            }
        }
        const unsigned settled = result.replies + result.timeouts;
        result.sampling = settled < MIN_SAMPLES;
        if (settled != 0)
            result.timeoutPercent = 100.0 * result.timeouts / settled;
        return result;
    }

    int latencyMs(uint32_t now) const {
        if (!m_enabled || !m_hasReply || now - m_lastReply >= TIMEOUT_MS)
            return -1;
        return m_rtt;
    }

private:
    enum State { Pending, Replied, TimedOut, SendFailed };
    struct Record {
        uint32_t id;
        uint32_t sent;
        uint32_t replyAt;
        State state;
        bool committed;
        bool earlyReply;
        Record(uint32_t token, uint32_t time)
            : id(token), sent(time), replyAt(0), state(Pending),
              committed(false), earlyReply(false) {}
    };

    Record* findPending(uint32_t id) {
        for (std::deque<Record>::iterator it = m_records.begin(); it != m_records.end(); ++it) {
            if (it->id == id && it->state == Pending)
                return &*it;
        }
        return 0;
    }

    void acceptReply(Record& record, uint32_t now) {
        record.state = Replied;
        m_rtt = static_cast<int>(now - record.sent);
        m_hasReply = true;
        m_lastReply = now;
        m_reference = now;
        if (m_outage && ++m_recoveryReplies >= 2u) {
            m_outage = false;
            m_recoveryReplies = 0;
        }
    }

    void expire(uint32_t now) {
        // The caller uses a monotonic 32-bit millisecond clock. Subtraction is
        // wrap-safe because retained intervals are far shorter than 2^31 ms.
        for (std::deque<Record>::iterator it = m_records.begin(); it != m_records.end(); ++it) {
            if (it->state == Pending && it->committed && now - it->sent >= TIMEOUT_MS) {
                it->state = TimedOut;
                // Probes lost before a newly received reply must not delay
                // recovery merely because their deadlines arrive afterwards.
                if (!m_hasReply || static_cast<int32_t>(it->sent - m_lastReply) >= 0)
                    m_recoveryReplies = 0;
            }
        }
        while (!m_records.empty() && now - m_records.front().sent >= WINDOW_MS)
            m_records.pop_front();
        if (m_enabled && m_started && !m_queueSendFailure && !m_transportSendFailure &&
            now - m_reference >= TIMEOUT_MS) {
            m_outage = true;
            m_recoveryReplies = 0;
        }
    }

    std::deque<Record> m_records;
    int m_pathKind;
    bool m_enabled;
    bool m_started;
    bool m_outage;
    bool m_queueSendFailure;
    bool m_transportSendFailure;
    bool m_hasReply;
    uint32_t m_reference;
    uint32_t m_lastReply;
    int m_rtt;
    unsigned m_recoveryReplies;
};

} // namespace VLan

#endif // VLAN_PROBE_QUALITY_H

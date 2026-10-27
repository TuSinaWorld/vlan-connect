#ifndef VLAN_ROOM_OPERATION_H
#define VLAN_ROOM_OPERATION_H

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace VLan {

inline uint64_t roomMonotonicMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

enum class RoomOperationPhase {
    Idle, WaitingServer, PreparingAdapter, WaitingReadiness, InRoom,
    Cancelling, CleaningUp
};

inline const char* roomOperationPhaseName(RoomOperationPhase phase) {
    switch (phase) {
    case RoomOperationPhase::WaitingServer: return "waiting-server";
    case RoomOperationPhase::PreparingAdapter: return "preparing-adapter";
    case RoomOperationPhase::WaitingReadiness: return "waiting-readiness";
    case RoomOperationPhase::InRoom: return "in-room";
    case RoomOperationPhase::Cancelling: return "cancelling";
    case RoomOperationPhase::CleaningUp: return "cleaning-up";
    default: return "idle";
    }
}

class RoomOperationTracker {
public:
    RoomOperationTracker() : m_phase(RoomOperationPhase::Idle), m_id(0),
        m_connection(0), m_started(0), m_phaseStarted(0), m_deadline(0),
        m_totalDeadline(0), m_localDeadline(0) {}

    bool begin(uint64_t now, uint64_t connectionGeneration) {
        if (m_phase != RoomOperationPhase::Idle) return false;
        ++m_id;
        m_connection = connectionGeneration;
        m_started = m_phaseStarted = now;
        m_totalDeadline = now + 45000;
        m_deadline = now + 15000;
        m_localDeadline = 0;
        m_phase = RoomOperationPhase::WaitingServer;
        return true;
    }
    bool beginPreparation(uint64_t now) {
        if (m_phase != RoomOperationPhase::WaitingServer || expired(now)) return false;
        m_localDeadline = (std::min)(now + 30000, m_totalDeadline);
        m_deadline = m_localDeadline;
        m_phaseStarted = now;
        m_phase = RoomOperationPhase::PreparingAdapter;
        return true;
    }
    bool beginReadiness(uint64_t now) {
        if (m_phase != RoomOperationPhase::PreparingAdapter || expired(now)) return false;
        m_deadline = (std::min)(now + 10000, m_localDeadline);
        m_phaseStarted = now;
        m_phase = RoomOperationPhase::WaitingReadiness;
        return true;
    }
    bool markInRoom(uint64_t now) {
        if ((m_phase != RoomOperationPhase::PreparingAdapter &&
             m_phase != RoomOperationPhase::WaitingReadiness) || expired(now)) return false;
        m_phase = RoomOperationPhase::InRoom;
        m_phaseStarted = now;
        m_deadline = 0;
        return true;
    }
    bool beginCancel(uint64_t now) {
        if (m_phase == RoomOperationPhase::Idle ||
            m_phase == RoomOperationPhase::Cancelling ||
            m_phase == RoomOperationPhase::CleaningUp) return false;
        m_phase = RoomOperationPhase::Cancelling;
        m_phaseStarted = now;
        m_deadline = 0;
        return true;
    }
    void beginCleanup(uint64_t now) {
        if (m_phase == RoomOperationPhase::CleaningUp) return;
        m_phase = RoomOperationPhase::CleaningUp;
        m_phaseStarted = now;
        m_deadline = 0;
    }
    void finishCleanup() { m_phase = RoomOperationPhase::Idle; m_deadline = 0; }
    bool expired(uint64_t now) const { return m_deadline != 0 && now >= m_deadline; }
    bool cleanupSlow(uint64_t now) const {
        return m_phase == RoomOperationPhase::CleaningUp && now - m_phaseStarted >= 10000;
    }
    bool pending() const {
        return m_phase != RoomOperationPhase::Idle && m_phase != RoomOperationPhase::InRoom;
    }
    bool canStart() const { return m_phase == RoomOperationPhase::Idle; }
    RoomOperationPhase phase() const { return m_phase; }
    uint64_t id() const { return m_id; }
    uint64_t connectionGeneration() const { return m_connection; }
    uint64_t deadlineMs() const { return m_deadline; }
    uint64_t totalDeadlineMs() const { return m_totalDeadline; }
    uint64_t localDeadlineMs() const { return m_localDeadline; }
    uint64_t elapsedMs(uint64_t now) const { return now - m_started; }
    bool matches(uint64_t id, uint64_t connection) const {
        return m_id == id && m_connection == connection;
    }
private:
    RoomOperationPhase m_phase;
    uint64_t m_id, m_connection, m_started, m_phaseStarted, m_deadline;
    uint64_t m_totalDeadline, m_localDeadline;
};

// One lifecycle worker owns each control. Only cancellation/deadline are
// modified cross-thread. Progress runs on the worker and must not touch UI.
struct RoomTaskControl {
    std::atomic<bool> cancelRequested;
    std::atomic<uint64_t> deadlineMs;
    uint64_t operationId;
    uint64_t connectionGeneration;
    std::function<void(const std::string&)> progress;

    RoomTaskControl(uint64_t id = 0, uint64_t connection = 0, uint64_t deadline = 0)
        : cancelRequested(false), deadlineMs(deadline), operationId(id),
          connectionGeneration(connection) {}
    bool cancelled() const { return cancelRequested.load(); }
    bool expired() const {
        const uint64_t end = deadlineMs.load();
        return end != 0 && roomMonotonicMs() >= end;
    }
    bool allowed() const { return !cancelled() && !expired(); }
    unsigned long remainingMs(unsigned long cap) const {
        if (cancelled()) return 0;
        const uint64_t end = deadlineMs.load();
        if (!end) return cap;
        const uint64_t now = roomMonotonicMs();
        return now >= end ? 0 : static_cast<unsigned long>((std::min)(uint64_t(cap), end - now));
    }
    void report(const std::string& stage) const { if (progress) progress(stage); }
};

} // namespace VLan
#endif

#ifndef VLAN_CLI_TUN_LIFECYCLE_H
#define VLAN_CLI_TUN_LIFECYCLE_H

#include "cli_tun.h"
#include "cli_log.h"
#include "room_operation.h"
#include <condition_variable>
#include <memory>

namespace VLan {

// The lifecycle owner is separate from CliTunAdapter's packet reader. No
// caller may destroy or shut down its adapter while a native call is active.
class CliTunLifecycleBackend {
public:
    virtual ~CliTunLifecycleBackend() {}
    virtual bool initialize() = 0;
    virtual bool configure(uint32_t ip, uint32_t mask, int mtu) = 0;
    virtual bool start() = 0;
    virtual void beginReadiness() = 0;
    virtual WindowsReadinessResult pollReadiness() = 0;
    virtual CliTunAdapter* adapter() = 0;
    virtual std::string error() const = 0;
    virtual void cleanup() = 0;
};

class NativeCliTunLifecycleBackend : public CliTunLifecycleBackend {
public:
    explicit NativeCliTunLifecycleBackend(const std::shared_ptr<RoomTaskControl>& control)
        : m_control(control) {}
    bool initialize() override {
        m_adapter.reset(new CliTunAdapter());
        m_adapter->setSetupControl(m_control);
        return m_adapter->initialize();
    }
    bool configure(uint32_t ip, uint32_t mask, int mtu) override {
        return m_adapter->configureIP(ip, mask, mtu);
    }
    bool start() override { return m_adapter->startSession(); }
    void beginReadiness() override { m_adapter->beginReadiness(); }
    WindowsReadinessResult pollReadiness() override {
        const std::vector<std::string> errors = m_adapter->errorQueue().popAll();
        if (!errors.empty() || !m_adapter->isRunning()) {
            m_error = errors.empty() ? "TUN reader stopped before readiness" : errors.front();
            WindowsReadinessResult failed(WindowsReadinessState::Failed); failed.error = m_error; return failed;
        }
        WindowsReadinessResult result = m_adapter->pollReadiness();
        if (result.state == WindowsReadinessState::Ready && !m_adapter->isRunning()) {
            m_error = "TUN reader stopped during readiness";
            result.state = WindowsReadinessState::Failed; result.error = m_error;
        }
        return result;
    }
    CliTunAdapter* adapter() override { return m_adapter.get(); }
    std::string error() const override { return !m_error.empty() ? m_error : m_adapter ? m_adapter->configurationError() : ""; }
    void cleanup() override {
        // Cleanup is not subject to the cancelled preparation token. The
        // worker retains ownership until all scoped rollback has completed.
        if (m_adapter) { m_adapter->setSetupControl(std::shared_ptr<RoomTaskControl>()); m_adapter.reset(); }
    }
private:
    std::shared_ptr<RoomTaskControl> m_control;
    std::unique_ptr<CliTunAdapter> m_adapter;
    std::string m_error;
};

struct CliTunLifecycleEvent {
    enum Kind { Progress, Ready, Failed, Cleaned } kind;
    uint64_t generation;
    uint64_t at;
    std::string stage;
    std::string error;
    CliTunAdapter* adapter;
    CliTunLifecycleEvent(Kind k, uint64_t g, const std::string& s = "", const std::string& e = "", CliTunAdapter* a = nullptr)
        : kind(k), generation(g), at(roomMonotonicMs()), stage(s), error(e), adapter(a) {}
};

class CliTunLifecycleJob {
public:
    typedef std::function<std::shared_ptr<CliTunLifecycleBackend>(const std::shared_ptr<RoomTaskControl>&)> Factory;
    CliTunLifecycleJob(uint64_t generation, uint32_t ip, int mtu,
                       const std::shared_ptr<RoomTaskControl>& control, Factory factory)
        : m_generation(generation), m_ip(ip), m_mtu(mtu), m_control(control),
          m_factory(factory), m_cancel(false), m_done(false) {}
    ~CliTunLifecycleJob() {
        // Normal shutdown pumps the event loop until done, so this join never
        // waits for a native operation on that loop. The destructor fallback
        // still respects resource ownership for embedding/test callers.
        cancel();
        if (m_thread.joinable()) m_thread.join();
    }
    void start() {
        try { m_thread = std::thread(&CliTunLifecycleJob::run, this); }
        catch (const std::exception& e) {
            m_stageAt = roomMonotonicMs();
            fail("worker-start", e.what());
            m_events.push(CliTunLifecycleEvent(CliTunLifecycleEvent::Cleaned, m_generation, "idle"));
            m_done = true;
        }
    }
    void cancel() { m_cancel = true; if (m_control) m_control->cancelRequested = true; m_wake.notify_all(); }
    bool done() const { return m_done.load(); }
    std::vector<CliTunLifecycleEvent> events() { return m_events.popAll(); }
private:
    bool allowed() const {
        return !m_cancel.load() && (!m_control || m_control->allowed());
    }
    void progress(const char* stage) {
        if (!m_lastStage.empty())
            LOG_INFO("[room-operation] task=%llu phase=%s event=end elapsed_ms=%llu",
                static_cast<unsigned long long>(m_generation), m_lastStage.c_str(),
                static_cast<unsigned long long>(roomMonotonicMs() - m_stageAt));
        m_lastStage = stage; m_stageAt = roomMonotonicMs();
        LOG_INFO("[room-operation] task=%llu phase=%s event=begin", static_cast<unsigned long long>(m_generation), stage);
        m_events.push(CliTunLifecycleEvent(CliTunLifecycleEvent::Progress, m_generation, stage));
    }
    void fail(const char* stage, const std::string& detail) {
        LOG_INFO("[room-operation] task=%llu phase=%s event=failed elapsed_ms=%llu",
            static_cast<unsigned long long>(m_generation), stage,
            static_cast<unsigned long long>(roomMonotonicMs() - m_stageAt));
        m_events.push(CliTunLifecycleEvent(CliTunLifecycleEvent::Failed, m_generation, stage,
            detail.empty() ? std::string(stage) + " failed" : detail));
    }
    void run() {
        std::shared_ptr<CliTunLifecycleBackend> backend;
        try {
            backend = m_factory(m_control);
            bool ok = allowed() && static_cast<bool>(backend);
            if (ok) { progress("prepare-adapter"); ok = backend->initialize(); }
            if (!ok || !allowed()) { if (!m_cancel) fail("prepare-adapter", backend ? backend->error() : "setup cancelled or expired"); }
            else {
                progress("configure-network"); ok = backend->configure(m_ip, VNET_MASK, m_mtu);
                if (!ok || !allowed()) { if (!m_cancel) fail("configure-network", backend->error()); }
                else {
                    progress("start-session"); ok = backend->start();
                    if (!ok || !allowed()) { if (!m_cancel) fail("start-session", backend->error()); }
                    else {
                        progress("waiting-readiness"); backend->beginReadiness();
                        const uint64_t readinessStart = roomMonotonicMs();
                        WindowsReadinessResult result(WindowsReadinessState::Pending);
                        while (allowed()) {
                            result = backend->pollReadiness();
                            if (result.state != WindowsReadinessState::Pending) break;
                            if (roomMonotonicMs() - readinessStart >= 10000) break;
                            std::unique_lock<std::mutex> lock(m_mutex);
                            m_wake.wait_for(lock, std::chrono::milliseconds(100), [this]() { return m_cancel.load(); });
                        }
                        if (allowed() && roomMonotonicMs() - readinessStart < 10000 && result.state == WindowsReadinessState::Ready) {
                            progress("in-room");
                            m_events.push(CliTunLifecycleEvent(CliTunLifecycleEvent::Ready, m_generation, "ready", "", backend->adapter()));
                            // Ready is a lifetime hand-off, not an ownership transfer.
                            std::unique_lock<std::mutex> lock(m_mutex);
                            m_wake.wait(lock, [this]() { return m_cancel.load(); });
                        } else if (!m_cancel) fail("waiting-readiness", result.error.empty() ? "Virtual network initialization timed out" : result.error);
                    }
                }
            }
        } catch (const std::exception& e) { fail("exception", e.what()); }
        catch (...) { fail("exception", "Unknown adapter lifecycle error"); }
        progress("cleaning-up");
        try { if (backend) backend->cleanup(); }
        catch (...) { LOG_ERR("[room-operation] task=%llu cleanup exception", static_cast<unsigned long long>(m_generation)); }
        backend.reset();
        LOG_INFO("[room-operation] task=%llu phase=cleaning-up event=end elapsed_ms=%llu",
            static_cast<unsigned long long>(m_generation), static_cast<unsigned long long>(roomMonotonicMs() - m_stageAt));
        m_events.push(CliTunLifecycleEvent(CliTunLifecycleEvent::Cleaned, m_generation, "idle"));
        m_done = true;
    }
    uint64_t m_generation;
    uint32_t m_ip;
    int m_mtu;
    std::shared_ptr<RoomTaskControl> m_control;
    Factory m_factory;
    std::atomic<bool> m_cancel, m_done;
    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    ThreadSafeQueue<CliTunLifecycleEvent> m_events;
    std::string m_lastStage;
    uint64_t m_stageAt = 0;
};
} // namespace VLan
#endif

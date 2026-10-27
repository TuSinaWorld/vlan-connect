#ifndef VLAN_GUI_TUN_LIFECYCLE_TASK_H
#define VLAN_GUI_TUN_LIFECYCLE_TASK_H

#include "tun_adapter.h"
#include "room_operation.h"
#include <memory>
#include <mutex>
#include <thread>
#include <condition_variable>

namespace VLan {

// The lifecycle owner is distinct from TunAdapter's packet-read QThread.
// Only the owner calls native configuration, shutdown and delete. The GUI
// borrows the adapter after Ready and must detach it before requestCancel().
class GuiTunLifecycleBackend {
public:
    virtual ~GuiTunLifecycleBackend() {}
    virtual bool initialize(QString* error) = 0;
    virtual bool configure(uint32_t ip, uint32_t mask, int mtu, QString* error) = 0;
    virtual bool start(QString* error) = 0;
    virtual void beginReadiness() = 0;
    virtual WindowsReadinessResult readiness() = 0;
    virtual TunAdapter* adapter() const = 0;
    virtual QString runtimeError() const { return QString(); }
    virtual void cleanup() = 0;
};

class NativeGuiTunLifecycleBackend : public GuiTunLifecycleBackend {
public:
    explicit NativeGuiTunLifecycleBackend(const std::shared_ptr<RoomTaskControl>& control)
        : m_control(control) {}
    bool initialize(QString* error) override {
        m_adapter.reset(new TunAdapter());
        m_adapter->setSetupControl(m_control);
        QObject::connect(m_adapter.get(), &TunAdapter::errorOccurred, m_adapter.get(),
            [this](const QString& message) {
                std::lock_guard<std::mutex> lock(m_errorMutex);
                if (m_runtimeError.isEmpty()) m_runtimeError = message;
            }, Qt::DirectConnection);
        if (m_adapter->initialize()) return true;
        *error = QStringLiteral("Virtual adapter initialization failed."); return false;
    }
    bool configure(uint32_t ip, uint32_t mask, int mtu, QString* error) override {
        if (m_adapter->configureIP(ip, mask, mtu)) return true;
        *error = m_adapter->configurationError(); return false;
    }
    bool start(QString* error) override {
        if (m_adapter->startSession()) return true;
        *error = QStringLiteral("Virtual adapter session could not start."); return false;
    }
    void beginReadiness() override { m_adapter->beginReadiness(); }
    WindowsReadinessResult readiness() override {
        const QString failure = runtimeError();
        if (!failure.isEmpty()) {
            WindowsReadinessResult result(WindowsReadinessState::Failed);
            result.error = failure.toStdString(); return result;
        }
        return m_adapter->pollReadiness();
    }
    QString runtimeError() const override {
        std::lock_guard<std::mutex> lock(m_errorMutex); return m_runtimeError;
    }
    TunAdapter* adapter() const override { return m_adapter.get(); }
    void cleanup() override {
        if (m_adapter) { m_adapter->setSetupControl(std::shared_ptr<RoomTaskControl>()); m_adapter->shutdown(); m_adapter.reset(); }
    }
private:
    std::shared_ptr<RoomTaskControl> m_control;
    std::unique_ptr<TunAdapter> m_adapter;
    mutable std::mutex m_errorMutex;
    QString m_runtimeError;
};

class GuiTunLifecycleTask {
public:
    struct Snapshot {
        bool ready, done, failed;
        QString phase, error;
        TunAdapter* adapter;
        Snapshot() : ready(false), done(false), failed(false), adapter(nullptr) {}
    };
    GuiTunLifecycleTask(const std::shared_ptr<RoomTaskControl>& control,
                        uint32_t ip, uint32_t mask, int mtu,
                        const std::shared_ptr<GuiTunLifecycleBackend>& backend = std::shared_ptr<GuiTunLifecycleBackend>())
        : m_state(new State), m_control(control) {
        std::shared_ptr<State> state = m_state;
        std::shared_ptr<GuiTunLifecycleBackend> owner = backend;
        std::thread([state, control, owner, ip, mask, mtu]() mutable {
            QString error;
            bool ok = true;
            const auto phase = [&](const char* name) {
                { std::lock_guard<std::mutex> lock(state->mutex); state->value.phase = QString::fromLatin1(name); }
                control->report(name);
            };
            const auto allowed = [&]() { return control->allowed(); };
            try {
            if (!owner) owner.reset(new NativeGuiTunLifecycleBackend(control));
            phase("adapter-create");
            if (!allowed() || !owner->initialize(&error) || !allowed()) ok = false;
            control->report("adapter-create-end");
            if (ok) { phase("network-configure"); if (!allowed() || !owner->configure(ip, mask, mtu, &error) || !allowed()) ok = false; }
            control->report("network-configure-end");
            if (ok) { phase("session-start"); if (!allowed() || !owner->start(&error) || !allowed()) ok = false; }
            control->report("session-start-end");
            if (ok) {
                phase("waiting-readiness");
                owner->beginReadiness();
                const uint64_t readinessDeadline = roomMonotonicMs() + 10000u;
                for (;;) {
                    if (!allowed()) { ok = false; break; }
                    WindowsReadinessResult result = owner->readiness();
                    if (!allowed()) { ok = false; break; }
                    if (roomMonotonicMs() >= readinessDeadline) {
                        error = QStringLiteral("Virtual adapter readiness timed out."); ok = false; break;
                    }
                    if (result.state == WindowsReadinessState::Ready) break;
                    if (result.state == WindowsReadinessState::Failed || roomMonotonicMs() >= readinessDeadline) {
                        error = QString::fromUtf8(result.error.c_str()); ok = false; break;
                    }
                    std::unique_lock<std::mutex> lock(state->mutex);
                    state->wake.wait_for(lock, std::chrono::milliseconds(100));
                }
            }
            if (ok && allowed()) {
                phase("ready");
                { std::lock_guard<std::mutex> lock(state->mutex); state->value.ready = true; state->value.adapter = owner->adapter(); }
                // The local-setup deadline stops applying after Ready. Only
                // cancellation ends a running room, never the setup clock.
                control->deadlineMs.store(0);
                while (!control->cancelled()) {
                    const QString failure = owner->runtimeError();
                    std::unique_lock<std::mutex> lock(state->mutex);
                    if (!failure.isEmpty()) {
                        // Keep the adapter alive until the GUI has detached
                        // every data-plane reference and requested cancel.
                        state->value.failed = true; state->value.error = failure;
                    }
                    state->wake.wait_for(lock, std::chrono::milliseconds(100));
                }
            } else {
                if (error.isEmpty()) error = control->cancelled()
                    ? QStringLiteral("Room setup cancelled.") : QStringLiteral("Local room setup timed out.");
                std::lock_guard<std::mutex> lock(state->mutex);
                state->value.failed = true; state->value.error = error;
            }
            } catch (const std::exception& exception) {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->value.failed = true; state->value.error = QString::fromUtf8(exception.what());
            } catch (...) {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->value.failed = true; state->value.error = QStringLiteral("Unexpected native adapter lifecycle failure.");
            }
            {
                std::unique_lock<std::mutex> lock(state->mutex);
                // Exceptions after publication must never free an adapter
                // borrowed by the GUI. Failed is observed there first; its
                // detach-then-cancel handshake authorizes destruction.
                if (state->value.ready)
                    state->wake.wait(lock, [&]() { return control->cancelled(); });
            }
            try { phase("cleanup"); } catch (...) {}
            try { if (owner) owner->cleanup(); } catch (...) {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->value.failed = true; state->value.error = QStringLiteral("Adapter cleanup failed.");
            }
            owner.reset();
            try { control->report("cleanup-end"); } catch (...) {}
            { std::lock_guard<std::mutex> lock(state->mutex); state->value.ready = false; state->value.adapter = nullptr; state->value.done = true; state->value.phase = QStringLiteral("cleaned"); }
        }).detach();
    }
    Snapshot snapshot() const { std::lock_guard<std::mutex> lock(m_state->mutex); return m_state->value; }
    void requestCancel() { m_control->cancelRequested.store(true); m_state->wake.notify_all(); }
    ~GuiTunLifecycleTask() { requestCancel(); }
private:
    struct State { std::mutex mutex; std::condition_variable wake; Snapshot value; };
    std::shared_ptr<State> m_state;
    std::shared_ptr<RoomTaskControl> m_control;
};

} // namespace VLan
#endif

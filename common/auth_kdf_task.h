#ifndef VLAN_AUTH_KDF_TASK_H
#define VLAN_AUTH_KDF_TASK_H

#include "payload_cipher.h"
#include <array>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace VLan {

// One worker and one replaceable queued request per client. Cancellation never
// waits for Argon2 and completed stale results never reach the network thread.
class AuthKdfTask {
public:
    struct Result {
        uint8_t hash[32];
        bool success;
        uint64_t elapsedMs;
        Result() : success(false), elapsedMs(0) { memset(hash, 0, sizeof(hash)); }
        ~Result() { crypto_wipe(hash, sizeof(hash)); }
    };
    AuthKdfTask() : m_stop(false), m_queued(false), m_ready(false), m_generation(0) {}
    ~AuthKdfTask() {
        { std::lock_guard<std::mutex> lock(m_mutex); m_stop = true; cancelLocked(); }
        m_wake.notify_one();
        if (m_worker.joinable()) m_worker.join();
    }
    void cancel() { std::lock_guard<std::mutex> lock(m_mutex); cancelLocked(); }
    void start(const uint8_t* password, size_t length, const uint8_t salt[16]) {
        std::lock_guard<std::mutex> lock(m_mutex);
        cancelLocked();
        if (length) m_password.assign(password, password + length);
        memcpy(m_salt.data(), salt, 16);
        m_queued = true;
        if (!m_worker.joinable()) m_worker = std::thread(&AuthKdfTask::run, this);
        m_wake.notify_one();
    }
    bool poll(Result& out) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_ready) return false;
        memcpy(out.hash, m_result.hash, 32);
        out.success = m_result.success;
        out.elapsedMs = m_result.elapsedMs;
        crypto_wipe(m_result.hash, 32);
        m_ready = false;
        return true;
    }
private:
    void cancelLocked() {
        ++m_generation;
        if (!m_password.empty()) crypto_wipe(m_password.data(), m_password.size());
        m_password.clear();
        crypto_wipe(m_salt.data(), m_salt.size());
        crypto_wipe(m_result.hash, 32);
        m_queued = m_ready = false;
    }
    void run() {
        std::unique_lock<std::mutex> lock(m_mutex);
        for (;;) {
            m_wake.wait(lock, [this]() { return m_stop || m_queued; });
            if (m_stop) return;
            const uint64_t generation = m_generation;
            std::vector<uint8_t> password;
            password.swap(m_password);
            const std::array<uint8_t, 16> salt = m_salt;
            m_queued = false;
            lock.unlock();
            Result result;
            const auto started = std::chrono::steady_clock::now();
            result.success = hashPasswordSalted(password.data(), password.size(), salt.data(), result.hash);
            result.elapsedMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count());
            if (!password.empty()) crypto_wipe(password.data(), password.size());
            lock.lock();
            if (!m_stop && generation == m_generation) {
                memcpy(m_result.hash, result.hash, 32);
                m_result.success = result.success;
                m_result.elapsedMs = result.elapsedMs;
                m_ready = true;
            }
        }
    }
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::thread m_worker;
    bool m_stop, m_queued, m_ready;
    uint64_t m_generation;
    std::vector<uint8_t> m_password;
    std::array<uint8_t, 16> m_salt;
    Result m_result;
    AuthKdfTask(const AuthKdfTask&) = delete;
    AuthKdfTask& operator=(const AuthKdfTask&) = delete;
};
}
#endif

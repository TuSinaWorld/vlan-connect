#ifndef VLAN_TEST_FAKE_WINTUN_API_H
#define VLAN_TEST_FAKE_WINTUN_API_H

#include "../../client/src/core/wintun_api.h"
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace VLanTest {

class FakeWintunApi : public VLan::WintunApi {
public:
    FakeWintunApi()
        : m_event(nullptr), m_readEventAvailable(true),
          m_packetPending(false),
          m_packetOutstanding(false), m_loadCount(0),
          m_unloadCount(0), m_endSessionCount(0),
          m_closeAdapterCount(0), m_releaseCount(0),
          m_sendCount(0), m_unsafeReleaseCount(0) {}

    ~FakeWintunApi() override {
        if (m_event)
            CloseHandle(m_event);
    }

    bool load(QString*) override {
        ++m_loadCount;
        if (!m_event)
            m_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        return m_event != nullptr;
    }

    void unload() override {
        ++m_unloadCount;
        noteUnsafeRelease();
        if (m_event) {
            CloseHandle(m_event);
            m_event = nullptr;
        }
    }

    WINTUN_ADAPTER_HANDLE createAdapter(
        LPCWSTR, LPCWSTR, const GUID*) override {
        return reinterpret_cast<WINTUN_ADAPTER_HANDLE>(1);
    }

    void closeAdapter(WINTUN_ADAPTER_HANDLE) override {
        noteUnsafeRelease();
        ++m_closeAdapterCount;
    }

    WINTUN_SESSION_HANDLE startSession(
        WINTUN_ADAPTER_HANDLE, DWORD) override {
        return reinterpret_cast<WINTUN_SESSION_HANDLE>(2);
    }

    void endSession(WINTUN_SESSION_HANDLE) override {
        noteUnsafeRelease();
        ++m_endSessionCount;
    }

    HANDLE getReadWaitEvent(WINTUN_SESSION_HANDLE) override {
        return m_readEventAvailable ? m_event : nullptr;
    }

    BYTE* receivePacket(
        WINTUN_SESSION_HANDLE, DWORD* packetSize) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_packetPending || m_packetOutstanding)
            return nullptr;
        m_packetPending = false;
        m_packetOutstanding = true;
        if (packetSize)
            *packetSize = static_cast<DWORD>(m_packet.size());
        return m_packet.empty() ? nullptr : m_packet.data();
    }

    void releaseReceivePacket(
        WINTUN_SESSION_HANDLE, const BYTE* packet) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_packetOutstanding ||
            (!m_packet.empty() && packet != m_packet.data())) {
            ++m_unsafeReleaseCount;
            return;
        }
        m_packetOutstanding = false;
        ++m_releaseCount;
    }

    BYTE* allocateSendPacket(
        WINTUN_SESSION_HANDLE, DWORD packetSize) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_sendBuffer.assign(packetSize, 0);
        return m_sendBuffer.empty() ? nullptr : m_sendBuffer.data();
    }

    void sendPacket(
        WINTUN_SESSION_HANDLE, const BYTE*) override {
        ++m_sendCount;
    }

    void injectPacket(const std::vector<uint8_t>& packet) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_packet.assign(packet.begin(), packet.end());
            m_packetPending = !m_packet.empty();
        }
        if (m_event)
            SetEvent(m_event);
    }

    void setReadEventAvailable(bool available) {
        m_readEventAvailable = available;
    }

    int loadCount() const { return m_loadCount.load(); }
    int unloadCount() const { return m_unloadCount.load(); }
    int endSessionCount() const { return m_endSessionCount.load(); }
    int closeAdapterCount() const { return m_closeAdapterCount.load(); }
    int releaseCount() const { return m_releaseCount.load(); }
    int sendCount() const { return m_sendCount.load(); }
    int unsafeReleaseCount() const {
        return m_unsafeReleaseCount.load();
    }

private:
    void noteUnsafeRelease() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_packetOutstanding)
            ++m_unsafeReleaseCount;
    }

    HANDLE m_event;
    bool m_readEventAvailable;
    mutable std::mutex m_mutex;
    std::vector<BYTE> m_packet;
    std::vector<BYTE> m_sendBuffer;
    bool m_packetPending;
    bool m_packetOutstanding;
    std::atomic<int> m_loadCount;
    std::atomic<int> m_unloadCount;
    std::atomic<int> m_endSessionCount;
    std::atomic<int> m_closeAdapterCount;
    std::atomic<int> m_releaseCount;
    std::atomic<int> m_sendCount;
    std::atomic<int> m_unsafeReleaseCount;
};

} // namespace VLanTest

#endif // VLAN_TEST_FAKE_WINTUN_API_H

#ifndef VLAN_WINTUN_API_H
#define VLAN_WINTUN_API_H

#include <QString>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>

typedef void* WINTUN_ADAPTER_HANDLE;
typedef void* WINTUN_SESSION_HANDLE;

namespace VLan {

class WintunApi {
public:
    virtual ~WintunApi() {}

    virtual bool load(QString* error) = 0;
    virtual void unload() = 0;
    virtual WINTUN_ADAPTER_HANDLE createAdapter(
        LPCWSTR name, LPCWSTR tunnelType, const GUID* requestedGuid) = 0;
    virtual bool getAdapterLuid(WINTUN_ADAPTER_HANDLE, NET_LUID*) { return false; }
    virtual void closeAdapter(WINTUN_ADAPTER_HANDLE adapter) = 0;
    virtual WINTUN_SESSION_HANDLE startSession(
        WINTUN_ADAPTER_HANDLE adapter, DWORD capacity) = 0;
    virtual void endSession(WINTUN_SESSION_HANDLE session) = 0;
    virtual HANDLE getReadWaitEvent(WINTUN_SESSION_HANDLE session) = 0;
    virtual BYTE* receivePacket(
        WINTUN_SESSION_HANDLE session, DWORD* packetSize) = 0;
    virtual void releaseReceivePacket(
        WINTUN_SESSION_HANDLE session, const BYTE* packet) = 0;
    virtual BYTE* allocateSendPacket(
        WINTUN_SESSION_HANDLE session, DWORD packetSize) = 0;
    virtual void sendPacket(
        WINTUN_SESSION_HANDLE session, const BYTE* packet) = 0;
};

class RealWintunApi : public WintunApi {
public:
    RealWintunApi()
        : m_dll(nullptr), m_create(nullptr), m_close(nullptr),
          m_startSession(nullptr), m_endSession(nullptr),
          m_getReadWaitEvent(nullptr), m_receivePacket(nullptr),
          m_releaseReceivePacket(nullptr), m_allocateSendPacket(nullptr),
          m_sendPacket(nullptr), m_getAdapterLuid(nullptr) {}

    ~RealWintunApi() override { unload(); }

    bool load(QString* error) override {
        if (m_dll) return true;
        m_dll = LoadLibraryW(L"wintun.dll");
        if (!m_dll) {
            if (error) *error = QStringLiteral("Failed to load wintun.dll");
            return false;
        }

        m_create = reinterpret_cast<FnCreateAdapter>(
            GetProcAddress(m_dll, "WintunCreateAdapter"));
        m_close = reinterpret_cast<FnCloseAdapter>(
            GetProcAddress(m_dll, "WintunCloseAdapter"));
        m_startSession = reinterpret_cast<FnStartSession>(
            GetProcAddress(m_dll, "WintunStartSession"));
        m_endSession = reinterpret_cast<FnEndSession>(
            GetProcAddress(m_dll, "WintunEndSession"));
        m_getReadWaitEvent = reinterpret_cast<FnGetReadWaitEvent>(
            GetProcAddress(m_dll, "WintunGetReadWaitEvent"));
        m_receivePacket = reinterpret_cast<FnReceivePacket>(
            GetProcAddress(m_dll, "WintunReceivePacket"));
        m_releaseReceivePacket = reinterpret_cast<FnReleaseReceivePacket>(
            GetProcAddress(m_dll, "WintunReleaseReceivePacket"));
        m_allocateSendPacket = reinterpret_cast<FnAllocateSendPacket>(
            GetProcAddress(m_dll, "WintunAllocateSendPacket"));
        m_sendPacket = reinterpret_cast<FnSendPacket>(
            GetProcAddress(m_dll, "WintunSendPacket"));
        m_getAdapterLuid = reinterpret_cast<FnGetAdapterLuid>(
            GetProcAddress(m_dll, "WintunGetAdapterLUID"));

        if (!m_create || !m_close || !m_startSession || !m_endSession ||
            !m_getReadWaitEvent || !m_receivePacket ||
            !m_releaseReceivePacket || !m_allocateSendPacket ||
            !m_sendPacket || !m_getAdapterLuid) {
            if (error) {
                *error = QStringLiteral(
                    "wintun.dll: missing exports (wrong version?)");
            }
            unload();
            return false;
        }
        return true;
    }

    void unload() override {
        if (m_dll) {
            FreeLibrary(m_dll);
            m_dll = nullptr;
        }
        m_create = nullptr;
        m_close = nullptr;
        m_startSession = nullptr;
        m_endSession = nullptr;
        m_getReadWaitEvent = nullptr;
        m_receivePacket = nullptr;
        m_releaseReceivePacket = nullptr;
        m_allocateSendPacket = nullptr;
        m_sendPacket = nullptr;
        m_getAdapterLuid = nullptr;
    }

    WINTUN_ADAPTER_HANDLE createAdapter(
        LPCWSTR name, LPCWSTR tunnelType,
        const GUID* requestedGuid) override {
        return m_create ? m_create(name, tunnelType, requestedGuid) : nullptr;
    }

    void closeAdapter(WINTUN_ADAPTER_HANDLE adapter) override {
        if (m_close) m_close(adapter);
    }

    bool getAdapterLuid(WINTUN_ADAPTER_HANDLE adapter, NET_LUID* luid) override {
        if (!m_getAdapterLuid || !luid) return false;
        m_getAdapterLuid(adapter, luid);
        return true;
    }

    WINTUN_SESSION_HANDLE startSession(
        WINTUN_ADAPTER_HANDLE adapter, DWORD capacity) override {
        return m_startSession ? m_startSession(adapter, capacity) : nullptr;
    }

    void endSession(WINTUN_SESSION_HANDLE session) override {
        if (m_endSession) m_endSession(session);
    }

    HANDLE getReadWaitEvent(WINTUN_SESSION_HANDLE session) override {
        return m_getReadWaitEvent
            ? m_getReadWaitEvent(session) : nullptr;
    }

    BYTE* receivePacket(
        WINTUN_SESSION_HANDLE session, DWORD* packetSize) override {
        return m_receivePacket
            ? m_receivePacket(session, packetSize) : nullptr;
    }

    void releaseReceivePacket(
        WINTUN_SESSION_HANDLE session, const BYTE* packet) override {
        if (m_releaseReceivePacket)
            m_releaseReceivePacket(session, packet);
    }

    BYTE* allocateSendPacket(
        WINTUN_SESSION_HANDLE session, DWORD packetSize) override {
        return m_allocateSendPacket
            ? m_allocateSendPacket(session, packetSize) : nullptr;
    }

    void sendPacket(
        WINTUN_SESSION_HANDLE session, const BYTE* packet) override {
        if (m_sendPacket) m_sendPacket(session, packet);
    }

private:
    typedef WINTUN_ADAPTER_HANDLE
        (WINAPI *FnCreateAdapter)(LPCWSTR, LPCWSTR, const GUID*);
    typedef void (WINAPI *FnCloseAdapter)(WINTUN_ADAPTER_HANDLE);
    typedef WINTUN_SESSION_HANDLE
        (WINAPI *FnStartSession)(WINTUN_ADAPTER_HANDLE, DWORD);
    typedef void (WINAPI *FnEndSession)(WINTUN_SESSION_HANDLE);
    typedef HANDLE
        (WINAPI *FnGetReadWaitEvent)(WINTUN_SESSION_HANDLE);
    typedef BYTE*
        (WINAPI *FnReceivePacket)(WINTUN_SESSION_HANDLE, DWORD*);
    typedef void
        (WINAPI *FnReleaseReceivePacket)(
            WINTUN_SESSION_HANDLE, const BYTE*);
    typedef BYTE*
        (WINAPI *FnAllocateSendPacket)(WINTUN_SESSION_HANDLE, DWORD);
    typedef void
        (WINAPI *FnSendPacket)(WINTUN_SESSION_HANDLE, const BYTE*);
    typedef void (WINAPI *FnGetAdapterLuid)(WINTUN_ADAPTER_HANDLE, NET_LUID*);

    HMODULE m_dll;
    FnCreateAdapter m_create;
    FnCloseAdapter m_close;
    FnStartSession m_startSession;
    FnEndSession m_endSession;
    FnGetReadWaitEvent m_getReadWaitEvent;
    FnReceivePacket m_receivePacket;
    FnReleaseReceivePacket m_releaseReceivePacket;
    FnAllocateSendPacket m_allocateSendPacket;
    FnSendPacket m_sendPacket;
    FnGetAdapterLuid m_getAdapterLuid;
};

} // namespace VLan

#endif // VLAN_WINTUN_API_H

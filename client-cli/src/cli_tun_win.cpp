#ifdef _WIN32

#include "cli_tun.h"
#include "cli_log.h"
#include <windows.h>
#include <cstring>
#include <string>

namespace VLan {

CliTunAdapter::CliTunAdapter()
    : m_running(false), m_ip(0), m_mask(0),
      m_dll(nullptr), m_adapter(nullptr), m_session(nullptr), m_readEvent(nullptr),
      m_fnCreate(nullptr), m_fnClose(nullptr),
      m_fnStartSession(nullptr), m_fnEndSession(nullptr),
      m_fnGetReadWaitEvent(nullptr), m_fnReceivePacket(nullptr),
      m_fnReleaseReceivePacket(nullptr), m_fnAllocateSendPacket(nullptr),
      m_fnSendPacket(nullptr), m_adapterLuid(0)
{}

CliTunAdapter::~CliTunAdapter() {
    shutdown();
}

bool CliTunAdapter::initialize(const std::string& adapterName) {
    if (m_setupControl && !m_setupControl->allowed()) return false;
    if (m_dll || m_adapter || m_session || m_readThread.joinable())
        return false;
    HMODULE dll = LoadLibraryW(L"wintun.dll");
    if (!dll) {
        LOG_ERR("Failed to load wintun.dll");
        return false;
    }
    m_dll = dll;

    m_fnCreate             = (FnCreateAdapter)       GetProcAddress(dll, "WintunCreateAdapter");
    m_fnClose              = (FnCloseAdapter)         GetProcAddress(dll, "WintunCloseAdapter");
    m_fnStartSession       = (FnStartSession)        GetProcAddress(dll, "WintunStartSession");
    m_fnEndSession         = (FnEndSession)           GetProcAddress(dll, "WintunEndSession");
    m_fnGetReadWaitEvent   = (FnGetReadWaitEvent)    GetProcAddress(dll, "WintunGetReadWaitEvent");
    m_fnReceivePacket      = (FnReceivePacket)       GetProcAddress(dll, "WintunReceivePacket");
    m_fnReleaseReceivePacket = (FnReleaseReceivePacket) GetProcAddress(dll, "WintunReleaseReceivePacket");
    m_fnAllocateSendPacket = (FnAllocateSendPacket)  GetProcAddress(dll, "WintunAllocateSendPacket");
    m_fnSendPacket         = (FnSendPacket)           GetProcAddress(dll, "WintunSendPacket");

    if (!m_fnCreate || !m_fnClose || !m_fnStartSession || !m_fnEndSession ||
        !m_fnGetReadWaitEvent || !m_fnReceivePacket || !m_fnReleaseReceivePacket ||
        !m_fnAllocateSendPacket || !m_fnSendPacket) {
        LOG_ERR("wintun.dll: missing exports (wrong version?)");
        FreeLibrary(dll);
        m_dll = nullptr;
        return false;
    }

    std::wstring wname = windowsWide(adapterName);
    if (wname.empty()) {
        LOG_ERR("Invalid UTF-8 virtual adapter name");
        FreeLibrary(dll);
        m_dll = nullptr;
        return false;
    }
    m_adapter = m_fnCreate(wname.c_str(), L"VLan", nullptr);
    if (!m_adapter) {
        LOG_ERR("WintunCreateAdapter failed (need admin?)");
        FreeLibrary(dll);
        m_dll = nullptr;
        return false;
    }
    typedef void (WINAPI *FnGetAdapterLuid)(void*, NET_LUID*);
    FnGetAdapterLuid getLuid = reinterpret_cast<FnGetAdapterLuid>(
        GetProcAddress(dll, "WintunGetAdapterLUID"));
    NET_LUID luid = {};
    wchar_t alias[IF_MAX_STRING_SIZE + 1] = {};
    if (!getLuid) {
        LOG_ERR("wintun.dll: WintunGetAdapterLUID is unavailable");
        shutdown();
        return false;
    }
    getLuid(m_adapter, &luid);
    m_adapterLuid = luid.Value;
    DWORD result = ConvertInterfaceLuidToAlias(&luid, alias, IF_MAX_STRING_SIZE + 1);
    if (result != NO_ERROR) {
        LOG_ERR("Cannot resolve virtual adapter alias (Windows %lu)", result);
        shutdown();
        return false;
    }
    m_networkState.alias = alias;
    return !m_setupControl || m_setupControl->allowed();
}

bool CliTunAdapter::configureIP(uint32_t ip, uint32_t mask, int mtu) {
    m_ip = ip;
    m_mask = mask;
    SystemWindowsNetworkCommands commands(m_setupControl);
    const bool ok = configureWindowsNetwork(commands, m_networkState, ip, mask, mtu,
        [](const std::string& message) { LOG_INFO("%s", message.c_str()); }, &m_configurationError);
    if (!ok) LOG_ERR("Virtual adapter configuration failed: %s", m_configurationError.c_str());
    return ok;
}

void CliTunAdapter::beginReadiness() {
    WindowsReadinessConfig config;
    config.luid = m_session && m_running.load() ? m_adapterLuid : 0;
    config.alias = m_networkState.alias;
    config.ip = m_ip; config.mask = m_mask;
    m_readiness.begin(&m_readinessBackend, config,
        [](const std::string& message) { LOG_INFO("%s", message.c_str()); });
}

WindowsReadinessResult CliTunAdapter::pollReadiness() { return m_readiness.poll(); }
void CliTunAdapter::cancelReadiness() { m_readiness.cancel(); }

bool CliTunAdapter::checkRoute(uint32_t destination, std::string* error) const {
    std::string detail;
    const bool ok = checkWindowsOverlayRoute(m_networkState.alias, m_ip, destination, &detail);
    if (error) *error = detail;
    if (ok) LOG_INFO("[network] route-check destination=%s alias=%s result=success",
                    ipToString(destination).c_str(), windowsUtf8(m_networkState.alias).c_str());
    else LOG_ERR("%s", detail.c_str());
    return ok;
}

bool CliTunAdapter::startSession() {
    if (m_setupControl && !m_setupControl->allowed()) return false;
    if (!m_adapter || m_session || m_readThread.joinable() ||
        !m_fnStartSession || !m_fnEndSession ||
        !m_fnGetReadWaitEvent) {
        return false;
    }
    m_session = m_fnStartSession(m_adapter, 0x400000);
    if (!m_session) {
        LOG_ERR("WintunStartSession failed");
        return false;
    }
    m_readEvent = m_fnGetReadWaitEvent(m_session);
    if (!m_readEvent) {
        LOG_ERR("WintunGetReadWaitEvent failed");
        m_fnEndSession(m_session);
        m_session = nullptr;
        return false;
    }
    m_running = true;
    m_readThread = std::thread(&CliTunAdapter::readLoop, this);
    return true;
}

void CliTunAdapter::shutdown() {
    cancelReadiness();
    m_running = false;
    if (m_readEvent) SetEvent((HANDLE)m_readEvent);
    if (m_readThread.joinable()) m_readThread.join();

    SystemWindowsNetworkCommands commands;
    rollbackWindowsNetwork(commands, m_networkState,
        [](const std::string& message) { LOG_INFO("%s", message.c_str()); });
    {
        std::lock_guard<std::mutex> lock(m_writeMutex);
        if (m_session && m_fnEndSession) {
            m_fnEndSession(m_session);
            m_session = nullptr;
        }
        m_readEvent = nullptr;
        if (m_adapter && m_fnClose) {
            m_fnClose(m_adapter);
            m_adapter = nullptr;
        }
        if (m_dll) { FreeLibrary((HMODULE)m_dll); m_dll = nullptr; }
    }


}

void CliTunAdapter::readLoop() {
    while (m_running) {
        DWORD waitResult = WaitForSingleObject((HANDLE)m_readEvent, 500);
        if (!m_running) break;
        if (waitResult == WAIT_TIMEOUT) continue;
        if (waitResult == WAIT_FAILED) {
            LOG_ERR("Wintun read wait failed (error %lu)", GetLastError());
            m_errorQueue.push("Wintun read wait failed");
            break;
        }
        if (waitResult != WAIT_OBJECT_0) {
            m_errorQueue.push("Wintun read session became invalid");
            break;
        }

        while (m_running) {
            DWORD packetSize = 0;
            BYTE* packet = m_fnReceivePacket(m_session, &packetSize);
            if (!packet) {
                const DWORD error = GetLastError();
                if (error != ERROR_NO_MORE_ITEMS) {
                    m_errorQueue.push("WintunReceivePacket failed");
                    m_running = false;
                }
                break;
            }
            Buffer received(packet, packet + packetSize);
            m_fnReleaseReceivePacket(m_session, packet);
            if (!m_running) break;
            m_recvQueue.push(received);
        }
    }
    m_running = false;
}

bool CliTunAdapter::writePacket(const uint8_t* data, size_t len) {
    std::lock_guard<std::mutex> lock(m_writeMutex);
    if (!m_running || !m_session || !m_fnAllocateSendPacket || !m_fnSendPacket)
        return false;

    BYTE* buf = m_fnAllocateSendPacket(m_session, static_cast<DWORD>(len));
    if (!buf) return false;
    memcpy(buf, data, len);
    m_fnSendPacket(m_session, buf);
    return true;
}

} // namespace VLan

#endif // _WIN32

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
      m_fnSendPacket(nullptr),
      m_firewallRuleActive(false), m_broadcastRouteActive(false)
{}

CliTunAdapter::~CliTunAdapter() {
    shutdown();
}

bool CliTunAdapter::initialize(const std::string& adapterName) {
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

    int wlen = MultiByteToWideChar(CP_UTF8, 0, adapterName.c_str(), -1, nullptr, 0);
    std::wstring wname(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, adapterName.c_str(), -1, &wname[0], wlen);

    m_adapter = m_fnCreate(wname.c_str(), L"VLan", nullptr);
    if (!m_adapter) {
        LOG_ERR("WintunCreateAdapter failed (need admin?)");
        FreeLibrary(dll);
        m_dll = nullptr;
        return false;
    }
    return true;
}

bool CliTunAdapter::configureIP(uint32_t ip, uint32_t mask, int mtu) {
    m_ip   = ip;
    m_mask = mask;

    std::string sIP   = ipToString(ip);
    std::string sMask = ipToString(mask);

    char cmd[512];
    snprintf(cmd, sizeof(cmd),
        "netsh interface ip set address name=\"VLan\" static %s %s",
        sIP.c_str(), sMask.c_str());
    LOG_INFO("Configuring IP: %s", cmd);
    int ret = system(cmd);
    if (ret != 0) {
        LOG_ERR("Failed to configure IP on virtual adapter");
        return false;
    }

    system("netsh interface ip set interface \"VLan\" metric=9999");

    if (mtu > 0) {
        snprintf(cmd, sizeof(cmd),
            "netsh interface ipv4 set subinterface \"VLan\" mtu=%d store=active", mtu);
        system(cmd);
    }

    system("netsh advfirewall firewall delete rule name=\"VLan Virtual LAN\" >nul 2>&1");
    ret = system("netsh advfirewall firewall add rule "
        "name=\"VLan Virtual LAN\" dir=in action=allow "
        "remoteip=10.10.0.0/24 enable=yes");
    if (ret == 0) {
        m_firewallRuleActive = true;
        LOG_INFO("Firewall rule added: allow inbound from 10.10.0.0/24");
    }

    system("netsh interface ipv4 delete route 255.255.255.255/32 \"VLan\" store=active >nul 2>&1");
    snprintf(cmd, sizeof(cmd),
        "netsh interface ipv4 add route 255.255.255.255/32 \"VLan\" %s metric=1 store=active",
        sIP.c_str());
    if (system(cmd) == 0)
        m_broadcastRouteActive = true;

    return true;
}

bool CliTunAdapter::startSession() {
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
    m_running = false;
    if (m_readEvent) SetEvent((HANDLE)m_readEvent);
    if (m_readThread.joinable()) m_readThread.join();

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

    if (m_broadcastRouteActive) {
        m_broadcastRouteActive = false;
        system("netsh interface ipv4 delete route 255.255.255.255/32 \"VLan\" store=active >nul 2>&1");
    }
    if (m_firewallRuleActive) {
        m_firewallRuleActive = false;
        system("netsh advfirewall firewall delete rule name=\"VLan Virtual LAN\" >nul 2>&1");
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

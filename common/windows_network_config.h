#ifndef VLAN_WINDOWS_NETWORK_CONFIG_H
#define VLAN_WINDOWS_NETWORK_CONFIG_H

// Shared by the Qt GUI and standalone CLI. The configuration state machine is
// platform independent so rollback and diagnostics can be tested without admin.
#include <stdint.h>
#include <string>
#include <vector>
#include <functional>
#include <sstream>
#include <algorithm>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <netfw.h>
#include <objbase.h>
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "iphlpapi.lib")
#endif

namespace VLan {

struct WindowsCommandResult {
    unsigned long exitCode;
    unsigned long systemError;
    unsigned long elapsedMs;
    bool timedOut;
    bool absent;
    std::string output;
    WindowsCommandResult() : exitCode(0), systemError(0), elapsedMs(0),
                             timedOut(false), absent(false) {}
    bool ok() const { return !timedOut && !systemError && exitCode == 0; }
};

class WindowsNetworkCommands {
public:
    virtual ~WindowsNetworkCommands() {}
    virtual WindowsCommandResult execute(const std::vector<std::wstring>& arguments,
                                         unsigned long timeoutMs) = 0;
    // An exact rule lookup distinguishes an absent rule from a failed deletion,
    // independently of the Windows display language.
    virtual WindowsCommandResult firewallRuleExists(const std::wstring& rule) = 0;
    virtual WindowsCommandResult broadcastRouteExists(const std::wstring& alias) = 0;
    virtual void retryDelay() = 0;
};

struct WindowsNetworkState {
    std::wstring alias;
    std::wstring firewallRule;
    bool firewallActive;
    bool broadcastActive;
    WindowsNetworkState() : firewallActive(false), broadcastActive(false) {}
};

inline std::wstring windowsAscii(const std::string& value) {
    return std::wstring(value.begin(), value.end());
}

inline std::string windowsIPv4(uint32_t ip) {
    std::ostringstream out;
    out << ((ip >> 24) & 255) << '.' << ((ip >> 16) & 255) << '.'
        << ((ip >> 8) & 255) << '.' << (ip & 255);
    return out.str();
}

inline std::string windowsUtf8(const std::wstring& value) {
#ifdef _WIN32
    if (value.empty()) return std::string();
    int size = WideCharToMultiByte(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    if (size) WideCharToMultiByte(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), &result[0], size, nullptr, nullptr);
    return result;
#else
    return std::string(value.begin(), value.end());
#endif
}

inline std::wstring windowsWide(const std::string& value) {
#ifdef _WIN32
    if (value.empty()) return std::wstring();
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                  static_cast<int>(value.size()), nullptr, 0);
    if (!size) return std::wstring();
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), &result[0], size);
    return result;
#else
    return std::wstring(value.begin(), value.end());
#endif
}

typedef std::function<void(const std::string&)> WindowsNetworkLogger;

inline std::string windowsLogValue(std::string value) {
    if (value.size() > 2048) value = value.substr(0, 2048) + " [truncated]";
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\r' || value[i] == '\n' || value[i] == '\t') value[i] = ' ';
        else if (value[i] == '"') value[i] = '\'';
        else if (static_cast<unsigned char>(value[i]) < 32) value[i] = '?';
    }
    return value;
}

inline void logWindowsNetwork(const WindowsNetworkLogger& log,
    const WindowsNetworkState& state, const char* operation, int attempt,
    const WindowsCommandResult& result) {
    if (!log) return;
    std::ostringstream out;
    out << "[network] operation=" << operation << " alias=\""
        << windowsLogValue(windowsUtf8(state.alias)) << "\" rule=\""
        << windowsLogValue(windowsUtf8(state.firewallRule)) << "\" attempt="
        << attempt << " result=" << (result.absent ? "absent" :
            result.timedOut ? "timeout" : result.ok() ? "success" : "failed")
        << " exit=" << result.exitCode << " system_error=" << result.systemError
        << " elapsed_ms=" << result.elapsedMs << " output=\""
        << windowsLogValue(result.output) << '"';
    log(out.str());
}

inline bool removeWindowsFirewall(WindowsNetworkCommands& commands,
    WindowsNetworkState& state, const WindowsNetworkLogger& log, const char* operation) {
    WindowsCommandResult found = commands.firewallRuleExists(state.firewallRule);
    if (found.absent || !found.ok()) {
        logWindowsNetwork(log, state, operation, 1, found);
        return found.absent;
    }
    WindowsCommandResult result = commands.execute({L"advfirewall", L"firewall",
        L"delete", L"rule", L"name=" + state.firewallRule}, 10000);
    logWindowsNetwork(log, state, operation, 1, result);
    return result.ok();
}

inline bool rollbackWindowsNetwork(WindowsNetworkCommands& commands,
    WindowsNetworkState& state, const WindowsNetworkLogger& log) {
    bool ok = true;
    if (state.broadcastActive) {
        WindowsCommandResult result = commands.broadcastRouteExists(state.alias);
        if (result.ok() && !result.absent)
            result = commands.execute({L"interface", L"ipv4", L"delete", L"route",
                L"255.255.255.255/32", state.alias, L"store=active"}, 10000);
        logWindowsNetwork(log, state, "broadcast-remove", 1, result);
        if (result.ok() || result.absent) state.broadcastActive = false;
        else ok = false;
    }
    if (state.firewallActive) {
        if (removeWindowsFirewall(commands, state, log, "firewall-remove"))
            state.firewallActive = false;
        else ok = false;
    }
    return ok;
}

inline bool configureWindowsNetwork(WindowsNetworkCommands& commands,
    WindowsNetworkState& state, uint32_t ip, uint32_t mask, int mtu,
    const WindowsNetworkLogger& log, std::string* error) {
    if (error) error->clear();
    state.firewallRule = L"VLan Virtual LAN [" + state.alias + L"]";
    const auto fail = [&](const std::string& message) {
        if (error) *error = message;
        rollbackWindowsNetwork(commands, state, log);
        return false;
    };
    if (state.alias.empty()) return fail("The virtual adapter alias is unavailable");
    unsigned prefix = 0;
    bool zeroSeen = false;
    for (int bit = 31; bit >= 0; --bit) {
        bool set = (mask & (static_cast<uint32_t>(1) << bit)) != 0;
        if (set && zeroSeen) return fail("The virtual subnet mask is invalid");
        if (set) ++prefix; else zeroSeen = true;
    }
    const auto run = [&](const char* operation, std::vector<std::wstring> args) {
        WindowsCommandResult result = commands.execute(args, 10000);
        logWindowsNetwork(log, state, operation, 1, result);
        if (!result.ok() && error) {
            std::ostringstream out;
            out << operation << " failed (exit=" << result.exitCode
                << ", Windows=" << result.systemError
                << (result.timedOut ? ", timeout" : "") << "): "
                << windowsLogValue(result.output);
            *error = out.str();
        }
        return result.ok();
    };
    if (!run("address-set", {L"interface", L"ipv4", L"set", L"address",
            L"name=" + state.alias, L"static", windowsAscii(windowsIPv4(ip)),
            windowsAscii(windowsIPv4(mask)), L"gateway=none", L"store=active"}))
        return fail(error ? *error : "Virtual adapter address configuration failed");
    if (!run("metric-set", {L"interface", L"ipv4", L"set", L"interface",
            L"interface=" + state.alias, L"metric=5", L"store=active"}))
        return fail(error ? *error : "Virtual adapter metric configuration failed");
    if (mtu > 0 && !run("mtu-set", {L"interface", L"ipv4", L"set", L"subinterface",
            state.alias, L"mtu=" + std::to_wstring(mtu), L"store=active"}))
        return fail(error ? *error : "Virtual adapter MTU configuration failed");
    if (!removeWindowsFirewall(commands, state, log, "firewall-precleanup"))
        return fail("Firewall precleanup failed; check the firewall service and administrator rights");
    const std::wstring subnet = windowsAscii(windowsIPv4(ip & mask) + "/" + std::to_string(prefix));
    const std::wstring localAddresses = windowsAscii(windowsIPv4(ip) + "," +
        windowsIPv4((ip & mask) | ~mask) + ",255.255.255.255");
    for (int attempt = 1; attempt <= 3; ++attempt) {
        WindowsCommandResult result = commands.execute({L"advfirewall", L"firewall",
            L"add", L"rule", L"name=" + state.firewallRule, L"dir=in",
            L"action=allow", L"localip=" + localAddresses,
            L"remoteip=" + subnet, L"profile=any", L"enable=yes"}, 10000);
        logWindowsNetwork(log, state, "firewall-add", attempt, result);
        if (result.ok()) { state.firewallActive = true; break; }
        // A timed out command may have committed before termination. Reconcile
        // our exact rule before retrying, and retain cleanup ownership on error.
        state.firewallActive = true;
        if (!removeWindowsFirewall(commands, state, log, "firewall-retry-cleanup"))
            return fail("Firewall add failed and its partial result could not be cleaned up");
        state.firewallActive = false;
        if (attempt < 3) commands.retryDelay();
    }
    if (!state.firewallActive)
        return fail("Firewall rule creation failed after 3 attempts; room join cancelled");
    // Address assignment can create a default /32 with a much higher metric.
    // Replace only this newly created virtual interface's limited broadcast
    // route; routes on physical/VPN interfaces remain untouched.
    WindowsCommandResult previousRoute = commands.broadcastRouteExists(state.alias);
    if (previousRoute.ok() && !previousRoute.absent)
        previousRoute = commands.execute({L"interface", L"ipv4", L"delete", L"route",
            L"255.255.255.255/32", state.alias, L"store=active"}, 10000);
    logWindowsNetwork(log, state, "broadcast-precleanup", 1, previousRoute);
    if (!previousRoute.ok() && !previousRoute.absent)
        return fail("Virtual broadcast route precleanup failed");
    state.broadcastActive = true; // include a potentially committed timeout in rollback
    if (!run("broadcast-add", {L"interface", L"ipv4", L"add", L"route",
            L"255.255.255.255/32", state.alias, L"0.0.0.0", L"metric=1", L"store=active"}))
        return fail(error ? *error : "Virtual broadcast route configuration failed");
    return true;
}

#ifdef _WIN32
inline std::wstring quoteWindowsArgument(const std::wstring& argument) {
    std::wstring result(1, L'"');
    size_t slashes = 0;
    for (size_t i = 0; i < argument.size(); ++i) {
        if (argument[i] == L'\\') { ++slashes; continue; }
        if (argument[i] == L'"') result.append(slashes * 2 + 1, L'\\');
        else result.append(slashes, L'\\');
        slashes = 0;
        result += argument[i];
    }
    result.append(slashes * 2, L'\\');
    result += L'"';
    return result;
}

class SystemWindowsNetworkCommands : public WindowsNetworkCommands {
public:
    WindowsCommandResult execute(const std::vector<std::wstring>& arguments,
                                 unsigned long timeoutMs) override {
        WindowsCommandResult result;
        const ULONGLONG started = GetTickCount64();
        wchar_t systemPath[MAX_PATH] = {};
        if (!GetSystemDirectoryW(systemPath, MAX_PATH)) {
            result.systemError = GetLastError(); return result;
        }
        std::wstring executable = std::wstring(systemPath) + L"\\netsh.exe";
        std::wstring command = quoteWindowsArgument(executable);
        for (size_t i = 0; i < arguments.size(); ++i)
            command += L" " + quoteWindowsArgument(arguments[i]);
        SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
        HANDLE readPipe = nullptr, writePipe = nullptr;
        if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) {
            result.systemError = GetLastError(); return result;
        }
        if (!SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0)) {
            result.systemError = GetLastError();
            CloseHandle(readPipe); CloseHandle(writePipe); return result;
        }
        HANDLE nullInput = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                      &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        STARTUPINFOW startup = {};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = nullInput;
        startup.hStdOutput = writePipe;
        startup.hStdError = writePipe;
        PROCESS_INFORMATION process = {};
        std::vector<wchar_t> commandBuffer(command.begin(), command.end());
        commandBuffer.push_back(0);
        BOOL created = CreateProcessW(executable.c_str(), commandBuffer.data(), nullptr,
            nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
        if (!created) result.systemError = GetLastError();
        CloseHandle(writePipe);
        if (nullInput != INVALID_HANDLE_VALUE) CloseHandle(nullInput);
        if (created) {
            bool finished = false;
            for (;;) {
                DWORD available = 0;
                unsigned drained = 0;
                while (drained++ < 16 && PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr) && available) {
                    char bytes[1024]; DWORD count = 0;
                    if (!ReadFile(readPipe, bytes, (std::min)(available, DWORD(sizeof(bytes))), &count, nullptr)) break;
                    if (result.output.size() < 8192)
                        result.output.append(bytes, (std::min)(size_t(count), 8192 - result.output.size()));
                }
                if (finished) break;
                DWORD wait = WaitForSingleObject(process.hProcess, 20);
                if (wait == WAIT_OBJECT_0) { finished = true; continue; }
                if (wait == WAIT_FAILED) {
                    result.systemError = GetLastError();
                    TerminateProcess(process.hProcess, result.systemError);
                    WaitForSingleObject(process.hProcess, 1000);
                    break;
                }
                if (GetTickCount64() - started >= timeoutMs) {
                    result.timedOut = true;
                    TerminateProcess(process.hProcess, ERROR_TIMEOUT);
                    WaitForSingleObject(process.hProcess, 1000);
                    finished = true;
                }
            }
            GetExitCodeProcess(process.hProcess, &result.exitCode);
            CloseHandle(process.hThread); CloseHandle(process.hProcess);
        }
        CloseHandle(readPipe);
        result.elapsedMs = static_cast<DWORD>(GetTickCount64() - started);
        // netsh emits OEM text when redirected. Keep diagnostics readable in
        // UTF-8 Qt and CLI log files on non-English Windows installations.
        if (!result.output.empty()) {
            int count = MultiByteToWideChar(CP_OEMCP, 0, result.output.data(),
                static_cast<int>(result.output.size()), nullptr, 0);
            std::wstring wide(count, L'\0');
            if (count) MultiByteToWideChar(CP_OEMCP, 0, result.output.data(),
                static_cast<int>(result.output.size()), &wide[0], count);
            result.output = windowsUtf8(wide);
        }
        return result;
    }

    WindowsCommandResult firewallRuleExists(const std::wstring& rule) override {
        WindowsCommandResult result;
        ULONGLONG started = GetTickCount64();
        HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool uninitialize = SUCCEEDED(init);
        INetFwPolicy2* policy = nullptr;
        INetFwRules* rules = nullptr;
        INetFwRule* found = nullptr;
        HRESULT hr = (SUCCEEDED(init) || init == RPC_E_CHANGED_MODE)
            ? CoCreateInstance(__uuidof(NetFwPolicy2), nullptr, CLSCTX_INPROC_SERVER,
                __uuidof(INetFwPolicy2), reinterpret_cast<void**>(&policy)) : init;
        if (SUCCEEDED(hr)) hr = policy->get_Rules(&rules);
        BSTR name = SysAllocStringLen(rule.data(), static_cast<UINT>(rule.size()));
        if (SUCCEEDED(hr)) hr = name ? rules->Item(name, &found) : E_OUTOFMEMORY;
        SysFreeString(name);
        if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ||
            hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) result.absent = true;
        else if (FAILED(hr)) result.systemError = static_cast<DWORD>(hr);
        if (found) found->Release();
        if (rules) rules->Release();
        if (policy) policy->Release();
        if (uninitialize) CoUninitialize();
        result.elapsedMs = static_cast<DWORD>(GetTickCount64() - started);
        result.output = result.absent ? "Exact firewall rule does not exist" :
            result.ok() ? "Exact firewall rule exists" : "Firewall rule lookup failed";
        return result;
    }
    WindowsCommandResult broadcastRouteExists(const std::wstring& alias) override {
        WindowsCommandResult result;
        const ULONGLONG started = GetTickCount64();
        MIB_IPFORWARD_ROW2 row;
        InitializeIpForwardEntry(&row);
        DWORD status = ConvertInterfaceAliasToLuid(alias.c_str(), &row.InterfaceLuid);
        if (status == NO_ERROR) {
            row.DestinationPrefix.Prefix.Ipv4.sin_family = AF_INET;
            row.DestinationPrefix.Prefix.Ipv4.sin_addr.s_addr = htonl(0xffffffffu);
            row.DestinationPrefix.PrefixLength = 32;
            row.NextHop.Ipv4.sin_family = AF_INET;
            row.NextHop.Ipv4.sin_addr.s_addr = 0;
            status = GetIpForwardEntry2(&row);
        }
        if (status == ERROR_NOT_FOUND || status == ERROR_FILE_NOT_FOUND) result.absent = true;
        else result.systemError = status;
        result.output = result.absent ? "Exact interface broadcast route does not exist" :
            result.ok() ? "Exact interface broadcast route exists" : "Broadcast route lookup failed";
        result.elapsedMs = static_cast<DWORD>(GetTickCount64() - started);
        return result;
    }
    void retryDelay() override { Sleep(250); }
};

inline bool checkWindowsOverlayRoute(const std::wstring& alias, uint32_t ip,
                                     uint32_t destination, std::string* error) {
    NET_LUID luid = {};
    DWORD result = ConvertInterfaceAliasToLuid(alias.c_str(), &luid);
    SOCKADDR_INET target = {};
    target.Ipv4.sin_family = AF_INET;
    target.Ipv4.sin_addr.s_addr = htonl(destination);
    MIB_IPFORWARD_ROW2 route = {};
    SOCKADDR_INET source = {};
    if (result == NO_ERROR) {
        for (int attempt = 0; attempt < 60; ++attempt) {
            result = GetBestRoute2(nullptr, 0, nullptr, &target, 0, &route, &source);
            if (result == NO_ERROR && route.InterfaceLuid.Value == luid.Value &&
                source.si_family == AF_INET && source.Ipv4.sin_addr.s_addr == htonl(ip)) {
                if (error) error->clear();
                return true;
            }
            Sleep(50);
        }
    }
    if (error) {
        std::ostringstream out;
        out << "Overlay route unavailable or conflicting: destination=" << windowsIPv4(destination)
            << " expected_alias=\"" << windowsUtf8(alias) << "\" selected_interface="
            << route.InterfaceIndex << " source=" << windowsIPv4(ntohl(source.Ipv4.sin_addr.s_addr))
            << " Windows=" << result << ". Check competing VPN/subnet routes.";
        *error = out.str();
    }
    return false;
}
#endif

} // namespace VLan
#endif

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
#include "room_operation.h"

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
    std::string outputEncoding;
    unsigned int outputCodePage;
    unsigned int oemCodePage;
    unsigned int ansiCodePage;
    unsigned int parentConsoleCodePage;
    size_t outputCapturedBytes;
    uint64_t outputObservedBytes;
    bool outputTruncated;
    std::string output;
    WindowsCommandResult() : exitCode(0), systemError(0), elapsedMs(0),
        timedOut(false), absent(false), outputEncoding("utf-8"),
        outputCodePage(65001), oemCodePage(0), ansiCodePage(0),
        parentConsoleCodePage(0), outputCapturedBytes(0),
        outputObservedBytes(0), outputTruncated(false) {}
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
    virtual bool shouldContinue() const { return true; }
    virtual unsigned long commandTimeout(unsigned long cap) const { return cap; }
    virtual void operationStarted(const char*) {}
    virtual void beginCleanup() {}
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

static const size_t WINDOWS_COMMAND_OUTPUT_LIMIT = 8192;
static const size_t WINDOWS_COMMAND_OUTPUT_LOOKAHEAD = 4;

inline void captureWindowsCommandOutput(const char* bytes, size_t length,
    std::string* captured, uint64_t* observedBytes) {
    if (!captured || !observedBytes || (!bytes && length)) return;
    *observedBytes += length;
    const size_t limit = WINDOWS_COMMAND_OUTPUT_LIMIT + WINDOWS_COMMAND_OUTPUT_LOOKAHEAD;
    if (captured->size() < limit && length)
        captured->append(bytes, (std::min)(length, limit - captured->size()));
}

// Positive: a complete canonical UTF-8 sequence; zero: incomplete final
// sequence; negative: invalid. Check available continuation bytes even when the
// last sequence is incomplete, so truncation cannot conceal an invalid prefix.
inline int windowsUtf8Sequence(const std::string& value, size_t offset) {
    if (offset >= value.size()) return 0;
    const unsigned char lead = static_cast<unsigned char>(value[offset]);
    const int length = lead < 0x80 ? 1 : lead >= 0xc2 && lead <= 0xdf ? 2 :
        lead >= 0xe0 && lead <= 0xef ? 3 : lead >= 0xf0 && lead <= 0xf4 ? 4 : -1;
    if (length < 0) return -1;
    for (int i = 1; i < length && offset + i < value.size(); ++i) {
        const unsigned char next = static_cast<unsigned char>(value[offset + i]);
        if (next < 0x80 || next > 0xbf) return -1;
        if (i == 1 && ((lead == 0xe0 && next < 0xa0) ||
            (lead == 0xed && next > 0x9f) || (lead == 0xf0 && next < 0x90) ||
            (lead == 0xf4 && next > 0x8f))) return -1;
    }
    return value.size() - offset < static_cast<size_t>(length) ? 0 : length;
}

inline bool windowsValidateUtf8(const std::string& value, bool allowIncompleteTail,
                                size_t* validBytes) {
    size_t offset = 0;
    while (offset < value.size()) {
        const int length = windowsUtf8Sequence(value, offset);
        if (length <= 0) {
            if (validBytes) *validBytes = offset;
            return length == 0 && allowIncompleteTail;
        }
        offset += static_cast<size_t>(length);
    }
    if (validBytes) *validBytes = offset;
    return true;
}

inline size_t windowsUtf8BoundedLength(const std::string& value, size_t limit) {
    size_t offset = 0;
    while (offset < value.size() && offset < limit) {
        const int length = windowsUtf8Sequence(value, offset);
        if (length <= 0 || static_cast<size_t>(length) > limit - offset) break;
        offset += static_cast<size_t>(length);
    }
    return offset;
}

inline std::string windowsEscapeOutput(const std::string& value, size_t limit,
                                      bool* truncated) {
    static const char hex[] = "0123456789abcdef";
    std::string result;
    size_t offset = 0;
    for (; offset < value.size(); ++offset) {
        const unsigned char byte = static_cast<unsigned char>(value[offset]);
        const size_t needed = byte >= 32 && byte < 127 ? 1 : 4;
        if (needed > limit - result.size()) break;
        if (needed == 1) result += static_cast<char>(byte);
        else {
            result += "\\x"; result += hex[byte >> 4]; result += hex[byte & 15];
        }
    }
    if (truncated) *truncated = offset < value.size();
    return result;
}

inline void windowsAppendUtf8(std::string& output, uint32_t scalar) {
    if (scalar < 0x80) output += static_cast<char>(scalar);
    else if (scalar < 0x800) {
        output += static_cast<char>(0xc0 | (scalar >> 6));
        output += static_cast<char>(0x80 | (scalar & 63));
    } else if (scalar < 0x10000) {
        output += static_cast<char>(0xe0 | (scalar >> 12));
        output += static_cast<char>(0x80 | ((scalar >> 6) & 63));
        output += static_cast<char>(0x80 | (scalar & 63));
    } else {
        output += static_cast<char>(0xf0 | (scalar >> 18));
        output += static_cast<char>(0x80 | ((scalar >> 12) & 63));
        output += static_cast<char>(0x80 | ((scalar >> 6) & 63));
        output += static_cast<char>(0x80 | (scalar & 63));
    }
}

inline bool windowsDecodeUtf16(const std::string& bytes, bool bigEndian,
                               bool allowIncompleteTail, std::string* output) {
    output->clear();
    size_t offset = 2; // caller has verified a UTF-16 BOM
    while (offset + 1 < bytes.size()) {
        const unsigned char a = static_cast<unsigned char>(bytes[offset]);
        const unsigned char b = static_cast<unsigned char>(bytes[offset + 1]);
        uint32_t scalar = bigEndian ? (a << 8) | b : a | (b << 8);
        offset += 2;
        if (scalar >= 0xd800 && scalar <= 0xdbff) {
            if (offset + 1 >= bytes.size()) return allowIncompleteTail;
            const unsigned char c = static_cast<unsigned char>(bytes[offset]);
            const unsigned char d = static_cast<unsigned char>(bytes[offset + 1]);
            const uint32_t low = bigEndian ? (c << 8) | d : c | (d << 8);
            if (low < 0xdc00 || low > 0xdfff) return false;
            scalar = 0x10000 + ((scalar - 0xd800) << 10) + low - 0xdc00;
            offset += 2;
        } else if (scalar >= 0xdc00 && scalar <= 0xdfff) return false;
        windowsAppendUtf8(*output, scalar);
    }
    return offset == bytes.size() || allowIncompleteTail;
}

#ifdef _WIN32
inline bool windowsDecodeCodePage(const std::string& bytes, unsigned int codePage,
                                  bool allowIncompleteTail, std::string* output) {
    if (!codePage || bytes.empty()) return false;
    const size_t attempts = allowIncompleteTail ? (std::min)(size_t(3), bytes.size() - 1) : 0;
    for (size_t trim = 0; trim <= attempts; ++trim) {
        const int length = static_cast<int>(bytes.size() - trim);
        const int count = MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS,
            bytes.data(), length, nullptr, 0);
        if (!count) continue;
        std::wstring wide(count, L'\0');
        if (!MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS, bytes.data(),
                length, &wide[0], count)) return false;
        *output = windowsUtf8(wide);
        return true;
    }
    return false;
}
#endif

// Encoding affects diagnostic text only. Never overwrite the process exit
// status or Win32 error merely because its redirected text cannot be decoded.
inline void decodeWindowsCommandOutput(const std::string& raw, uint64_t observedBytes,
    WindowsCommandResult* result, unsigned int oemCodePage = 0,
    unsigned int ansiCodePage = 0) {
    if (!result) return;
    const std::string bytes = raw.substr(0,
        WINDOWS_COMMAND_OUTPUT_LIMIT + WINDOWS_COMMAND_OUTPUT_LOOKAHEAD);
    result->outputCapturedBytes = bytes.size();
    result->outputObservedBytes = observedBytes;
    result->outputTruncated = result->outputTruncated || result->timedOut ||
        observedBytes > WINDOWS_COMMAND_OUTPUT_LIMIT || raw.size() > bytes.size();
    const bool incompleteTail = result->outputTruncated;
#ifdef _WIN32
    if (!oemCodePage) oemCodePage = GetOEMCP();
    if (!ansiCodePage) ansiCodePage = GetACP();
#endif
    result->oemCodePage = oemCodePage;
    result->ansiCodePage = ansiCodePage;
    result->output.clear(); result->outputCodePage = 0;
    result->outputEncoding = "none";
    if (bytes.empty()) return;
    const unsigned char first = static_cast<unsigned char>(bytes[0]);
    const unsigned char second = bytes.size() > 1 ? static_cast<unsigned char>(bytes[1]) : 0;
    const bool utf16le = first == 0xff && second == 0xfe;
    const bool utf16be = first == 0xfe && second == 0xff;
    if (utf16le || utf16be) {
        if (windowsDecodeUtf16(bytes, utf16be, incompleteTail, &result->output)) {
            result->outputEncoding = utf16be ? "utf-16be-bom" : "utf-16le-bom";
            result->outputCodePage = utf16be ? 1201 : 1200;
            return;
        }
    } else {
        const bool bom = bytes.size() >= 3 && first == 0xef && second == 0xbb &&
            static_cast<unsigned char>(bytes[2]) == 0xbf;
        const std::string utf8 = bom ? bytes.substr(3) : bytes;
        size_t validBytes = 0;
        if (windowsValidateUtf8(utf8, incompleteTail, &validBytes)) {
            result->output = utf8.substr(0, validBytes);
            result->outputEncoding = bom ? "utf-8-bom" : "utf-8";
            result->outputCodePage = 65001;
            return;
        }
#ifdef _WIN32
        // Legacy netsh contexts may still emit OEM/ANSI bytes. This fallback is
        // explicit in the log; an arbitrary byte stream has no unique encoding.
        if (!bom && windowsDecodeCodePage(bytes, oemCodePage, incompleteTail, &result->output)) {
            result->outputEncoding = "oem"; result->outputCodePage = oemCodePage; return;
        }
        if (!bom && ansiCodePage != oemCodePage &&
            windowsDecodeCodePage(bytes, ansiCodePage, incompleteTail, &result->output)) {
            result->outputEncoding = "acp"; result->outputCodePage = ansiCodePage; return;
        }
#endif
    }
    bool escapedTruncated = false;
    result->output = windowsEscapeOutput(bytes, WINDOWS_COMMAND_OUTPUT_LIMIT, &escapedTruncated);
    result->outputTruncated = result->outputTruncated || escapedTruncated;
    result->outputEncoding = "escaped-bytes"; result->outputCodePage = 0;
}

inline std::string windowsLogValue(std::string value) {
    size_t validBytes = 0;
    bool truncated = value.size() > 2048;
    if (!windowsValidateUtf8(value, false, &validBytes))
        value = windowsEscapeOutput(value, 2048, &truncated);
    else value.resize(windowsUtf8BoundedLength(value, 2048));
    if (truncated) value += " [truncated]";
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
        << " elapsed_ms=" << result.elapsedMs
        << " encoding=" << result.outputEncoding << " codepage=" << result.outputCodePage
        << " oem_cp=" << result.oemCodePage << " acp=" << result.ansiCodePage
        << " parent_console_cp=" << result.parentConsoleCodePage
        << " captured_bytes=" << result.outputCapturedBytes
        << " observed_bytes=" << result.outputObservedBytes
        << " truncated=" << ((result.outputTruncated || result.output.size() > 2048) ? "yes" : "no") << " output=\""
        << windowsLogValue(result.output) << '"';
    log(out.str());
}

inline bool removeWindowsFirewall(WindowsNetworkCommands& commands,
    WindowsNetworkState& state, const WindowsNetworkLogger& log, const char* operation) {
    commands.operationStarted(operation);
    if (!commands.shouldContinue()) return false;
    WindowsCommandResult found = commands.firewallRuleExists(state.firewallRule);
    if (found.absent || !found.ok()) {
        logWindowsNetwork(log, state, operation, 1, found);
        return found.absent;
    }
    WindowsCommandResult result = commands.execute({L"advfirewall", L"firewall",
        L"delete", L"rule", L"name=" + state.firewallRule}, commands.commandTimeout(10000));
    logWindowsNetwork(log, state, operation, 1, result);
    return result.ok();
}

inline bool rollbackWindowsNetwork(WindowsNetworkCommands& commands,
    WindowsNetworkState& state, const WindowsNetworkLogger& log) {
    commands.beginCleanup();
    bool ok = true;
    if (state.broadcastActive) {
        commands.operationStarted("broadcast-remove");
        WindowsCommandResult result = commands.broadcastRouteExists(state.alias);
        if (result.ok() && !result.absent)
            result = commands.execute({L"interface", L"ipv4", L"delete", L"route",
                L"255.255.255.255/32", state.alias, L"store=active"}, commands.commandTimeout(10000));
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
        commands.operationStarted(operation);
        if (!commands.shouldContinue()) {
            if (error) *error = "Virtual network preparation cancelled or timed out";
            return false;
        }
        WindowsCommandResult result = commands.execute(args, commands.commandTimeout(10000));
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
        commands.operationStarted("firewall-add");
        if (!commands.shouldContinue()) return fail("Virtual network preparation cancelled or timed out");
        WindowsCommandResult result = commands.execute({L"advfirewall", L"firewall",
            L"add", L"rule", L"name=" + state.firewallRule, L"dir=in",
            L"action=allow", L"localip=" + localAddresses,
            L"remoteip=" + subnet, L"profile=any", L"enable=yes"}, commands.commandTimeout(10000));
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
    commands.operationStarted("broadcast-precleanup");
    if (!commands.shouldContinue()) return fail("Virtual network preparation cancelled or timed out");
    WindowsCommandResult previousRoute = commands.broadcastRouteExists(state.alias);
    if (previousRoute.ok() && !previousRoute.absent)
        previousRoute = commands.execute({L"interface", L"ipv4", L"delete", L"route",
            L"255.255.255.255/32", state.alias, L"store=active"}, commands.commandTimeout(10000));
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
    explicit SystemWindowsNetworkCommands(std::shared_ptr<RoomTaskControl> control = std::shared_ptr<RoomTaskControl>())
        : m_control(control), m_cleanup(false) {}
    bool shouldContinue() const override { return m_cleanup || !m_control || m_control->allowed(); }
    unsigned long commandTimeout(unsigned long cap) const override {
        if (m_cleanup) return (std::min)(cap, 5000ul);
        return m_control ? m_control->remainingMs(cap) : cap;
    }
    void operationStarted(const char* operation) override { if (m_control) m_control->report(operation); }
    void beginCleanup() override { m_cleanup = true; }
    WindowsCommandResult execute(const std::vector<std::wstring>& arguments,
                                 unsigned long timeoutMs) override {
        WindowsCommandResult result;
        timeoutMs = commandTimeout(timeoutMs);
        if (!shouldContinue() || !timeoutMs) return interruptedResult();
        const ULONGLONG started = GetTickCount64();
        result.parentConsoleCodePage = GetConsoleOutputCP();
        std::string rawOutput;
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
                    if (!count) break;
                    captureWindowsCommandOutput(bytes, count, &rawOutput, &result.outputObservedBytes);
                }
                if (finished) {
                    // Continue draining a terminated child's buffered output.
                    // A descendant can retain the handle, so this final drain
                    // is bounded rather than waiting indefinitely for EOF.
                    DWORD remaining = 0;
                    if (PeekNamedPipe(readPipe, nullptr, 0, nullptr, &remaining, nullptr) && remaining) {
                        if (GetTickCount64() - started < static_cast<ULONGLONG>(timeoutMs) + 1000)
                            continue;
                        result.outputTruncated = true;
                    }
                    break;
                }
                DWORD wait = WaitForSingleObject(process.hProcess, 20);
                if (wait == WAIT_OBJECT_0) { finished = true; continue; }
                if (!shouldContinue()) {
                    result.systemError = m_control && m_control->cancelled() ? ERROR_CANCELLED : ERROR_TIMEOUT;
                    result.timedOut = result.systemError == ERROR_TIMEOUT;
                    TerminateProcess(process.hProcess, result.systemError);
                    WaitForSingleObject(process.hProcess, 1000);
                    finished = true;
                    continue;
                }
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
            if (!GetExitCodeProcess(process.hProcess, &result.exitCode) && !result.systemError)
                result.systemError = GetLastError();
            CloseHandle(process.hThread); CloseHandle(process.hProcess);
        }
        CloseHandle(readPipe);
        result.elapsedMs = static_cast<DWORD>(GetTickCount64() - started);
        // Redirected netsh output varies by context/Windows configuration.
        // Preserve UTF-8 first; never reinterpret it unconditionally as OEM.
        decodeWindowsCommandOutput(rawOutput, result.outputObservedBytes, &result);
        return result;
    }

    WindowsCommandResult firewallRuleExists(const std::wstring& rule) override {
        if (!shouldContinue()) return interruptedResult();
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
        if (!shouldContinue()) {
            WindowsCommandResult stopped = interruptedResult();
            stopped.elapsedMs = static_cast<DWORD>(GetTickCount64() - started);
            return stopped;
        }
        result.elapsedMs = static_cast<DWORD>(GetTickCount64() - started);
        result.output = result.absent ? "Exact firewall rule does not exist" :
            result.ok() ? "Exact firewall rule exists" : "Firewall rule lookup failed";
        return result;
    }
    WindowsCommandResult broadcastRouteExists(const std::wstring& alias) override {
        if (!shouldContinue()) return interruptedResult();
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
        if (!shouldContinue()) {
            WindowsCommandResult stopped = interruptedResult();
            stopped.elapsedMs = static_cast<DWORD>(GetTickCount64() - started);
            return stopped;
        }
        result.output = result.absent ? "Exact interface broadcast route does not exist" :
            result.ok() ? "Exact interface broadcast route exists" : "Broadcast route lookup failed";
        result.elapsedMs = static_cast<DWORD>(GetTickCount64() - started);
        return result;
    }
    void retryDelay() override {
        for (int i = 0; i < 10 && shouldContinue(); ++i) Sleep(25);
    }
private:
    WindowsCommandResult interruptedResult() const {
        WindowsCommandResult result;
        result.systemError = m_control && m_control->cancelled() ? ERROR_CANCELLED : ERROR_TIMEOUT;
        result.timedOut = result.systemError == ERROR_TIMEOUT;
        result.output = result.timedOut ? "Room preparation deadline expired" : "Room preparation cancelled";
        return result;
    }
    std::shared_ptr<RoomTaskControl> m_control;
    bool m_cleanup;
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

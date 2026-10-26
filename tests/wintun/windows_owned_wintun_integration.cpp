#ifdef _WIN32
#include "cli_tun.h"
#include "cli_log.h"
#include "windows_network_readiness.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

bool VLan::g_verboseLog = false;
using namespace VLan;

namespace {
bool elevated() {
    HANDLE token = nullptr; TOKEN_ELEVATION state = {}; DWORD bytes = 0;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    const BOOL ok = GetTokenInformation(token, TokenElevation, &state, sizeof(state), &bytes);
    CloseHandle(token); return ok && state.TokenIsElevated != 0;
}

bool chooseIsolatedSubnet(uint32_t* address) {
    MIB_IPFORWARD_TABLE2* table = nullptr;
    if (GetIpForwardTable2(AF_INET, &table) != NO_ERROR) return false;
    bool chosen = false;
    for (unsigned trial = 0; trial < 96 && !chosen; ++trial) {
        uint32_t subnet = trial < 64 ? (0x0afd0000u | ((64 + trial) << 8)) :
                                      (0xac1e0000u | ((160 + trial - 64) << 8));
        bool conflict = false;
        for (ULONG i = 0; i < table->NumEntries; ++i) {
            const MIB_IPFORWARD_ROW2& row = table->Table[i];
            const unsigned prefix = row.DestinationPrefix.PrefixLength;
            if (prefix == 0) continue; // a normal Internet default route is expected
            const unsigned common = (std::min)(prefix, 24u);
            const uint32_t mask = common ? (0xffffffffu << (32 - common)) : 0;
            if ((subnet & mask) == (ntohl(row.DestinationPrefix.Prefix.Ipv4.sin_addr.s_addr) & mask)) {
                conflict = true; break;
            }
        }
        if (!conflict) { *address = subnet | 2; chosen = true; }
    }
    FreeMibTable(table); return chosen;
}

bool captureProbe(CliTunAdapter& adapter, SOCKET socket, uint32_t source,
                  uint32_t destination, uint16_t port, const std::string& marker) {
    const sockaddr_in target = makeAddr(destination, port);
    if (sendto(socket, marker.data(), static_cast<int>(marker.size()), 0,
               reinterpret_cast<const sockaddr*>(&target), sizeof(target)) != static_cast<int>(marker.size())) {
        LOG_ERR("[owned-test] UDP send failed destination=%s error=%d", windowsIPv4(destination).c_str(), WSAGetLastError());
        return false;
    }
    const ULONGLONG started = GetTickCount64();
    while (GetTickCount64() - started < 2000) {
        for (const Buffer& packet : adapter.recvQueue().popAll()) {
            if (packet.size() < 28 || (packet[0] >> 4) != 4 || packet[9] != 17) continue;
            const size_t ipHeader = (packet[0] & 15) * 4;
            if (ipHeader < 20 || packet.size() < ipHeader + 8 + marker.size()) continue;
            if (extractDstIP(packet.data(), packet.size()) != destination ||
                readU32BE(packet.data() + 12) != source) continue;
            const uint16_t destinationPort = (uint16_t(packet[ipHeader + 2]) << 8) | packet[ipHeader + 3];
            if (destinationPort != port || memcmp(packet.data() + ipHeader + 8, marker.data(), marker.size()) != 0) continue;
            LOG_INFO("[owned-test] outbound UDP captured source=%s destination=%s bytes=%u",
                     windowsIPv4(source).c_str(), windowsIPv4(destination).c_str(), static_cast<unsigned>(packet.size()));
            return true;
        }
        if (!adapter.errorQueue().empty()) return false;
        Sleep(10);
    }
    LOG_ERR("[owned-test] UDP capture timed out destination=%s", windowsIPv4(destination).c_str());
    return false;
}
}

// Deliberately not an automatic CTest: it creates only its own unique Wintun
// adapter and scoped firewall rule, and requires an explicit elevated opt-in.
int wmain(int argc, wchar_t** argv) {
    bool optIn = false; std::wstring dllPath; std::string logPath;
    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], L"--run-owned-wintun") == 0) optIn = true;
        else if (wcscmp(argv[i], L"--wintun") == 0 && i + 1 < argc) dllPath = argv[++i];
        else if (wcscmp(argv[i], L"--log-file") == 0 && i + 1 < argc) logPath = windowsUtf8(argv[++i]);
        else { fprintf(stderr, "Unknown or incomplete argument\n"); return 2; }
    }
    if (!optIn || dllPath.empty() || logPath.empty()) {
        fprintf(stderr, "Manual use: --run-owned-wintun --wintun ABSOLUTE_DLL --log-file ABSOLUTE_LOG\n");
        return 77;
    }
    if (!CliLog::instance().setLogFile(logPath)) return 3;
    if (!elevated()) { LOG_ERR("[owned-test] Administrator elevation is required"); return 4; }
    if (dllPath.size() < 3 || !(dllPath[1] == L':' || (dllPath[0] == L'\\' && dllPath[1] == L'\\'))) {
        LOG_ERR("[owned-test] Absolute official wintun.dll path required"); return 5;
    }
    HMODULE dll = LoadLibraryExW(dllPath.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dll) { LOG_ERR("[owned-test] Wintun preload failed Windows=%lu", GetLastError()); return 6; }
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) { FreeLibrary(dll); return 7; }
    int result = 1;
    {
        uint32_t ip = 0;
        const std::string alias = "VLanTest-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64());
        NET_LUID existing = {};
        if (ConvertInterfaceAliasToLuid(windowsWide(alias).c_str(), &existing) == NO_ERROR) {
            LOG_ERR("[owned-test] Refusing to reuse an existing adapter alias");
        } else if (!chooseIsolatedSubnet(&ip)) {
            LOG_ERR("[owned-test] No unconflicted isolated subnet; no adapter was created");
        } else {
            CliTunAdapter adapter;
            const ULONGLONG started = GetTickCount64();
            LOG_INFO("[owned-test] BEGIN alias=%s ip=%s/24", alias.c_str(), windowsIPv4(ip).c_str());
            if (adapter.initialize(alias) && adapter.configureIP(ip, 0xffffff00u, 1280) && adapter.startSession()) {
                adapter.beginReadiness();
                WindowsReadinessResult readiness;
                do { readiness = adapter.pollReadiness(); if (readiness.state == WindowsReadinessState::Pending) Sleep(20); }
                while (readiness.state == WindowsReadinessState::Pending);
                if (readiness.state == WindowsReadinessState::Ready) {
                    SOCKET socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
                    int broadcast = 1;
                    const sockaddr_in local = makeAddr(ip, 0);
                    if (socket != INVALID_SOCKET && setsockopt(socket, SOL_SOCKET, SO_BROADCAST,
                        reinterpret_cast<const char*>(&broadcast), sizeof(broadcast)) == 0 &&
                        bind(socket, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) == 0) {
                        adapter.recvQueue().popAll();
                        const uint16_t port = static_cast<uint16_t>(49000 + GetCurrentProcessId() % 10000);
                        const std::string marker = "VLan-owned-readiness-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64());
                        const uint32_t targets[] = {ip + 1, (ip & 0xffffff00u) | 255, 0xffffffffu};
                        bool captured = true;
                        for (int i = 0; i < 3; ++i)
                            captured = captureProbe(adapter, socket, ip, targets[i], port, marker + "-" + std::to_string(i)) && captured;
                        if (captured) result = 0;
                    } else LOG_ERR("[owned-test] Cannot create/bind broadcast probe socket Windows=%d", WSAGetLastError());
                    if (socket != INVALID_SOCKET) closesocket(socket);
                } else LOG_ERR("[owned-test] Readiness failed: %s", readiness.error.c_str());
            } else LOG_ERR("[owned-test] Adapter setup failed: %s", adapter.configurationError().c_str());
            adapter.shutdown();
            NET_LUID after = {};
            DWORD aliasResult = NO_ERROR;
            const ULONGLONG cleanupStarted = GetTickCount64();
            do {
                aliasResult = ConvertInterfaceAliasToLuid(windowsWide(alias).c_str(), &after);
                if (aliasResult != NO_ERROR) break;
                Sleep(50);
            } while (GetTickCount64() - cleanupStarted < 2000);
            SystemWindowsNetworkCommands commands;
            const WindowsCommandResult firewall = commands.firewallRuleExists(L"VLan Virtual LAN [" + windowsWide(alias) + L"]");
            if (aliasResult == NO_ERROR || !firewall.absent) {
                result = 8;
                LOG_ERR("[owned-test] CLEANUP INCOMPLETE adapter_lookup=%lu firewall_absent=%u firewall_query_error=%lu",
                        aliasResult, firewall.absent ? 1 : 0, firewall.systemError);
            }
            LOG_INFO("[owned-test] END result=%d elapsed_ms=%llu alias=%s", result,
                     static_cast<unsigned long long>(GetTickCount64() - started), alias.c_str());
        }
    }
    WSACleanup(); FreeLibrary(dll); return result;
}
#else
int main() { return 77; }
#endif

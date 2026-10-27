#ifndef VLAN_WINDOWS_NETWORK_READINESS_H
#define VLAN_WINDOWS_NETWORK_READINESS_H

#include "windows_network_config.h"
#include <array>
#include <cstdint>

namespace VLan {

enum class WindowsReadinessState { Pending, Ready, Failed };

struct WindowsReadinessResult {
    WindowsReadinessState state;
    std::string error;
    std::string diagnostic;
    WindowsReadinessResult(WindowsReadinessState value = WindowsReadinessState::Pending)
        : state(value) {}
};

struct WindowsReadinessConfig {
    uint64_t luid;
    std::wstring alias;
    uint32_t ip;
    uint32_t mask;
    WindowsReadinessConfig() : luid(0), ip(0), mask(0) {}
};

struct WindowsReadinessRoute {
    uint32_t error;
    uint64_t luid;
    uint32_t index;
    uint32_t prefix;
    uint8_t prefixLength;
    uint32_t nextHop;
    uint32_t metric;
    WindowsReadinessRoute() : error(0), luid(0), index(0), prefix(0),
        prefixLength(0), nextHop(0), metric(0) {}
};

struct WindowsReadinessSelection {
    uint32_t destination;
    uint32_t interfaceError;
    uint32_t routeError;
    uint32_t selectedIndex;
    uint32_t source;
    uint32_t interfaceMetric;
    uint32_t metricError;
    WindowsReadinessRoute route;
    WindowsReadinessSelection() : destination(0), interfaceError(0), routeError(0),
        selectedIndex(0), source(0), interfaceMetric(0), metricError(0) {}
};

struct WindowsReadinessSnapshot {
    uint32_t interfaceError;
    uint32_t addressError;
    uint32_t ipInterfaceError;
    uint32_t index;
    unsigned adminStatus;
    unsigned operStatus;
    unsigned mediaState;
    bool notMediaConnected;
    bool paused;
    bool ipv4Connected;
    uint32_t address;
    uint8_t prefixLength;
    unsigned dadState;
    bool skipAsSource;
    uint32_t validLifetime;
    WindowsReadinessRoute subnet;
    WindowsReadinessRoute directedBroadcast;
    WindowsReadinessRoute limitedBroadcast;
    std::array<WindowsReadinessSelection, 3> selections;
    WindowsReadinessSnapshot() : interfaceError(0), addressError(0), ipInterfaceError(0),
        index(0), adminStatus(0), operStatus(0), mediaState(0), notMediaConnected(false),
        paused(false), ipv4Connected(false), address(0), prefixLength(0),
        dadState(0), skipAsSource(false), validLifetime(0) {}
};

class WindowsReadinessBackend {
public:
    virtual ~WindowsReadinessBackend() {}
    virtual uint64_t nowMs() const = 0;
    virtual WindowsReadinessSnapshot sample(const WindowsReadinessConfig& config) = 0;
};

inline bool windowsReadinessPrefix(uint32_t mask, uint8_t* prefix) {
    uint8_t count = 0;
    bool zero = false;
    for (int bit = 31; bit >= 0; --bit) {
        if (mask & (uint32_t(1) << bit)) {
            if (zero) return false;
            ++count;
        } else zero = true;
    }
    if (prefix) *prefix = count;
    return true;
}

inline uint32_t windowsReadinessUnicast(const WindowsReadinessConfig& config) {
    // Choose a different usable host. This queries routing only and never
    // assumes that a peer exists or responds to ICMP/ARP.
    const uint32_t network = config.ip & config.mask;
    uint32_t candidate = network + 2;
    if (candidate == config.ip) ++candidate;
    return candidate;
}

inline std::string windowsReadinessError(uint32_t code) {
    std::string result = std::to_string(code);
#ifdef _WIN32
    wchar_t* message = nullptr;
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
        reinterpret_cast<wchar_t*>(&message), 0, nullptr);
    if (length && message) {
        result += " (" + windowsLogValue(windowsUtf8(std::wstring(message, length))) + ")";
        LocalFree(message);
    }
#endif
    return result;
}

inline std::string describeWindowsReadiness(const WindowsReadinessConfig& config,
                                           const WindowsReadinessSnapshot& value) {
    std::ostringstream out;
    out << "alias=\"" << windowsLogValue(windowsUtf8(config.alias)) << "\" expected_luid="
        << config.luid << " expected_ip=" << windowsIPv4(config.ip);
    if (value.interfaceError) out << " interface_query_error=" << windowsReadinessError(value.interfaceError);
    else out << " if_index=" << value.index << " admin=" << value.adminStatus
        << " oper=" << value.operStatus << " media=" << value.mediaState
        << " not_media_connected=" << value.notMediaConnected << " paused=" << value.paused;
    if (value.addressError) out << " address_query_error=" << windowsReadinessError(value.addressError);
    else out << " address=" << windowsIPv4(value.address) << '/' << unsigned(value.prefixLength)
        << " dad=" << value.dadState << " skip_as_source=" << value.skipAsSource
        << " valid_lifetime=" << value.validLifetime;
    if (value.ipInterfaceError) out << " ipv4_query_error=" << windowsReadinessError(value.ipInterfaceError);
    else out << " ipv4_connected=" << value.ipv4Connected;
    const WindowsReadinessRoute* rows[] = {&value.subnet, &value.directedBroadcast, &value.limitedBroadcast};
    const char* names[] = {"subnet", "directed_broadcast", "limited_broadcast"};
    for (int i = 0; i < 3; ++i) {
        out << ' ' << names[i] << "={";
        if (rows[i]->error) out << "query_error=" << windowsReadinessError(rows[i]->error);
        else out << "luid=" << rows[i]->luid << " prefix=" << windowsIPv4(rows[i]->prefix)
            << '/' << unsigned(rows[i]->prefixLength) << " next_hop=" << windowsIPv4(rows[i]->nextHop)
            << " route_metric=" << rows[i]->metric;
        out << '}';
    }
    for (const WindowsReadinessSelection& selection : value.selections) {
        out << " selection={destination=" << windowsIPv4(selection.destination);
        if (selection.interfaceError)
            out << " interface_query_error=" << windowsReadinessError(selection.interfaceError);
        else {
            out << " selected_if=" << selection.selectedIndex;
            if (selection.routeError) out << " route_query_error=" << windowsReadinessError(selection.routeError);
            else out << " selected_luid=" << selection.route.luid << " source=" << windowsIPv4(selection.source)
                << " prefix=" << windowsIPv4(selection.route.prefix) << '/' << unsigned(selection.route.prefixLength)
                << " next_hop=" << windowsIPv4(selection.route.nextHop) << " route_metric=" << selection.route.metric;
            if (selection.metricError) out << " metric_query_error=" << selection.metricError;
            else out << " interface_metric=" << selection.interfaceMetric;
        }
        out << '}';
    }
    return out.str();
}

class WindowsNetworkReadiness {
public:
    WindowsNetworkReadiness() : m_backend(nullptr), m_started(0), m_lastSample(0),
        m_sampled(false), m_active(false) {}

    void begin(WindowsReadinessBackend* backend, const WindowsReadinessConfig& config,
               const WindowsNetworkLogger& logger = WindowsNetworkLogger()) {
        m_backend = backend; m_config = config; m_logger = logger;
        m_started = backend ? backend->nowMs() : 0;
        m_lastSample = 0; m_sampled = false; m_active = true; m_signature.clear(); m_lastDetail.clear();
        m_result = WindowsReadinessResult();
        uint8_t prefix = 0;
        if (!backend || !config.luid || !config.ip ||
            !windowsReadinessPrefix(config.mask, &prefix) || prefix > 29 || prefix == 0 ||
            (config.ip & ~config.mask) == 0 || (config.ip | config.mask) == 0xffffffffu) {
            finish(WindowsReadinessState::Failed, "configuration-invalid",
                   "Virtual adapter readiness has an invalid identity, IPv4 address or subnet.", "");
        }
    }

    WindowsReadinessResult poll() {
        if (!m_active || !m_backend) return m_result;
        const uint64_t now = m_backend->nowMs();
        const uint64_t elapsed = now - m_started;
        if (m_sampled && now - m_lastSample < 100 && elapsed < 10000) return m_result;
        m_lastSample = now; m_sampled = true;
        const WindowsReadinessSnapshot snapshot = m_backend->sample(m_config);
        std::string stage, reason;
        bool fatal = false;
        uint8_t prefix = 0; windowsReadinessPrefix(m_config.mask, &prefix);
        if (!snapshot.addressError && snapshot.dadState == 2) {
            stage = "duplicate-address"; fatal = true;
            reason = "Windows detected a duplicate virtual IPv4 address; room startup was cancelled.";
        } else if (!snapshot.addressError && snapshot.skipAsSource) {
            stage = "source-address-disabled"; fatal = true;
            reason = "The virtual IPv4 address has SkipAsSource enabled and cannot be used for overlay traffic.";
        } else if (!snapshot.addressError && (snapshot.address != m_config.ip || snapshot.prefixLength != prefix)) {
            stage = "address-prefix-mismatch"; fatal = true;
            reason = "The configured virtual IPv4 address or subnet prefix does not match the room.";
        } else if (snapshot.interfaceError) {
            stage = "interface-query-failed";
            reason = "Cannot query virtual adapter status: Windows " + windowsReadinessError(snapshot.interfaceError);
        } else if (snapshot.adminStatus != 1 || snapshot.operStatus != 1 ||
                   snapshot.mediaState != 1 || snapshot.notMediaConnected || snapshot.paused) {
            stage = "waiting-link"; reason = "Waiting for the virtual adapter link to become ready.";
        } else if (snapshot.addressError) {
            stage = "address-query-failed";
            reason = "Cannot query the virtual IPv4 address: Windows " + windowsReadinessError(snapshot.addressError);
        } else if (snapshot.dadState != 4 || snapshot.validLifetime == 0) {
            stage = "waiting-address";
            reason = "Waiting for Windows IPv4 duplicate-address detection and source-address readiness.";
        } else if (snapshot.ipInterfaceError || !snapshot.ipv4Connected) {
            stage = "waiting-ipv4"; reason = snapshot.ipInterfaceError
                ? "Cannot query the virtual IPv4 interface: Windows " + windowsReadinessError(snapshot.ipInterfaceError)
                : "Waiting for the virtual IPv4 interface to become connected.";
        } else {
            const WindowsReadinessRoute* rows[] = {&snapshot.subnet, &snapshot.directedBroadcast, &snapshot.limitedBroadcast};
            const uint32_t targets[] = {m_config.ip & m_config.mask, m_config.ip | ~m_config.mask, 0xffffffffu};
            const char* names[] = {"subnet-route", "directed-broadcast-route", "limited-broadcast-route"};
            for (int i = 0; i < 3 && stage.empty(); ++i) {
                if (rows[i]->error) {
                    stage = std::string(names[i]) + "-query-failed";
                    reason = std::string("Cannot query the virtual ") + names[i] + ": Windows " + windowsReadinessError(rows[i]->error);
                } else if (rows[i]->luid != m_config.luid || rows[i]->prefix != targets[i] ||
                           rows[i]->prefixLength != (i == 0 ? prefix : 32) || rows[i]->nextHop != 0) {
                    stage = std::string(names[i]) + "-invalid";
                    reason = std::string("The virtual ") + names[i] + " is missing or is not on-link on the expected adapter.";
                }
            }
            for (const WindowsReadinessSelection& selection : snapshot.selections) {
                if (!stage.empty()) break;
                const std::string target = windowsIPv4(selection.destination);
                if (selection.interfaceError || selection.routeError) {
                    stage = "route-selection-query-failed";
                    reason = "Windows route/source query failed for " + target + ": " +
                        windowsReadinessError(selection.interfaceError ? selection.interfaceError : selection.routeError);
                } else if (selection.route.luid != m_config.luid) {
                    stage = "route-conflict";
                    reason = "A competing interface wins the route to " + target + " (interface " +
                        std::to_string(selection.selectedIndex) + ", prefix " + windowsIPv4(selection.route.prefix) +
                        "/" + std::to_string(selection.route.prefixLength) + "); check overlapping VPN/subnet routes.";
                } else if (selection.source != m_config.ip) {
                    stage = "source-selection-mismatch";
                    reason = "Windows selected source " + windowsIPv4(selection.source) + " instead of " +
                        windowsIPv4(m_config.ip) + " for " + target + ".";
                }
            }
        }
        const std::string detail = describeWindowsReadiness(m_config, snapshot);
        m_lastDetail = detail;
        if (stage.empty()) {
            // A final sample started exactly at the deadline is allowed. A
            // delayed event-loop callback must not extend the startup budget
            // by accepting readiness first observed after that deadline.
            if (elapsed > 10000)
                return finish(WindowsReadinessState::Failed, "deadline-expired",
                    "Virtual adapter readiness was first observed after the startup deadline of 10 seconds.", detail);
            return finish(WindowsReadinessState::Ready, "ready", "", detail);
        }
        if (!fatal && elapsed >= 10000)
            reason = "Virtual adapter did not become ready within 10 seconds. " + reason;
        if (fatal || elapsed >= 10000)
            return finish(WindowsReadinessState::Failed, stage, reason, detail);
        publish(WindowsReadinessState::Pending, stage, reason, detail);
        return m_result;
    }

    void cancel() {
        if (m_active) finish(WindowsReadinessState::Failed, "cancelled", "Virtual adapter startup was cancelled.", m_lastDetail);
    }

private:
    WindowsReadinessResult finish(WindowsReadinessState state, const std::string& stage,
                                  const std::string& reason, const std::string& detail) {
        publish(state, stage, reason, detail); m_active = false; return m_result;
    }
    void publish(WindowsReadinessState state, const std::string& stage,
                 const std::string& reason, const std::string& detail) {
        const char* name = state == WindowsReadinessState::Ready ? "ready" :
            state == WindowsReadinessState::Failed ? "failed" : "pending";
        const std::string signature = std::string(name) + '|' + stage + '|' + detail;
        m_result.state = state;
        m_result.error = state == WindowsReadinessState::Failed ? reason : std::string();
        std::ostringstream out;
        out << "[network-readiness] state=" << name << " stage=" << stage << " elapsed_ms="
            << (m_backend ? m_backend->nowMs() - m_started : 0) << ' ' << detail;
        if (!reason.empty()) out << " reason=\"" << windowsLogValue(reason) << '"';
        m_result.diagnostic = out.str();
        if (m_logger && signature != m_signature) m_logger(m_result.diagnostic);
        m_signature = signature;
    }
    WindowsReadinessBackend* m_backend;
    WindowsReadinessConfig m_config;
    WindowsNetworkLogger m_logger;
    uint64_t m_started, m_lastSample;
    bool m_sampled, m_active;
    std::string m_signature;
    std::string m_lastDetail;
    WindowsReadinessResult m_result;
};

#ifdef _WIN32
class NativeWindowsReadinessBackend : public WindowsReadinessBackend {
public:
    uint64_t nowMs() const override { return GetTickCount64(); }

    static WindowsReadinessRoute routeValue(const MIB_IPFORWARD_ROW2& row) {
        WindowsReadinessRoute value;
        value.luid = row.InterfaceLuid.Value; value.index = row.InterfaceIndex;
        value.prefix = ntohl(row.DestinationPrefix.Prefix.Ipv4.sin_addr.s_addr);
        value.prefixLength = row.DestinationPrefix.PrefixLength;
        value.nextHop = ntohl(row.NextHop.Ipv4.sin_addr.s_addr); value.metric = row.Metric;
        return value;
    }
    static WindowsReadinessRoute exactRoute(uint64_t luid, uint32_t address, uint8_t prefix) {
        MIB_IPFORWARD_ROW2 row; InitializeIpForwardEntry(&row);
        row.InterfaceLuid.Value = luid;
        row.DestinationPrefix.Prefix.Ipv4.sin_family = AF_INET;
        row.DestinationPrefix.Prefix.Ipv4.sin_addr.s_addr = htonl(address);
        row.DestinationPrefix.PrefixLength = prefix;
        row.NextHop.Ipv4.sin_family = AF_INET;
        row.NextHop.Ipv4.sin_addr.s_addr = 0;
        const DWORD error = GetIpForwardEntry2(&row);
        WindowsReadinessRoute value = error ? WindowsReadinessRoute() : routeValue(row);
        value.error = error; return value;
    }
    static WindowsReadinessSelection selectRoute(uint32_t destination) {
        WindowsReadinessSelection value; value.destination = destination;
        SOCKADDR_INET target = {}; target.Ipv4.sin_family = AF_INET;
        target.Ipv4.sin_addr.s_addr = htonl(destination);
        DWORD index = 0;
        value.interfaceError = GetBestInterfaceEx(reinterpret_cast<sockaddr*>(&target), &index);
        if (value.interfaceError) return value;
        if (index == 0) { value.interfaceError = ERROR_INVALID_DATA; return value; }
        value.selectedIndex = index;
        MIB_IPFORWARD_ROW2 route = {}; SOCKADDR_INET source = {};
        value.routeError = GetBestRoute2(nullptr, index, nullptr, &target, 0, &route, &source);
        if (value.routeError) return value;
        value.route = routeValue(route);
        if (source.si_family != AF_INET) { value.routeError = ERROR_INVALID_DATA; return value; }
        value.source = ntohl(source.Ipv4.sin_addr.s_addr);
        MIB_IPINTERFACE_ROW ipInterface; InitializeIpInterfaceEntry(&ipInterface);
        ipInterface.Family = AF_INET; ipInterface.InterfaceLuid = route.InterfaceLuid;
        value.metricError = GetIpInterfaceEntry(&ipInterface);
        if (!value.metricError) value.interfaceMetric = ipInterface.Metric;
        return value;
    }

    WindowsReadinessSnapshot sample(const WindowsReadinessConfig& config) override {
        WindowsReadinessSnapshot value;
        MIB_IF_ROW2 interfaceRow = {}; interfaceRow.InterfaceLuid.Value = config.luid;
        value.interfaceError = GetIfEntry2(&interfaceRow);
        if (!value.interfaceError) {
            value.index = interfaceRow.InterfaceIndex; value.adminStatus = interfaceRow.AdminStatus;
            value.operStatus = interfaceRow.OperStatus; value.mediaState = interfaceRow.MediaConnectState;
            value.notMediaConnected = interfaceRow.InterfaceAndOperStatusFlags.NotMediaConnected != 0;
            value.paused = interfaceRow.InterfaceAndOperStatusFlags.Paused != 0;
        }
        MIB_UNICASTIPADDRESS_ROW address; InitializeUnicastIpAddressEntry(&address);
        address.InterfaceLuid.Value = config.luid; address.Address.Ipv4.sin_family = AF_INET;
        address.Address.Ipv4.sin_addr.s_addr = htonl(config.ip);
        value.addressError = GetUnicastIpAddressEntry(&address);
        if (!value.addressError) {
            value.address = ntohl(address.Address.Ipv4.sin_addr.s_addr); value.prefixLength = address.OnLinkPrefixLength;
            value.dadState = address.DadState; value.skipAsSource = address.SkipAsSource != 0;
            value.validLifetime = address.ValidLifetime;
        }
        MIB_IPINTERFACE_ROW ipInterface; InitializeIpInterfaceEntry(&ipInterface);
        ipInterface.Family = AF_INET; ipInterface.InterfaceLuid.Value = config.luid;
        value.ipInterfaceError = GetIpInterfaceEntry(&ipInterface);
        if (!value.ipInterfaceError) value.ipv4Connected = ipInterface.Connected != 0;
        uint8_t prefix = 0; windowsReadinessPrefix(config.mask, &prefix);
        value.subnet = exactRoute(config.luid, config.ip & config.mask, prefix);
        value.directedBroadcast = exactRoute(config.luid, config.ip | ~config.mask, 32);
        value.limitedBroadcast = exactRoute(config.luid, 0xffffffffu, 32);
        const uint32_t targets[] = {windowsReadinessUnicast(config), config.ip | ~config.mask, 0xffffffffu};
        for (int i = 0; i < 3; ++i) value.selections[i] = selectRoute(targets[i]);
        return value;
    }
};
#endif

} // namespace VLan
#endif

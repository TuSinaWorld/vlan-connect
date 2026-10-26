#include "windows_network_readiness.h"
#include <cassert>
#include <iostream>

using namespace VLan;

static WindowsReadinessConfig config() {
    WindowsReadinessConfig value; value.luid = 77; value.alias = L"Owned test adapter";
    value.ip = 0x0a0a0002; value.mask = 0xffffff00; return value;
}

static WindowsReadinessRoute row(uint32_t address, uint8_t prefix) {
    WindowsReadinessRoute value; value.luid = 77; value.index = 12;
    value.prefix = address; value.prefixLength = prefix; value.metric = 1; return value;
}

static WindowsReadinessSnapshot ready() {
    WindowsReadinessSnapshot value; value.index = 12; value.adminStatus = 1;
    value.operStatus = 1; value.mediaState = 1; value.ipv4Connected = true;
    value.address = config().ip; value.prefixLength = 24;
    value.dadState = 4; value.validLifetime = 0xffffffffu;
    value.subnet = row(0x0a0a0000, 24);
    value.directedBroadcast = row(0x0a0a00ff, 32);
    value.limitedBroadcast = row(0xffffffffu, 32);
    const uint32_t targets[] = {0x0a0a0003, 0x0a0a00ff, 0xffffffffu};
    for (int i = 0; i < 3; ++i) {
        value.selections[i].destination = targets[i]; value.selections[i].source = config().ip;
        value.selections[i].selectedIndex = 12; value.selections[i].interfaceMetric = 5;
        value.selections[i].route = i == 0 ? value.subnet : i == 1 ? value.directedBroadcast : value.limitedBroadcast;
    }
    return value;
}

class Backend : public WindowsReadinessBackend {
public:
    uint64_t time; int samples; WindowsReadinessSnapshot value;
    Backend() : time(50000), samples(0), value(ready()) {}
    uint64_t nowMs() const override { return time; }
    WindowsReadinessSnapshot sample(const WindowsReadinessConfig&) override { ++samples; return value; }
};

static void testLinkDadDelayAndRateLimit() {
    Backend backend; WindowsNetworkReadiness check; std::vector<std::string> logs;
    backend.value.operStatus = 2; backend.value.mediaState = 2;
    backend.value.dadState = 1;
    check.begin(&backend, config(), [&](const std::string& text) { logs.push_back(text); });
    assert(check.poll().state == WindowsReadinessState::Pending);
    assert(logs.back().find("waiting-link") != std::string::npos);
    for (int i = 0; i < 100; ++i) check.poll();
    assert(backend.samples == 1 && logs.size() == 1);
    backend.time += 3500; backend.value.operStatus = 1; backend.value.mediaState = 1;
    assert(check.poll().state == WindowsReadinessState::Pending);
    assert(logs.back().find("waiting-address") != std::string::npos);
    backend.time += 2000; backend.value.dadState = 4;
    WindowsReadinessResult result = check.poll();
    assert(result.state == WindowsReadinessState::Ready && result.error.empty());
    assert(result.diagnostic.find("elapsed_ms=5500") != std::string::npos);
    assert(logs.back().find("dad=4") != std::string::npos);
    int samples = backend.samples; backend.time += 20000;
    assert(check.poll().state == WindowsReadinessState::Ready && samples == backend.samples);
}

static void testAddressFailures() {
    for (int kind = 0; kind < 3; ++kind) {
        Backend backend; WindowsNetworkReadiness check;
        if (kind == 0) backend.value.dadState = 2;
        if (kind == 1) backend.value.skipAsSource = true;
        if (kind == 2) backend.value.prefixLength = 32;
        check.begin(&backend, config());
        const WindowsReadinessResult result = check.poll();
        assert(result.state == WindowsReadinessState::Failed && !result.error.empty());
        assert(backend.time == 50000); // no blind wait for a duplicate/misconfiguration
    }
}

static void testQueryFailureIsNotConflictingInterfaceZero() {
    Backend backend; WindowsNetworkReadiness check;
    backend.value.selections[2].routeError = 1232;
    backend.value.selections[2].selectedIndex = 12;
    backend.value.selections[2].source = 0;
    backend.value.selections[2].route = WindowsReadinessRoute();
    check.begin(&backend, config());
    assert(check.poll().state == WindowsReadinessState::Pending);
    backend.time += 10000;
    const WindowsReadinessResult result = check.poll();
    assert(result.state == WindowsReadinessState::Failed);
    assert(result.error.find("query failed") != std::string::npos);
    assert(result.error.find("1232") != std::string::npos);
    assert(result.error.find("competing") == std::string::npos);
    const size_t broadcast = result.diagnostic.find("selection={destination=255.255.255.255");
    assert(broadcast != std::string::npos);
    const std::string failed = result.diagnostic.substr(broadcast);
    assert(failed.find("source=0.0.0.0") == std::string::npos);
    assert(failed.find("selected_luid=0") == std::string::npos);
}

static void testRouteConflictAndConvergence() {
    Backend backend; WindowsNetworkReadiness check;
    backend.value.selections[0].route.luid = 900; backend.value.selections[0].selectedIndex = 22;
    backend.value.selections[0].route.prefix = 0x0a0a0003;
    backend.value.selections[0].route.prefixLength = 32;
    check.begin(&backend, config());
    assert(check.poll().state == WindowsReadinessState::Pending);
    backend.time += 10000;
    WindowsReadinessResult result = check.poll();
    assert(result.state == WindowsReadinessState::Failed);
    assert(result.error.find("competing interface") != std::string::npos);
    assert(result.error.find("10.10.0.3/32") != std::string::npos);
    assert(result.error.find("22") != std::string::npos);
    check.begin(&backend, config());
    assert(check.poll().state == WindowsReadinessState::Pending);
    backend.time += 8500; backend.value = ready();
    assert(check.poll().state == WindowsReadinessState::Ready);
}

static void testRequiredRoutesSourcesAndDeadline() {
    for (int kind = 0; kind < 6; ++kind) {
        Backend backend; WindowsNetworkReadiness check;
        if (kind == 0) backend.value.subnet.error = 1168;
        if (kind == 1) backend.value.directedBroadcast.error = 1168;
        if (kind == 2) backend.value.limitedBroadcast.error = 1168;
        if (kind == 3) backend.value.selections[0].source = 0xc0a802c8;
        if (kind == 4) backend.value.addressError = 1168;
        if (kind == 5) backend.value.interfaceError = 5;
        check.begin(&backend, config());
        assert(check.poll().state == WindowsReadinessState::Pending);
        backend.time += 9999; assert(check.poll().state == WindowsReadinessState::Pending);
        backend.time += 1; const WindowsReadinessResult result = check.poll();
        assert(result.state == WindowsReadinessState::Failed && !result.error.empty());
    }
}

static void testCancellationAndNewAttempt() {
    Backend backend; WindowsNetworkReadiness check; int logs = 0;
    backend.value.dadState = 1;
    check.begin(&backend, config(), [&](const std::string&) { ++logs; });
    check.poll(); check.cancel(); const int afterCancel = logs;
    backend.value = ready(); backend.time += 4000;
    assert(check.poll().state == WindowsReadinessState::Failed);
    check.cancel(); assert(logs == afterCancel && backend.samples == 1);
    check.begin(&backend, config()); assert(check.poll().state == WindowsReadinessState::Ready);
    WindowsReadinessConfig bad = config(); bad.luid = 0;
    check.begin(&backend, bad); assert(check.poll().state == WindowsReadinessState::Failed);
}

static void testLateReadyCannotExtendDeadline() {
    for (int initialSample = 0; initialSample < 2; ++initialSample) {
        Backend backend; WindowsNetworkReadiness check;
        backend.value.dadState = 1;
        check.begin(&backend, config());
        if (initialSample) assert(check.poll().state == WindowsReadinessState::Pending);
        backend.time += 10100; backend.value = ready();
        const WindowsReadinessResult result = check.poll();
        assert(result.state == WindowsReadinessState::Failed);
        assert(result.error.find("10 seconds") != std::string::npos);
        assert(result.diagnostic.find("deadline-expired") != std::string::npos);
        assert(result.diagnostic.find("dad=4") != std::string::npos);
        const int samples = backend.samples;
        backend.time += 100;
        assert(check.poll().state == WindowsReadinessState::Failed && backend.samples == samples);
    }
    Backend boundary; WindowsNetworkReadiness check;
    boundary.value.dadState = 1;
    check.begin(&boundary, config());
    boundary.time += 9999;
    assert(check.poll().state == WindowsReadinessState::Pending);
    boundary.time += 1; boundary.value = ready();
    assert(check.poll().state == WindowsReadinessState::Ready);
    assert(boundary.samples == 2); // final deadline sample bypasses 100ms throttle
}

int main() {
    testLinkDadDelayAndRateLimit(); testAddressFailures();
    testQueryFailureIsNotConflictingInterfaceZero(); testRouteConflictAndConvergence();
    testRequiredRoutesSourcesAndDeadline(); testCancellationAndNewAttempt();
    testLateReadyCannotExtendDeadline();
    std::cout << "Windows readiness: delayed DAD, route/source validation, diagnostics, deadline and cancellation passed\n";
}

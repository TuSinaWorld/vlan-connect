#include "windows_network_config.h"
#include <cassert>
#include <iostream>

using namespace VLan;

class FakeCommands : public WindowsNetworkCommands {
public:
    std::vector<std::vector<std::wstring> > calls;
    std::string failOperation;
    int failures;
    int delays;
    bool timeout;
    bool firewall;
    bool broadcast;
    bool lookupFailure;
    FakeCommands() : failures(0), delays(0), timeout(false), firewall(false),
                     broadcast(true), lookupFailure(false) {}

    static std::string operation(const std::vector<std::wstring>& args) {
        if (args[0] == L"advfirewall") return args[2] == L"add" ? "firewall-add" : "firewall-remove";
        if (args[2] == L"set") return windowsUtf8(args[3]);
        return args[2] == L"add" ? "broadcast-add" : "broadcast-remove";
    }

    WindowsCommandResult execute(const std::vector<std::wstring>& args,
                                 unsigned long timeoutMs) override {
        assert(timeoutMs == 10000);
        calls.push_back(args);
        WindowsCommandResult result;
        result.elapsedMs = 17;
        std::string op = operation(args);
        if (op == failOperation && failures > 0) {
            --failures;
            result.exitCode = 5;
            result.output = "simulated denial\r\nsecond line";
            result.timedOut = timeout;
            // Simulate a command that committed just before timing out.
            if (timeout && op == "firewall-add") firewall = true;
            if (timeout && op == "broadcast-add") broadcast = true;
            return result;
        }
        if (op == "firewall-add") firewall = true;
        else if (op == "firewall-remove") firewall = false;
        else if (op == "broadcast-add") broadcast = true;
        else if (op == "broadcast-remove") broadcast = false;
        return result;
    }

    WindowsCommandResult firewallRuleExists(const std::wstring&) override {
        WindowsCommandResult result;
        result.systemError = lookupFailure ? 5 : 0;
        result.absent = !lookupFailure && !firewall;
        return result;
    }
    WindowsCommandResult broadcastRouteExists(const std::wstring&) override {
        WindowsCommandResult result;
        result.absent = !broadcast;
        return result;
    }
    void retryDelay() override { ++delays; }
};

static WindowsNetworkState state() {
    WindowsNetworkState value;
    value.alias = L"VLan Custom adapter";
    return value;
}

static void testSuccessAndScopedCommands() {
    FakeCommands commands;
    WindowsNetworkState value = state();
    std::vector<std::string> logs;
    std::string error;
    const WindowsNetworkLogger logger = [&](const std::string& line) { logs.push_back(line); };
    assert(configureWindowsNetwork(commands, value, 0x0a0a0002, 0xffffff00, 1280, logger, &error));
    assert(error.empty());
    assert(value.firewallActive && value.broadcastActive);
    assert(commands.firewall && commands.broadcast);
    bool metric = false, gateway = false, onLink = false, firewallScope = false;
    for (const auto& args : commands.calls) {
        for (const auto& arg : args) {
            assert(arg != L"metric=9999");
            if (arg == L"metric=5") metric = true;
            if (arg == L"gateway=none") gateway = true;
        }
        if (FakeCommands::operation(args) == "broadcast-add") {
            assert(args[5] == value.alias);
            assert(args[6] == L"0.0.0.0");
            onLink = true;
        }
        if (FakeCommands::operation(args) == "firewall-add") {
            assert(args[4] == L"name=VLan Virtual LAN [VLan Custom adapter]");
            assert(std::find(args.begin(), args.end(), L"remoteip=10.10.0.0/24") != args.end());
            assert(std::find(args.begin(), args.end(), L"localip=10.10.0.2,10.10.0.255,255.255.255.255") != args.end());
            firewallScope = true;
        }
    }
    assert(metric && gateway && onLink && firewallScope);
    assert(rollbackWindowsNetwork(commands, value, logger));
    assert(!value.firewallActive && !value.broadcastActive);
    assert(!commands.firewall && !commands.broadcast);
    size_t calls = commands.calls.size();
    assert(rollbackWindowsNetwork(commands, value, logger));
    assert(commands.calls.size() == calls);
    for (const auto& line : logs) {
        assert(line.find("operation=") != std::string::npos);
        assert(line.find(" alias=\"") != std::string::npos);
        assert(line.find(" rule=\"") != std::string::npos);
        assert(line.find(" attempt=") != std::string::npos);
        assert(line.find(" result=") != std::string::npos);
        assert(line.find(" exit=") != std::string::npos);
        assert(line.find(" elapsed_ms=") != std::string::npos);
        assert(line.find('\n') == std::string::npos);
    }
}

static void testCriticalStepsFailClosed() {
    const char* operations[] = {"address", "interface", "subinterface", "firewall-add", "broadcast-add"};
    for (const char* operation : operations) {
        FakeCommands commands;
        commands.failOperation = operation;
        commands.failures = 3;
        WindowsNetworkState value = state();
        std::string error;
        assert(!configureWindowsNetwork(commands, value, 0x0a0a0002, 0xffffff00, 1280,
                                         WindowsNetworkLogger(), &error));
        assert(!error.empty());
        assert(!value.firewallActive && !value.broadcastActive);
        assert(!commands.firewall);
    }
}

static void testRetryAndTimeoutReconciliation() {
    FakeCommands commands;
    commands.failOperation = "firewall-add";
    commands.failures = 2;
    commands.timeout = true;
    WindowsNetworkState value = state();
    std::vector<std::string> logs;
    const auto logger = [&](const std::string& line) { logs.push_back(line); };
    std::string error;
    assert(configureWindowsNetwork(commands, value, 0x0a0a0002, 0xffffff00, 1280, logger, &error));
    assert(commands.delays == 2);
    int timedOut = 0, retries = 0;
    for (const auto& line : logs) {
        if (line.find("operation=firewall-add") != std::string::npos &&
            line.find("result=timeout") != std::string::npos) ++timedOut;
        if (line.find("operation=firewall-retry-cleanup") != std::string::npos) ++retries;
    }
    assert(timedOut == 2 && retries == 2);
    assert(rollbackWindowsNetwork(commands, value, logger));

    FakeCommands route;
    route.failOperation = "broadcast-add";
    route.failures = 1;
    route.timeout = true;
    value = state();
    assert(!configureWindowsNetwork(route, value, 0x0a0a0002, 0xffffff00, 1280, logger, &error));
    assert(!route.firewall && !route.broadcast);
    assert(!value.firewallActive && !value.broadcastActive);
}

static void testAbsentVersusFailedCleanup() {
    FakeCommands commands;
    WindowsNetworkState value = state();
    value.firewallRule = L"VLan Virtual LAN [VLan Custom adapter]";
    value.firewallActive = true;
    value.broadcastActive = true;
    commands.broadcast = false;
    std::vector<std::string> logs;
    const auto logger = [&](const std::string& line) { logs.push_back(line); };
    assert(rollbackWindowsNetwork(commands, value, logger));
    assert(commands.calls.empty());
    assert(logs.size() == 2 && logs[0].find("result=absent") != std::string::npos &&
           logs[1].find("result=absent") != std::string::npos);

    commands.lookupFailure = true;
    value.firewallActive = true;
    assert(!rollbackWindowsNetwork(commands, value, logger));
    assert(value.firewallActive); // retain ownership so shutdown can retry
    assert(logs.back().find("result=failed") != std::string::npos);
    std::string error;
    assert(!configureWindowsNetwork(commands, value, 0x0a0a0002, 0xffffff00, 1280, logger, &error));
    assert(error.find("precleanup") != std::string::npos);
}

static void testInvalidMaskNoCommandsAndBoundedLog() {
    FakeCommands commands;
    WindowsNetworkState value = state();
    std::string error;
    assert(!configureWindowsNetwork(commands, value, 0x0a0a0002, 0xff00ff00, 1280,
                                     WindowsNetworkLogger(), &error));
    assert(commands.calls.empty());
    assert(windowsLogValue(std::string(100000, 'a')).size() < 2200);
    assert(windowsLogValue("x\n\"y\r") == "x 'y ");
}

#ifdef _WIN32
static void testReadOnlySystemRunner() {
    SystemWindowsNetworkCommands commands;
    WindowsCommandResult result = commands.execute({L"help"}, 10000);
    assert(result.ok());
    assert(!result.output.empty());
    assert(result.output.size() <= 32768);
    assert(result.elapsedMs < 12000);
    assert(quoteWindowsArgument(L"name=adapter with \"quotes\"\\") ==
           L"\"name=adapter with \\\"quotes\\\"\\\\\"");
    result = commands.broadcastRouteExists(L"VLan-configuration-test-nonexistent-adapter");
    assert(!result.ok() || result.absent);
}
#endif

int main() {
    testSuccessAndScopedCommands();
    testCriticalStepsFailClosed();
    testRetryAndTimeoutReconciliation();
    testAbsentVersusFailedCleanup();
    testInvalidMaskNoCommandsAndBoundedLog();
#ifdef _WIN32
    testReadOnlySystemRunner();
#endif
    std::cout << "Windows network configuration: success, failure, retry, timeout, rollback and logs passed\n";
}

#include "cli_log.h"
#include <cassert>
#include <fstream>
#include <sstream>
#include <cstdio>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace VLan { bool g_verboseLog = false; }

static std::string readFile(const std::string& path) {
    std::ifstream file(path.c_str(), std::ios::binary);
    std::ostringstream content;
    content << file.rdbuf();
    return content.str();
}

int main() {
#ifdef _WIN32
    int pid = _getpid();
    // Keep even singleton default log initialization inside the isolated test
    // directory instead of modifying the user's real LOCALAPPDATA directory.
    wchar_t current[32768] = {};
    assert(GetCurrentDirectoryW(32768, current));
    assert(SetEnvironmentVariableW(L"LOCALAPPDATA", current));
#else
    int pid = getpid();
#endif
    const std::string path = "cli-log-test-" + std::to_string(pid) + ".log";
    VLan::CliLog& log = VLan::CliLog::instance();
    assert(log.setLogFile(path));
    assert(log.logFile() == path);
    log.info("firewall operation=%s exit=%d", "add", 0);
    log.error("failure diagnostic");
    log.detail("hidden detail");
    std::string contents = readFile(path);
    assert(contents.find("firewall operation=add exit=0") != std::string::npos);
    assert(contents.find("failure diagnostic") != std::string::npos);
    assert(contents.find("hidden detail") == std::string::npos);
    log.setVerbose(true);
    log.detail("visible detail");
    assert(readFile(path).find("visible detail") != std::string::npos);
    assert(!log.setLogFile("" + path + "/invalid.log"));
    log.info("previous file remains open after invalid replacement");
    assert(readFile(path).find("previous file remains open") != std::string::npos);
    assert(log.setLogFile(""));
    FILE* seed = fopen(path.c_str(), "wb");
    assert(seed);
    const std::string block(1024, 's');
    for (int i = 0; i < 5120; ++i) assert(fwrite(block.data(), 1, block.size(), seed) == block.size());
    fclose(seed);
    assert(log.setLogFile(path));
    log.info("rotation marker");
    assert(readFile(path).find("rotation marker") != std::string::npos);
    assert(readFile(path + ".1").size() == 5 * 1024 * 1024);
    assert(log.setLogFile(""));
    log.info("stderr remains available");
    assert(log.logFile().empty());
    std::remove(path.c_str());
    std::remove((path + ".1").c_str());
    return 0;
}

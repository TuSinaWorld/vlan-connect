#include "signal_server.h"
#include "server_logger.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>

static VLan::SignalServer* g_server = nullptr;

static void signalHandler(int) {
    if (g_server) g_server->stop();
}

static void usage(const char* prog) {
    fprintf(stderr, "Usage: %s [options]\n"
        "  -p PORT   Listen port (TCP+UDP)   (default %u)\n"
        "  -l PATH   Log file path           (default: stdout only)\n"
        "  -L SIZE   Max log file MB         (default: 10)\n"
        "  -v        Verbose/detail log\n"
        "  -h        Show this help\n",
        prog,
        VLan::DEFAULT_PORT);
}

int main(int argc, char* argv[]) {
    uint16_t port = VLan::DEFAULT_PORT;
    std::string logPath;
    size_t logMaxMB = 10;
    bool verbose = false;

    auto parsePort = [&](const char* arg, const char* flag) -> uint16_t {
        char* end = nullptr;
        long val = strtol(arg, &end, 10);
        if (end == arg || *end != '\0' || val < 1 || val > 65535) {
            fprintf(stderr, "Error: invalid port for %s: '%s' (must be 1-65535)\n", flag, arg);
            exit(1);
        }
        return static_cast<uint16_t>(val);
    };

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            port = parsePort(argv[++i], "-p");
        } else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) {
            logPath = argv[++i];
        } else if (strcmp(argv[i], "-L") == 0 && i + 1 < argc) {
            char* end = nullptr;
            long val = strtol(argv[++i], &end, 10);
            if (end == argv[i] || *end != '\0' || val < 1) {
                fprintf(stderr, "Error: invalid value for -L: '%s' (must be > 0)\n", argv[i]);
                return 1;
            }
            logMaxMB = static_cast<size_t>(val);
        } else if (strcmp(argv[i], "-v") == 0) {
            verbose = true;
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    VLan::ServerLogger::instance().init(
        logPath, logMaxMB * 1024 * 1024,
        verbose ? VLan::SRV_LOG_DETAIL : VLan::SRV_LOG_NORMAL);

    VLan::sock_init();

    VLan::SignalServer server;
    g_server = &server;

    signal(SIGINT,  signalHandler);
    signal(SIGTERM, signalHandler);

    if (!server.init(port)) {
        LOG_ERROR("Server init failed");
        return 1;
    }

    LOG_INFO("VLan Server started (port=%u log=%s maxLogMB=%zu verbose=%s)",
             port,
             logPath.empty() ? "stdout" : logPath.c_str(),
             logMaxMB, verbose ? "yes" : "no");

    server.run();
    LOG_INFO("Server stopped.");

    VLan::ServerLogger::instance().shutdown();
    VLan::sock_cleanup();
    return 0;
}

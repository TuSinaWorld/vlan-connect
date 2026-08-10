#include "signal_server.h"
#include "server_logger.h"
#include "auth_file.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <algorithm>

static volatile sig_atomic_t g_stopRequested = 0;

static void signalHandler(int) {
    g_stopRequested = 1;
}

static void usage(const char* prog) {
    fprintf(stderr, "Usage: %s [options]\n"
        "  -p PORT   Listen port (TCP+UDP)   (default %u)\n"
        "  -l PATH   Log file path           (default: stdout only)\n"
        "  -L SIZE   Max log file MB         (default: 10)\n"
        "  --auth-file PATH  Required server auth password file\n"
        "  --max-clients N             Maximum signaling clients (1-256)\n"
        "  --max-pending N             Maximum unclassified clients (1-64)\n"
        "  --max-rooms N               Maximum rooms (1-128)\n"
        "  --max-clients-per-ip N      Maximum clients per IPv4 (1-32)\n"
        "  --max-pending-per-ip N      Maximum pending per IPv4 (1-8)\n"
        "  --max-send-buffer-mb N      Global send buffers MiB (1-64)\n"
        "  -v        Verbose/detail log\n"
        "  -h        Show this help\n",
        prog,
        VLan::DEFAULT_PORT);
}

int main(int argc, char* argv[]) {
    uint16_t port = VLan::DEFAULT_PORT;
    std::string logPath;
    std::string authPassword;
    std::string authFile;
    size_t logMaxMB = 10;
    bool verbose = false;
    bool authFileSeen = false;
    VLan::ServerLimits limits;

    auto parsePort = [&](const char* arg, const char* flag) -> uint16_t {
        char* end = nullptr;
        long val = strtol(arg, &end, 10);
        if (end == arg || *end != '\0' || val < 1 || val > 65535) {
            fprintf(stderr, "Error: invalid port for %s: '%s' (must be 1-65535)\n", flag, arg);
            exit(1);
        }
        return static_cast<uint16_t>(val);
    };

    auto parseLimit = [&](const char* arg, const char* flag,
                          size_t hardMaximum) -> size_t {
        char* end = nullptr;
        unsigned long val = strtoul(arg, &end, 10);
        if (end == arg || *end != '\0' || val < 1 || val > hardMaximum) {
            fprintf(stderr, "Error: invalid value for %s: '%s' (must be 1-%zu)\n",
                    flag, arg, hardMaximum);
            exit(1);
        }
        return static_cast<size_t>(val);
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
        } else if (strcmp(argv[i], "--auth-file") == 0 && i + 1 < argc) {
            if (authFileSeen) {
                fprintf(stderr, "Error: --auth-file may be specified only once\n");
                return 1;
            }
            authFileSeen = true;
            authFile = argv[++i];
        } else if (strcmp(argv[i], "--max-clients") == 0 && i + 1 < argc) {
            limits.maxClients = parseLimit(argv[++i], "--max-clients", 256);
        } else if (strcmp(argv[i], "--max-pending") == 0 && i + 1 < argc) {
            limits.maxPending = parseLimit(argv[++i], "--max-pending", 64);
        } else if (strcmp(argv[i], "--max-rooms") == 0 && i + 1 < argc) {
            limits.maxRooms = parseLimit(argv[++i], "--max-rooms", 128);
        } else if (strcmp(argv[i], "--max-clients-per-ip") == 0 && i + 1 < argc) {
            limits.maxClientsPerIp = parseLimit(argv[++i], "--max-clients-per-ip", 32);
        } else if (strcmp(argv[i], "--max-pending-per-ip") == 0 && i + 1 < argc) {
            limits.maxPendingPerIp = parseLimit(argv[++i], "--max-pending-per-ip", 8);
        } else if (strcmp(argv[i], "--max-send-buffer-mb") == 0 && i + 1 < argc) {
            limits.maxSendBufferBytes =
                parseLimit(argv[++i], "--max-send-buffer-mb", 64) * 1024 * 1024;
        } else if (strcmp(argv[i], "-v") == 0) {
            verbose = true;
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    if (!authFileSeen) {
        fprintf(stderr, "Error: --auth-file PATH is required\n");
        usage(argv[0]);
        return 1;
    }
    std::string authError;
    if (!VLan::readAuthPasswordFile(authFile, &authPassword, &authError)) {
        fprintf(stderr, "Error: %s\n", authError.c_str());
        return 1;
    }

    VLan::SignalServer server;
    server.setLimits(limits);
    if (!server.setAuthPassword(authPassword)) {
        fprintf(stderr, "Error: server authentication KDF failed\n");
        return 1;
    }
    std::fill(authPassword.begin(), authPassword.end(), '\0');
    authPassword.clear();

    VLan::ServerLogger::instance().init(
        logPath, logMaxMB * 1024 * 1024,
        verbose ? VLan::SRV_LOG_DETAIL : VLan::SRV_LOG_NORMAL);

    VLan::sock_init();

    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = signalHandler;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);

    if (!server.init(port)) {
        LOG_ERROR("Server init failed");
        VLan::ServerLogger::instance().shutdown();
        VLan::sock_cleanup();
        return 1;
    }

    LOG_INFO("VLan Server started (port=%u log=%s maxLogMB=%zu verbose=%s auth=required)",
             port,
             logPath.empty() ? "stdout" : logPath.c_str(),
             logMaxMB, verbose ? "yes" : "no");

    server.run(&g_stopRequested);
    server.stop();
    LOG_INFO("Server stopped.");

    VLan::ServerLogger::instance().shutdown();
    VLan::sock_cleanup();
    return 0;
}

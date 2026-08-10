#include "cli_app.h"
#include "cli_log.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <csignal>
#include <fstream>

static volatile sig_atomic_t g_stopRequested = 0;

static void signalHandler(int) {
    g_stopRequested = 1;
}

static std::string trimLine(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static bool readPasswordFile(const char* path, std::string* out) {
    std::ifstream in(path, std::ios::in);
    if (!in) return false;
    std::string line;
    if (!std::getline(in, line)) return false;
    *out = trimLine(line);
    return !out->empty();
}

static void usage(const char* prog) {
    fprintf(stderr,
        "VLan CLI Client\n"
        "Usage: %s [options]\n"
        "  -s HOST   Server address          (default: 127.0.0.1)\n"
        "  -p PORT   Server port             (default: %u)\n"
        "  -n NAME   Player name             (prompted if omitted)\n"
        "  --auth PASSWORD       Server auth password for this process\n"
        "  --auth-file PATH      Read server auth password from first line\n"
        "  -v        Verbose/debug log\n"
        "  -h        Show this help\n"
        "\n"
        "Interactive commands (after connecting):\n"
        "  list                                    List rooms\n"
        "  server <host[:port]> [password]           Set server endpoint\n"
        "  server-password <password>                Set cached server password\n"
        "  create <name> [max] [password] [mtu] [opts]\n"
        "      opts: tcp=raw|kcp|tcp udp=raw|kcp|tcp tcp-fec=none|30 udp-fec=none|30\n"
        "            tcp-profile=realtime|bulk udp-profile=realtime|bulk mtu=1280|1400|1420\n"
        "  join <roomId> [password]                Join room\n"
        "  leave                                   Leave room\n"
        "  status                                  Show connection status\n"
        "  peers                                   Show connected peers\n"
        "  quit                                    Exit\n",
        prog, VLan::DEFAULT_PORT);
}

int main(int argc, char* argv[]) {
    std::string serverHost = "127.0.0.1";
    uint16_t    serverPort = VLan::DEFAULT_PORT;
    std::string playerName;
    std::string serverPassword;
    bool        verbose = false;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            serverHost = argv[++i];
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            char* end = nullptr;
            long val = strtol(argv[++i], &end, 10);
            if (end == argv[i] || *end != '\0' || val < 1 || val > 65535) {
                fprintf(stderr, "Error: invalid port '%s'\n", argv[i]);
                return 1;
            }
            serverPort = static_cast<uint16_t>(val);
        } else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            playerName = argv[++i];
        } else if (strcmp(argv[i], "--auth") == 0 && i + 1 < argc) {
            serverPassword = argv[++i];
        } else if (strcmp(argv[i], "--auth-file") == 0 && i + 1 < argc) {
            if (!readPasswordFile(argv[++i], &serverPassword)) {
                fprintf(stderr, "Error: failed to read auth password file\n");
                return 1;
            }
        } else if (strcmp(argv[i], "-v") == 0) {
            verbose = true;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    VLan::CliApp app;
#ifdef _WIN32
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
#else
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = signalHandler;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);
#endif

    app.setServer(serverHost, serverPort);
    const char* envPassword = std::getenv("VLAN_SERVER_AUTH_PASSWORD");
    if (!serverPassword.empty())
        app.setServerPassword(serverPassword);
    else if (envPassword && *envPassword)
        app.setServerPassword(envPassword);
    app.setPlayerName(playerName);
    app.setVerbose(verbose);

    return app.run(&g_stopRequested);
}

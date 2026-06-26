#include "cli_app.h"
#include "cli_log.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <csignal>

static VLan::CliApp* g_app = nullptr;

static void signalHandler(int) {
    if (g_app) g_app->requestStop();
}

static void usage(const char* prog) {
    fprintf(stderr,
        "VLan CLI Client\n"
        "Usage: %s [options]\n"
        "  -s HOST   Server address          (default: 127.0.0.1)\n"
        "  -p PORT   Server port             (default: %u)\n"
        "  -n NAME   Player name             (prompted if omitted)\n"
        "  -v        Verbose/debug log\n"
        "  -h        Show this help\n"
        "\n"
        "Interactive commands (after connecting):\n"
        "  list                                    List rooms\n"
        "  create <name> [max] [mode] [fec] [pwd]  Create room\n"
        "      mode: 1=P2P 2=KCP 3=TCP 4=RawUDP\n"
        "      fec:  0=None 1=10%% 2=30%% 3=50%% 4=70%% 5=100%% 6=200%%\n"
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
    g_app = &app;

    signal(SIGINT,  signalHandler);
    signal(SIGTERM, signalHandler);

    app.setServer(serverHost, serverPort);
    app.setPlayerName(playerName);
    app.setVerbose(verbose);

    return app.run();
}

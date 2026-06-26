#include "cli_app.h"
#include "cli_log.h"
#include <cstring>
#include <cstdlib>
#include <sstream>
#include <algorithm>
#include <cctype>
#ifndef _WIN32
#include <netdb.h>
#endif

extern "C" {
#include "monocypher.h"
}

namespace VLan {

bool g_verboseLog = false;

static std::string resolveHost(const std::string& host) {
    struct addrinfo hints, *res = nullptr;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res)
        return "";
    char ipBuf[INET_ADDRSTRLEN];
    struct sockaddr_in* addr = (struct sockaddr_in*)res->ai_addr;
    inet_ntop(AF_INET, &addr->sin_addr, ipBuf, sizeof(ipBuf));
    freeaddrinfo(res);
    return ipBuf;
}

static std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static bool parseIntStrict(const std::string& s, int* out) {
    if (s.empty()) return false;
    char* end = nullptr;
    long value = strtol(s.c_str(), &end, 10);
    if (!end || *end != '\0') return false;
    if (out) *out = static_cast<int>(value);
    return true;
}

CliApp::CliApp()
    : m_port(DEFAULT_PORT), m_tun(nullptr), m_puncher(nullptr),
      m_currentRoomId(0), m_myVirtualIP(0), m_myNatType(NAT_UNKNOWN),
      m_transportMode(MODE_RELAY_KCP), m_fecMode(FEC_NONE),
      m_roomMtu(ROOM_MTU_DEFAULT),
      m_encrypted(false), m_hasIntermediate(false), m_hasCipherParams(false),
      m_running(false),
      m_lastPingTime(0), m_lastKcpUpdateTime(0),
      m_lastUdpKeepaliveTime(0), m_lastTcpRelayCheckTime(0),
      m_lastLatencyCheckTime(0), m_lastDataChannelPingTime(0),
      m_wantReconnect(false), m_reconnectAttempts(0),
      m_nextReconnectTime(0), m_wasInRoom(false),
      m_savedMaxPlayers(8), m_savedTransportMode(MODE_RELAY_KCP),
      m_savedFecMode(FEC_NONE), m_savedRoomMtu(ROOM_MTU_DEFAULT),
      m_savedEncrypted(false)
{
    memset(m_intermediate, 0, CIPHER_KEY_SIZE);
    memset(m_encryptKey, 0, CIPHER_KEY_SIZE);
    memset(m_sessionSeed, 0, CIPHER_SESSION_SEED_SIZE);
}

CliApp::~CliApp() {
    teardownTun();
    delete m_puncher;
}

void CliApp::setServer(const std::string& host, uint16_t port) {
    m_serverHost = host;
    m_port = port;
}

void CliApp::setPlayerName(const std::string& name) { m_playerName = name; }
void CliApp::setVerbose(bool v) { CliLog::instance().setVerbose(v); }

void CliApp::requestStop() { m_running = false; }

void CliApp::setupCallbacks() {
    m_signal.onConnected     = [this]() { onSignalConnected(); };
    m_signal.onDisconnected  = [this]() { onSignalDisconnected(); };
    m_signal.onConnectFailed = [this](const std::string& reason) {
        fprintf(stdout, "* Connection failed: %s\n", reason.c_str());
        fflush(stdout);
        if (m_wantReconnect)
            m_nextReconnectTime = currentTimeMs() + RECONNECT_INTERVAL_MS;
    };
    m_signal.onLoginResponse = [this](uint32_t pid) { onLoginResponse(pid); };
    m_signal.onRoomCreated   = [this](uint32_t rid, uint32_t vip, TransportMode tm,
                                      FecMode fm, uint16_t mtu, bool enc, const uint8_t* salt,
                                      const uint8_t* seed) {
        onRoomCreated(rid, vip, tm, fm, mtu, enc, salt, seed);
    };
    m_signal.onJoinResponse  = [this](uint32_t rid, uint32_t vip, TransportMode tm,
                                      FecMode fm, uint16_t mtu, bool enc, const uint8_t* salt,
                                      const uint8_t* seed,
                                      const std::vector<PeerInfo>& members) {
        onJoinResponse(rid, vip, tm, fm, mtu, enc, salt, seed, members);
    };
    m_signal.onPeerJoined    = [this](PeerInfo info) { onPeerJoined(info); };
    m_signal.onPeerLeft      = [this](uint32_t pid) { onPeerLeft(pid); };
    m_signal.onRoomList      = [this](const std::vector<CliRoomListItem>& rooms) {
        m_cachedRoomList = rooms;

        if (m_wantReconnect && m_wasInRoom && !m_savedRoomName.empty()) {
            handleReconnectRoomList(rooms);
            return;
        }

        fprintf(stdout, "\n=== Room List (%zu rooms) ===\n", rooms.size());
        for (size_t i = 0; i < rooms.size(); ++i) {
            const CliRoomListItem& r = rooms[i];
            fprintf(stdout, "  [%u] \"%s\"  %u/%u  mode=%u fec=%s mtu=%u %s\n",
                    r.roomId, r.roomName.c_str(), r.playerCount, r.maxPlayers,
                    r.transportMode, fecModeName(r.fecMode), r.mtu,
                    r.encrypted ? "[encrypted]" : "");
        }
        fprintf(stdout, "=============================\n> ");
        fflush(stdout);
    };
    m_signal.onPunchNotify   = [this](uint32_t pid, uint32_t vip, NatType nat,
                                      uint32_t pubIP, uint16_t pubPort) {
        onPunchNotify(pid, vip, nat, pubIP, pubPort);
    };
    m_signal.onRelayReady    = [this](uint32_t pid) { onRelayReady(pid); };
    m_signal.onServerError   = [this](const std::string& msg) {
        fprintf(stdout, "* Server error: %s\n> ", msg.c_str());
        fflush(stdout);
        if (m_signal.myPeerId() == 0) {
            m_signal.disconnect();
            if (m_wantReconnect)
                m_nextReconnectTime = currentTimeMs() + RECONNECT_INTERVAL_MS;
        }
    };
    m_signal.onServerRtt     = [this](int rtt) {
        LOG_DBG("Server RTT: %d ms", rtt);
    };

    m_dataChannel.onConnectedCb    = []() { LOG_INFO("Data channel established"); };
    m_dataChannel.onDisconnectedCb = []() { LOG_INFO("Data channel disconnected"); };
    m_dataChannel.onRelayData      = [this](uint32_t src, Buffer data) {
        handleTcpRelayReceived(src, data);
    };

    m_tunnel.onTunnelDead = [this](uint32_t pid) { onTunnelDead(pid); };

    m_natDetector.onDetected = [this](NatType type, uint32_t pubIP, uint16_t pubPort) {
        onNatDetected(type, pubIP, pubPort);
    };
}

int CliApp::run() {
    sock_init();

    if (m_playerName.empty()) {
        fprintf(stdout, "Enter player name: ");
        fflush(stdout);
        char buf[256];
        if (!fgets(buf, sizeof(buf), stdin)) return 1;
        m_playerName = trim(buf);
        if (m_playerName.empty()) m_playerName = "CLIPlayer";
    }
    if (!isValidPlayerName(m_playerName)) {
        fprintf(stderr, "Invalid player name: use %d-%d ASCII letters or digits.\n",
                MIN_PLAYER_NAME_LEN, MAX_PLAYER_NAME_LEN);
        sock_cleanup();
        return 1;
    }

    if (!m_tunnel.initUdpSocket()) return 1;

    setupCallbacks();
    m_running = true;

    m_stdinThread = std::thread(&CliApp::stdinReadLoop, this);
    doConnect();

    fprintf(stdout, "VLan CLI Client - Type 'help' for commands.\n> ");
    fflush(stdout);

    eventLoop();

    if (m_stdinThread.joinable()) {
        m_stdinThread.detach();
    }

    teardownTun();
    m_signal.disconnect();
    m_dataChannel.disconnect();
    sock_cleanup();
    return 0;
}

void CliApp::stdinReadLoop() {
    char buf[1024];
    while (m_running) {
#ifdef _WIN32
        if (!fgets(buf, sizeof(buf), stdin)) break;
#else
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        int ready = select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv);
        if (!m_running) break;
        if (ready <= 0) continue;
        if (!fgets(buf, sizeof(buf), stdin)) break;
#endif
        std::string line = trim(buf);
        if (!line.empty())
            m_cmdQueue.push(line);
    }
}

void CliApp::eventLoop() {
    while (m_running) {
        fd_set readfds, writefds;
        FD_ZERO(&readfds);
        FD_ZERO(&writefds);

        socket_t maxfd = 0;
        socket_t sigFd = m_signal.fd();
        socket_t dcFd  = m_dataChannel.fd();
        socket_t udpFd = m_tunnel.udpFd();

        if (sigFd != SOCK_INVALID) {
            FD_SET(sigFd, &readfds);
            if (!m_signal.isConnected())
                FD_SET(sigFd, &writefds);
#ifndef _WIN32
            if (sigFd > maxfd) maxfd = sigFd;
#endif
        }
        if (dcFd != SOCK_INVALID) {
            FD_SET(dcFd, &readfds);
            if (!m_dataChannel.isConnected())
                FD_SET(dcFd, &writefds);
#ifndef _WIN32
            if (dcFd > maxfd) maxfd = dcFd;
#endif
        }
        if (udpFd != SOCK_INVALID) {
            FD_SET(udpFd, &readfds);
#ifndef _WIN32
            if (udpFd > maxfd) maxfd = udpFd;
#endif
        }

        struct timeval tv;
        tv.tv_sec  = 0;
        tv.tv_usec = 5000; // 5ms for KCP granularity

        int ready = select(static_cast<int>(maxfd + 1), &readfds, &writefds, nullptr, &tv);

        if (ready > 0) {
            if (sigFd != SOCK_INVALID && FD_ISSET(sigFd, &writefds))
                m_signal.onWritable();
            if (sigFd != SOCK_INVALID && FD_ISSET(sigFd, &readfds))
                m_signal.onReadable();

            if (dcFd != SOCK_INVALID && FD_ISSET(dcFd, &writefds))
                m_dataChannel.onWritable();
            if (dcFd != SOCK_INVALID && FD_ISSET(dcFd, &readfds))
                m_dataChannel.onReadable();

            if (udpFd != SOCK_INVALID && FD_ISSET(udpFd, &readfds))
                m_tunnel.onUdpReadable();
        }

        /* Flush pending TCP sends */
        if (m_signal.isConnected()) m_signal.onWritable();
        if (m_dataChannel.isConnected()) m_dataChannel.onWritable();

        /* Process TUN packets */
        m_tunnel.processTunPackets();

        /* Process stdin commands */
        std::string cmd;
        while (m_cmdQueue.tryPop(cmd))
            processCommand(cmd);

        /* Timers */
        uint32_t now = currentTimeMs();

        if (now - m_lastKcpUpdateTime >= 5) {
            m_lastKcpUpdateTime = now;
            m_tunnel.updateKcp();
        }

        if (m_signal.isConnected() && now - m_lastPingTime >= static_cast<uint32_t>(KEEPALIVE_INTERVAL_MS)) {
            m_lastPingTime = now;
            m_signal.sendPing();
        }

        if (now - m_lastUdpKeepaliveTime >= static_cast<uint32_t>(UDP_KEEPALIVE_INTERVAL_MS)) {
            m_lastUdpKeepaliveTime = now;
            m_tunnel.sendUdpKeepalive();
        }

        if (m_dataChannel.isConnected() && now - m_lastDataChannelPingTime >= static_cast<uint32_t>(KEEPALIVE_INTERVAL_MS)) {
            m_lastDataChannelPingTime = now;
            m_dataChannel.sendPing();
        }

        m_signal.checkTimeouts();
        m_dataChannel.checkTimeouts();

        if (!m_natDetector.done())
            m_natDetector.checkTimeout(m_tunnel.udpFd());

        if (m_puncher)
            m_puncher->update(m_tunnel.udpFd());

        if (m_currentRoomId != 0 && m_transportMode == MODE_RELAY_TCP &&
            now - m_lastTcpRelayCheckTime >= static_cast<uint32_t>(TCP_RELAY_KEEPALIVE_MS / 2)) {
            m_lastTcpRelayCheckTime = now;
            for (CliP2PPeer* peer : m_tunnel.allPeers()) {
                if (peer->transport() == TRANSPORT_RELAY_TCP) {
                    peer->sendTcpRelayKeepalive();
                    if (peer->isTcpRelayDead()) {
                        onTunnelDead(peer->peerId());
                        break;
                    }
                }
            }
        }

        if (m_currentRoomId != 0 && now - m_lastLatencyCheckTime >= 3000) {
            m_lastLatencyCheckTime = now;
            for (CliP2PPeer* peer : m_tunnel.allPeers()) {
                if (peer->transport() == TRANSPORT_RELAY_TCP)
                    peer->sendLatencyPing();
            }
        }

        if (m_wantReconnect && m_signal.fd() == SOCK_INVALID
            && now >= m_nextReconnectTime) {
            if (m_reconnectAttempts >= MAX_RECONNECT_ATTEMPTS) {
                m_wantReconnect = false;
                m_wasInRoom = false;
                m_savedRoomName.clear();
                fprintf(stdout, "* Auto-reconnect failed after %d attempts. "
                        "Type 'connect' to retry manually.\n> ",
                        MAX_RECONNECT_ATTEMPTS);
                fflush(stdout);
            } else {
                m_reconnectAttempts++;
                fprintf(stdout, "* Auto-reconnect attempt %d/%d ...\n",
                        m_reconnectAttempts, MAX_RECONNECT_ATTEMPTS);
                fflush(stdout);
                doConnect();
            }
        }
    }
}

void CliApp::processCommand(const std::string& line) {
    std::istringstream iss(line);
    std::string cmd;
    iss >> cmd;
    std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::tolower);

    if (cmd == "help" || cmd == "h" || cmd == "?") {
        printHelp();
    } else if (cmd == "list" || cmd == "ls") {
        if (!m_signal.isConnected()) {
            fprintf(stdout, "* Not connected\n> ");
        } else {
            m_signal.listRooms();
            fprintf(stdout, "* Fetching room list...\n");
        }
        fflush(stdout);
    } else if (cmd == "create") {
        std::string rest;
        std::getline(iss, rest);
        doCreateRoom(trim(rest));
    } else if (cmd == "join") {
        std::string rest;
        std::getline(iss, rest);
        doJoinRoom(trim(rest));
    } else if (cmd == "leave") {
        doLeaveRoom();
    } else if (cmd == "status") {
        printStatus();
    } else if (cmd == "peers") {
        printPeers();
    } else if (cmd == "connect") {
        m_wantReconnect = false;
        doConnect();
    } else if (cmd == "quit" || cmd == "exit" || cmd == "q") {
        fprintf(stdout, "Exiting...\n");
        m_running = false;
    } else {
        fprintf(stdout, "Unknown command: %s (type 'help')\n> ", cmd.c_str());
        fflush(stdout);
    }
}

void CliApp::printHelp() {
    fprintf(stdout,
        "\n=== VLan CLI Commands ===\n"
        "  connect                                    - (Re)connect to server\n"
        "  list                                       - List rooms\n"
        "  create <name> [max] [mode] [fec] [password] [mtu]\n"
        "      mode: 1=P2P 2=KCP 3=TCP 4=RawUDP\n"
        "      fec:  0=None 1=10%% 2=30%% 3=50%% 4=70%% 5=100%% 6=200%%\n"
        "      mtu:  1280=safe 1400=balanced 1420=aggressive (default 1400)\n"
        "      password: set to enable encryption\n"
        "  join <roomId> [password]                   - Join room\n"
        "  leave                                      - Leave room\n"
        "  status                                     - Show status\n"
        "  peers                                      - Show peers\n"
        "  quit                                       - Exit\n"
        "=========================\n> ");
    fflush(stdout);
}

void CliApp::printStatus() {
    fprintf(stdout, "\n--- Status ---\n");
    fprintf(stdout, "  Server:    %s:%u\n", m_serverHost.c_str(), m_port);
    fprintf(stdout, "  Connected: %s\n", m_signal.isConnected() ? "yes" : "no");
    fprintf(stdout, "  PeerId:    %u\n", m_signal.myPeerId());
    fprintf(stdout, "  Room:      %u\n", m_currentRoomId);
    if (m_myVirtualIP)
        fprintf(stdout, "  VirtualIP: %s\n", ipToString(m_myVirtualIP).c_str());
    fprintf(stdout, "  NAT:       %s\n", natTypeName(m_myNatType));
    fprintf(stdout, "  Transport: mode=%u\n", m_transportMode);
    fprintf(stdout, "  MTU:       %u\n", m_roomMtu);
    fprintf(stdout, "--------------\n> ");
    fflush(stdout);
}

void CliApp::printPeers() {
    auto peers = m_tunnel.allPeers();
    fprintf(stdout, "\n--- Peers (%zu) ---\n", peers.size());
    for (CliP2PPeer* p : peers) {
        fprintf(stdout, "  [%u] %s  IP=%s  transport=%s  latency=%dms\n",
                p->peerId(), p->name().c_str(),
                ipToString(p->virtualIP()).c_str(),
                transportName(p->transport()),
                p->latencyMs());
    }
    fprintf(stdout, "------------------\n> ");
    fflush(stdout);
}

void CliApp::doConnect() {
    if (m_signal.isConnected()) {
        fprintf(stdout, "* Already connected\n> ");
        fflush(stdout);
        return;
    }

    m_resolvedIP = resolveHost(m_serverHost);
    if (m_resolvedIP.empty()) {
        /* Maybe it's already an IP */
        struct in_addr testAddr;
        if (inet_pton(AF_INET, m_serverHost.c_str(), &testAddr) == 1)
            m_resolvedIP = m_serverHost;
        else {
            fprintf(stdout, "* Failed to resolve host: %s\n> ", m_serverHost.c_str());
            fflush(stdout);
            if (m_wantReconnect)
                m_nextReconnectTime = currentTimeMs() + RECONNECT_INTERVAL_MS;
            return;
        }
    }

    uint32_t serverIPn;
    inet_pton(AF_INET, m_resolvedIP.c_str(), &serverIPn);
    uint32_t serverIP = ntohl(serverIPn);
    m_tunnel.setServerEndpoint(serverIP, m_port);

    fprintf(stdout, "* Connecting to %s:%u ...\n", m_resolvedIP.c_str(), m_port);
    fflush(stdout);

    if (!m_signal.connectTo(m_resolvedIP, m_port)) {
        fprintf(stdout, "* Connect failed\n> ");
        fflush(stdout);
        if (m_wantReconnect)
            m_nextReconnectTime = currentTimeMs() + RECONNECT_INTERVAL_MS;
    }
}

void CliApp::doCreateRoom(const std::string& args) {
    if (!m_signal.isConnected() || m_signal.myPeerId() == 0) {
        fprintf(stdout, "* Not logged in\n> ");
        fflush(stdout);
        return;
    }
    if (m_currentRoomId != 0) {
        fprintf(stdout, "* Already in a room. Leave current room before creating another.\n> ");
        fflush(stdout);
        return;
    }
    std::istringstream iss(args);
    std::string roomName;
    int maxPlayers = 8;
    int mode = 2;
    int fec = 0;
    std::string password;
    std::string extra;
    uint16_t roomMtu = ROOM_MTU_DEFAULT;

    iss >> roomName;
    if (roomName.empty()) roomName = "CLIRoom";
    iss >> maxPlayers >> mode >> fec >> extra;
    if (!extra.empty()) {
        int parsed = 0;
        std::string mtuToken;
        if (parseIntStrict(extra, &parsed) && isValidRoomMtuValue(parsed)) {
            roomMtu = normalizeRoomMtu(parsed);
        } else {
            password = extra;
            if (iss >> mtuToken) {
                if (!parseIntStrict(mtuToken, &parsed) || !isValidRoomMtuValue(parsed)) {
                    fprintf(stdout, "* Invalid MTU. Use 1280, 1400 or 1420\n> ");
                    fflush(stdout);
                    return;
                }
                roomMtu = normalizeRoomMtu(parsed);
            }
        }
    }

    if (!isValidRoomName(roomName)) {
        fprintf(stdout, "* Room name must be 1-%d bytes\n> ", MAX_ROOM_NAME_LEN);
        fflush(stdout);
        return;
    }
    if (maxPlayers < 2 || maxPlayers > MAX_PLAYERS) {
        fprintf(stdout, "* Max players must be between 2 and %d\n> ", MAX_PLAYERS);
        fflush(stdout);
        return;
    }
    if (mode < 0 || mode > 255 ||
        !isValidTransportModeValue(static_cast<uint8_t>(mode))) {
        fprintf(stdout, "* Invalid mode. Use 1=P2P 2=KCP 3=TCP 4=RawUDP\n> ");
        fflush(stdout);
        return;
    }
    if (fec < 0 || fec > 255 ||
        !isValidFecModeValue(static_cast<uint8_t>(fec))) {
        fprintf(stdout, "* Invalid FEC. Use 0=None 1=10%% 2=30%% 3=50%% 4=70%% 5=100%% 6=200%%\n> ");
        fflush(stdout);
        return;
    }

    TransportMode tmode = normalizeTransportMode(static_cast<uint8_t>(mode));
    FecMode fmode = normalizeFecMode(static_cast<uint8_t>(fec), tmode);
    bool encrypted = !password.empty();
    if (encrypted && !isValidRoomPassword(password)) {
        fprintf(stdout, "* Password must be %d-%d ASCII letters or digits\n> ",
                MIN_ROOM_PASSWORD_LEN, MAX_ROOM_PASSWORD_LEN);
        fflush(stdout);
        return;
    }

    m_savedRoomName = roomName;
    m_savedMaxPlayers = static_cast<uint8_t>(maxPlayers);
    m_savedRoomMtu = roomMtu;
    m_roomMtu = roomMtu;

    const uint8_t* pwdHash = nullptr;
    uint8_t hashBuf[CIPHER_KEY_SIZE];
    if (encrypted) {
        computeIntermediate(reinterpret_cast<const uint8_t*>(password.data()),
                            password.size(), m_intermediate);
        m_hasIntermediate = true;
        authHashFromIntermediate(m_intermediate, hashBuf);
        pwdHash = hashBuf;
        m_roomPassword = password;
    }

    fprintf(stdout, "* Creating room \"%s\" max=%d mode=%d fec=%s mtu=%u%s ...\n",
            roomName.c_str(), maxPlayers, static_cast<int>(tmode), fecModeName(fmode),
            roomMtu, encrypted ? " [encrypted]" : "");
    fflush(stdout);
    m_signal.createRoom(roomName, static_cast<uint8_t>(maxPlayers),
                        tmode, fmode, roomMtu, encrypted, pwdHash);
}

void CliApp::doJoinRoom(const std::string& args) {
    if (!m_signal.isConnected() || m_signal.myPeerId() == 0) {
        fprintf(stdout, "* Not logged in\n> ");
        fflush(stdout);
        return;
    }
    if (m_currentRoomId != 0) {
        fprintf(stdout, "* Already in a room. Leave current room before joining another.\n> ");
        fflush(stdout);
        return;
    }
    std::istringstream iss(args);
    uint32_t roomId = 0;
    std::string password;
    iss >> roomId >> password;

    if (roomId == 0) {
        fprintf(stdout, "* Usage: join <roomId> [password]\n> ");
        fflush(stdout);
        return;
    }

    for (size_t i = 0; i < m_cachedRoomList.size(); ++i) {
        if (m_cachedRoomList[i].roomId == roomId) {
            m_savedRoomName = m_cachedRoomList[i].roomName;
            m_savedMaxPlayers = m_cachedRoomList[i].maxPlayers;
            m_savedRoomMtu = normalizeRoomMtu(m_cachedRoomList[i].mtu);
            m_roomMtu = m_savedRoomMtu;
            if (m_cachedRoomList[i].encrypted && password.empty()) {
                fprintf(stdout, "* This room is encrypted. Usage: join <roomId> <password>\n> ");
                fflush(stdout);
                return;
            }
            break;
        }
    }

    const uint8_t* authHash = nullptr;
    uint8_t hashBuf[CIPHER_KEY_SIZE];
    if (!password.empty()) {
        if (!isValidRoomPassword(password)) {
            fprintf(stdout, "* Password must be %d-%d ASCII letters or digits\n> ",
                    MIN_ROOM_PASSWORD_LEN, MAX_ROOM_PASSWORD_LEN);
            fflush(stdout);
            return;
        }
        computeIntermediate(reinterpret_cast<const uint8_t*>(password.data()),
                            password.size(), m_intermediate);
        m_hasIntermediate = true;
        authHashFromIntermediate(m_intermediate, hashBuf);
        authHash = hashBuf;
        m_roomPassword = password;
    }

    fprintf(stdout, "* Joining room %u ...\n", roomId);
    fflush(stdout);
    m_signal.joinRoom(roomId, authHash);
}

void CliApp::doLeaveRoom() {
    if (m_currentRoomId == 0) {
        fprintf(stdout, "* Not in a room\n> ");
        fflush(stdout);
        return;
    }
    m_signal.leaveRoom();
    teardownTun();
    m_currentRoomId = 0;
    m_myVirtualIP = 0;
    m_roomMtu = ROOM_MTU_DEFAULT;
    m_encrypted = false;
    m_roomPassword.clear();
    m_hasIntermediate = false;
    m_hasCipherParams = false;
    m_savedRoomName.clear();
    m_wasInRoom = false;
    fprintf(stdout, "* Left room\n> ");
    fflush(stdout);
}

// ───────── Signal callbacks ─────────

void CliApp::onSignalConnected() {
    fprintf(stdout, "* Connected, logging in as \"%s\"...\n", m_playerName.c_str());
    fflush(stdout);
    m_signal.login(m_playerName);
}

void CliApp::onSignalDisconnected() {
    bool wasInRoom = (m_currentRoomId != 0);

    if (!m_wantReconnect) {
        if (wasInRoom) {
            m_wasInRoom = true;
            m_savedTransportMode = m_transportMode;
            m_savedFecMode = m_fecMode;
            m_savedRoomMtu = m_roomMtu;
            m_savedEncrypted = m_encrypted;
        } else {
            m_wasInRoom = false;
        }
    }

    teardownTun();
    m_currentRoomId = 0;
    m_myVirtualIP = 0;
    m_roomMtu = ROOM_MTU_DEFAULT;

    if (!m_wantReconnect && !m_playerName.empty()) {
        m_wantReconnect = true;
        m_reconnectAttempts = 0;
        m_nextReconnectTime = currentTimeMs() + RECONNECT_INTERVAL_MS;
        fprintf(stdout, "* Disconnected from server. Auto-reconnect in %d sec "
                "(%d attempts max)%s\n",
                RECONNECT_INTERVAL_MS / 1000, MAX_RECONNECT_ATTEMPTS,
                m_wasInRoom ? ", will rejoin room" : "");
        fflush(stdout);
    } else if (m_wantReconnect) {
        m_nextReconnectTime = currentTimeMs() + RECONNECT_INTERVAL_MS;
    } else {
        fprintf(stdout, "* Disconnected from server\n> ");
        fflush(stdout);
    }
}

void CliApp::onLoginResponse(uint32_t peerId) {
    fprintf(stdout, "* Logged in, peerId=%u\n", peerId);
    fflush(stdout);
    m_tunnel.setMyPeerId(peerId);

    if (m_wantReconnect) {
        if (m_wasInRoom && !m_savedRoomName.empty()) {
            fprintf(stdout, "* Reconnected. Searching for room \"%s\"...\n",
                    m_savedRoomName.c_str());
            fflush(stdout);
            m_signal.listRooms();
        } else {
            m_wantReconnect = false;
            fprintf(stdout, "* Reconnected successfully.\n> ");
            fflush(stdout);
        }
    }

    /* Connect data channel */
    m_dataChannel.connectTo(m_resolvedIP, m_port, peerId);

    /* Create hole puncher */
    delete m_puncher;
    m_puncher = new CliHolePuncher(peerId);
    m_puncher->onPunchSucceeded = [this](uint32_t pid, uint32_t ip, uint16_t port) {
        CliP2PPeer* peer = m_tunnel.peerById(pid);
        if (!peer) return;
        m_tunnel.createKcpTunnel(peer, ip, port, TRANSPORT_P2P_KCP,
                                 FEC_NONE, m_roomMtu);
        m_signal.reportPunchResult(pid, true);
        fprintf(stdout, "* %s P2P direct connected (IP=%s)\n> ",
                peer->name().c_str(), ipToString(peer->virtualIP()).c_str());
        fflush(stdout);
    };
    m_puncher->onPunchFailed = [this](uint32_t pid) {
        m_signal.reportPunchResult(pid, false);
        LOG_INFO("Punch failed for peer %u", pid);
    };

    /* Setup raw UDP handler for STUN and punch responses */
    m_tunnel.onRawUdpPacket = [this](const uint8_t* data, size_t len,
                                     uint32_t senderIP, uint16_t senderPort) {
        if (len == 0) return;
        uint8_t pktType = data[0];
        if (pktType == UDP_STUN_RESPONSE) {
            m_natDetector.handleStunResponse(data, len);
        } else if ((pktType == UDP_PUNCH || pktType == UDP_PUNCH_ACK) && m_puncher) {
            m_puncher->handleIncomingPacket(data, len, senderIP, senderPort);
            /* Also need to send ACK for incoming PUNCH */
            if (pktType == UDP_PUNCH && len >= sizeof(PunchPacket)) {
                const PunchPacket* pkt = reinterpret_cast<const PunchPacket*>(data);
                PunchPacket ack;
                memset(&ack, 0, sizeof(ack));
                ack.type   = UDP_PUNCH_ACK;
                ack.peerId = htonl(m_signal.myPeerId());
                ack.token  = pkt->token;
                struct sockaddr_in addr = makeAddr(senderIP, senderPort);
                sendto(m_tunnel.udpFd(), reinterpret_cast<const char*>(&ack), sizeof(ack), 0,
                       (struct sockaddr*)&addr, sizeof(addr));
            }
        }
    };

    /* Detect NAT */
    uint32_t serverIPn;
    inet_pton(AF_INET, m_resolvedIP.c_str(), &serverIPn);
    uint32_t serverIP = ntohl(serverIPn);
    m_natDetector.detect(m_tunnel.udpFd(), m_tunnel.localUdpPort(),
                         serverIP, m_port, peerId);
}

void CliApp::onRoomCreated(uint32_t roomId, uint32_t virtualIP,
                            TransportMode tmode, FecMode fmode,
                            uint16_t mtu,
                            bool encrypted, const uint8_t* salt,
                            const uint8_t* sessionSeed) {
    m_currentRoomId = roomId;
    m_myVirtualIP   = virtualIP;
    m_transportMode = tmode;
    m_fecMode       = fmode;
    m_roomMtu       = normalizeRoomMtu(mtu);
    m_savedRoomMtu  = m_roomMtu;
    m_encrypted     = encrypted;
    m_tunnel.setMyVirtualIP(virtualIP);

    if (encrypted && m_hasIntermediate && salt && sessionSeed) {
        deriveKey(m_intermediate, salt, m_encryptKey);
        memcpy(m_sessionSeed, sessionSeed, CIPHER_SESSION_SEED_SIZE);
        m_hasCipherParams = true;
        crypto_wipe(m_intermediate, CIPHER_KEY_SIZE);
        m_hasIntermediate = false;
    }

    setupTun();
    fprintf(stdout, "* Room created (ID=%u, IP=%s, MTU=%u)\n> ",
            roomId, ipToString(virtualIP).c_str(), m_roomMtu);
    fflush(stdout);
}

void CliApp::onJoinResponse(uint32_t roomId, uint32_t virtualIP,
                             TransportMode tmode, FecMode fmode,
                             uint16_t mtu,
                             bool encrypted, const uint8_t* salt,
                             const uint8_t* sessionSeed,
                             const std::vector<PeerInfo>& members) {
    m_currentRoomId = roomId;
    m_myVirtualIP   = virtualIP;
    m_transportMode = tmode;
    m_fecMode       = fmode;
    m_roomMtu       = normalizeRoomMtu(mtu);
    m_savedRoomMtu  = m_roomMtu;
    m_encrypted     = encrypted;
    m_tunnel.setMyVirtualIP(virtualIP);

    if (encrypted && m_hasIntermediate && salt && sessionSeed) {
        deriveKey(m_intermediate, salt, m_encryptKey);
        memcpy(m_sessionSeed, sessionSeed, CIPHER_SESSION_SEED_SIZE);
        m_hasCipherParams = true;
        crypto_wipe(m_intermediate, CIPHER_KEY_SIZE);
        m_hasIntermediate = false;
    }

    setupTun();

    fprintf(stdout, "* Joined room (ID=%u, IP=%s, MTU=%u, %zu members)\n> ",
            roomId, ipToString(virtualIP).c_str(), m_roomMtu, members.size());
    fflush(stdout);

    for (const PeerInfo& pi : members) {
        if (pi.peerId == m_signal.myPeerId()) continue;
        std::string name = pi.name.empty() ? ("Peer" + std::to_string(pi.peerId)) : pi.name;
        CliP2PPeer* peer = m_tunnel.addPeer(pi.peerId, pi.virtualIP, name);
        peer->setNatType(pi.natType);
        if (m_hasCipherParams)
            peer->setCipherKey(m_encryptKey, m_signal.myPeerId(), m_sessionSeed);

        peer->setOnLatencyPong([this](uint32_t pid, const Buffer& pongData) {
            if (m_dataChannel.isConnected())
                m_dataChannel.sendRelayData(m_signal.myPeerId(), pid, pongData);
        });

        if (m_transportMode == MODE_RELAY_TCP)
            setupTcpRelayTunnel(pi.peerId);
        else if (m_transportMode == MODE_RELAY_KCP || m_transportMode == MODE_RELAY_RAW_UDP)
            m_signal.requestRelay(pi.peerId);
        else if (m_transportMode == MODE_P2P_ONLY && pi.publicIP != 0)
            m_puncher->startPunch(pi.peerId, pi.publicIP, pi.publicPort);
    }
}

void CliApp::onPeerJoined(PeerInfo info) {
    std::string name = info.name.empty() ? ("Peer" + std::to_string(info.peerId)) : info.name;
    CliP2PPeer* peer = m_tunnel.addPeer(info.peerId, info.virtualIP, name);
    if (peer) {
        peer->setNatType(info.natType);
        if (m_hasCipherParams)
            peer->setCipherKey(m_encryptKey, m_signal.myPeerId(), m_sessionSeed);
        peer->setOnLatencyPong([this](uint32_t pid, const Buffer& pongData) {
            if (m_dataChannel.isConnected())
                m_dataChannel.sendRelayData(m_signal.myPeerId(), pid, pongData);
        });
    }

    fprintf(stdout, "* Player %s joined (IP=%s)\n> ",
            name.c_str(), ipToString(info.virtualIP).c_str());
    fflush(stdout);

    if (m_transportMode == MODE_RELAY_TCP)
        setupTcpRelayTunnel(info.peerId);
    else if (m_transportMode == MODE_RELAY_KCP || m_transportMode == MODE_RELAY_RAW_UDP)
        m_signal.requestRelay(info.peerId);
}

void CliApp::onPeerLeft(uint32_t peerId) {
    CliP2PPeer* peer = m_tunnel.peerById(peerId);
    std::string name = peer ? peer->name() : ("Peer" + std::to_string(peerId));
    m_tunnel.removePeer(peerId);
    fprintf(stdout, "* Player %s left\n> ", name.c_str());
    fflush(stdout);
}

void CliApp::onPunchNotify(uint32_t peerId, uint32_t virtualIP,
                            NatType natType, uint32_t publicIP, uint16_t publicPort) {
    CliP2PPeer* peer = m_tunnel.peerById(peerId);
    if (!peer) {
        peer = m_tunnel.addPeer(peerId, virtualIP, "Peer" + std::to_string(peerId));
        if (m_hasCipherParams)
            peer->setCipherKey(m_encryptKey, m_signal.myPeerId(), m_sessionSeed);
        peer->setOnLatencyPong([this](uint32_t pid, const Buffer& pongData) {
            if (m_dataChannel.isConnected())
                m_dataChannel.sendRelayData(m_signal.myPeerId(), pid, pongData);
        });
    }
    peer->setNatType(natType);

    if (m_transportMode == MODE_RELAY_TCP) { setupTcpRelayTunnel(peerId); return; }
    if (m_transportMode == MODE_RELAY_KCP) { m_signal.requestRelay(peerId); return; }
    if (m_transportMode == MODE_RELAY_RAW_UDP) { m_signal.requestRelay(peerId); return; }
    if (publicIP != 0) {
        peer->setPublicEndpoint(publicIP, publicPort);
        if (m_puncher) m_puncher->startPunch(peerId, publicIP, publicPort);
    }
}

void CliApp::onRelayReady(uint32_t peerId) {
    CliP2PPeer* peer = m_tunnel.peerById(peerId);
    if (!peer) return;
    if (peer->transport() != TRANSPORT_NONE) return;

    if (m_transportMode == MODE_RELAY_TCP) { setupTcpRelayTunnel(peerId); return; }
    if (m_transportMode == MODE_RELAY_RAW_UDP) { setupRawUdpRelayTunnel(peerId); return; }
    if (m_transportMode == MODE_RELAY_KCP) {
        setupRelayTunnel(peerId);
        fprintf(stdout, "* %s connected via KCP relay (IP=%s)\n> ",
                peer->name().c_str(), ipToString(peer->virtualIP()).c_str());
        fflush(stdout);
    }
}

void CliApp::onNatDetected(NatType type, uint32_t, uint16_t) {
    m_myNatType = type;
    m_signal.reportNatType(type);
    fprintf(stdout, "* NAT type: %s\n> ", natTypeName(type));
    fflush(stdout);
}

void CliApp::onTunnelDead(uint32_t peerId) {
    CliP2PPeer* peer = m_tunnel.peerById(peerId);
    if (!peer) return;
    std::string name = peer->name();
    uint32_t vip = peer->virtualIP();
    uint32_t savedPubIP = peer->publicIP();
    uint16_t savedPubPort = peer->publicPort();

    LOG_INFO("%s tunnel dead, rebuilding...", name.c_str());

    m_tunnel.removePeer(peerId);
    peer = m_tunnel.addPeer(peerId, vip, name);
    if (!peer) return;
    if (m_hasCipherParams)
        peer->setCipherKey(m_encryptKey, m_signal.myPeerId(), m_sessionSeed);
    peer->setOnLatencyPong([this](uint32_t pid, const Buffer& pongData) {
        if (m_dataChannel.isConnected())
            m_dataChannel.sendRelayData(m_signal.myPeerId(), pid, pongData);
    });

    if (m_transportMode == MODE_RELAY_TCP) setupTcpRelayTunnel(peerId);
    else if (m_transportMode == MODE_RELAY_KCP) m_signal.requestRelay(peerId);
    else if (m_transportMode == MODE_RELAY_RAW_UDP) m_signal.requestRelay(peerId);
    else if (m_transportMode == MODE_P2P_ONLY && savedPubIP != 0) {
        peer->setPublicEndpoint(savedPubIP, savedPubPort);
        if (m_puncher) m_puncher->startPunch(peerId, savedPubIP, savedPubPort);
    }
}

// ───────── TUN setup/teardown ─────────

void CliApp::setupTun() {
    if (m_tun) return;
    m_tun = new CliTunAdapter();
    if (!m_tun->initialize()) {
        fprintf(stdout, "* TUN init failed (need admin/root)\n> ");
        fflush(stdout);
        delete m_tun; m_tun = nullptr;
        return;
    }
    int mtu = static_cast<int>(normalizeRoomMtu(m_roomMtu));
    if (!m_tun->configureIP(m_myVirtualIP, VNET_MASK, mtu)) {
        fprintf(stdout, "* TUN IP config failed\n> ");
        fflush(stdout);
        delete m_tun; m_tun = nullptr;
        return;
    }
    if (!m_tun->startSession()) {
        fprintf(stdout, "* TUN session start failed\n> ");
        fflush(stdout);
        delete m_tun; m_tun = nullptr;
        return;
    }
    m_tunnel.setTunAdapter(m_tun);
    fprintf(stdout, "* TUN adapter started, IP=%s, MTU=%d\n> ",
            ipToString(m_myVirtualIP).c_str(), mtu);
    fflush(stdout);
}

void CliApp::teardownTun() {
    m_tunnel.removeAllPeers();
    if (m_tun) {
        m_tunnel.setTunAdapter(nullptr);
        m_tun->shutdown();
        delete m_tun;
        m_tun = nullptr;
    }
    m_hasCipherParams = false;
}

// ───────── Relay tunnel setup ─────────

void CliApp::setupRelayTunnel(uint32_t peerId) {
    CliP2PPeer* peer = m_tunnel.peerById(peerId);
    if (!peer || peer->transport() != TRANSPORT_NONE) return;

    uint32_t serverIPn;
    inet_pton(AF_INET, m_resolvedIP.c_str(), &serverIPn);
    uint32_t serverIP = ntohl(serverIPn);

    CliKcpTunnel* kcp = m_tunnel.createKcpTunnel(
        peer, serverIP, m_port, TRANSPORT_RELAY_KCP, m_fecMode, m_roomMtu);
    kcp->setRelayMode(m_signal.myPeerId(), peerId);
}

void CliApp::setupRawUdpRelayTunnel(uint32_t peerId) {
    CliP2PPeer* peer = m_tunnel.peerById(peerId);
    if (!peer || peer->transport() != TRANSPORT_NONE) return;

    uint32_t serverIPn;
    inet_pton(AF_INET, m_resolvedIP.c_str(), &serverIPn);
    uint32_t serverIP = ntohl(serverIPn);

    CliRawUdpTunnel* tunnel = m_tunnel.createRawUdpTunnel(
        peer, serverIP, m_port, m_fecMode, m_roomMtu);
    tunnel->setRelayMode(m_signal.myPeerId(), peerId);

    fprintf(stdout, "* %s connected via Raw UDP relay (IP=%s)\n> ",
            peer->name().c_str(), ipToString(peer->virtualIP()).c_str());
    fflush(stdout);
}

void CliApp::setupTcpRelayTunnel(uint32_t peerId) {
    CliP2PPeer* peer = m_tunnel.peerById(peerId);
    if (!peer || peer->transport() != TRANSPORT_NONE) return;

    uint32_t myId = m_signal.myPeerId();
    peer->setTcpRelaySender([this, myId](uint32_t dstPeerId, const Buffer& data) {
        if (m_dataChannel.isConnected())
            m_dataChannel.sendRelayData(myId, dstPeerId, data);
    });
    peer->setTransport(TRANSPORT_RELAY_TCP);

    fprintf(stdout, "* %s connected via TCP relay (IP=%s)\n> ",
            peer->name().c_str(), ipToString(peer->virtualIP()).c_str());
    fflush(stdout);
}

void CliApp::handleReconnectRoomList(const std::vector<CliRoomListItem>& rooms) {
    m_wantReconnect = false;

    uint32_t foundRoomId = 0;
    for (size_t i = 0; i < rooms.size(); ++i) {
        if (rooms[i].roomName == m_savedRoomName) {
            foundRoomId = rooms[i].roomId;
            break;
        }
    }

    if (foundRoomId != 0) {
        fprintf(stdout, "* Found room \"%s\" (ID=%u), rejoining...\n",
                m_savedRoomName.c_str(), foundRoomId);
        fflush(stdout);

        const uint8_t* authHash = nullptr;
        uint8_t hashBuf[CIPHER_KEY_SIZE];
        if (m_savedEncrypted && !m_roomPassword.empty()) {
            computeIntermediate(
                reinterpret_cast<const uint8_t*>(m_roomPassword.data()),
                m_roomPassword.size(), m_intermediate);
            m_hasIntermediate = true;
            authHashFromIntermediate(m_intermediate, hashBuf);
            authHash = hashBuf;
        }
        m_signal.joinRoom(foundRoomId, authHash);
    } else {
        fprintf(stdout, "* Room \"%s\" not found, creating with same settings...\n",
                m_savedRoomName.c_str());
        fflush(stdout);

        const uint8_t* pwdHash = nullptr;
        uint8_t hashBuf[CIPHER_KEY_SIZE];
        if (m_savedEncrypted && !m_roomPassword.empty()) {
            computeIntermediate(
                reinterpret_cast<const uint8_t*>(m_roomPassword.data()),
                m_roomPassword.size(), m_intermediate);
            m_hasIntermediate = true;
            authHashFromIntermediate(m_intermediate, hashBuf);
            pwdHash = hashBuf;
        }
        m_signal.createRoom(m_savedRoomName, m_savedMaxPlayers,
                            m_savedTransportMode, m_savedFecMode,
                            m_savedRoomMtu, m_savedEncrypted, pwdHash);
    }
}

void CliApp::handleTcpRelayReceived(uint32_t srcPeerId, Buffer data) {
    CliP2PPeer* peer = m_tunnel.peerById(srcPeerId);
    if (!peer) return;
    peer->onTcpRelayDataReceived();
    if (data.empty()) return;

    if (data.size() >= 2 && data[0] == CLI_LATENCY_PROBE_MARKER) {
        peer->handleLatencyProbe(data);
        return;
    }

    if (peer->hasCipher() && data.size() >= 20 && (data[0] & 0xF0) == 0x40) {
        data = peer->decryptData(data);
        if (data.empty()) return;
    }
    if (m_tun) m_tun->writePacket(data);
}

} // namespace VLan

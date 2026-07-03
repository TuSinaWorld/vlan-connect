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

static std::string toLowerCopy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

static const char* transportModeCliName(TransportMode mode) {
    switch (mode) {
    case MODE_RELAY_KCP:     return "KCP";
    case MODE_RELAY_TCP:     return "TCP Relay";
    case MODE_RELAY_RAW_UDP: return "Raw UDP";
    default:                 return "Unknown";
    }
}

static const char* kcpProfileName(KcpProfile profile) {
    return profile == KCP_PROFILE_BULK ? "bulk" : "realtime";
}

static std::string policyText(const RoomTrafficPolicy& policy) {
    std::string text = transportModeCliName(policy.transportMode);
    if (policy.transportMode == MODE_RELAY_KCP) {
        text += "/";
        text += kcpProfileName(policy.kcpProfile);
    }
    if (policy.fecMode != FEC_NONE) {
        text += "/";
        text += fecModeName(policy.fecMode);
    }
    return text;
}

static bool samePolicy(const RoomTrafficPolicy& a, const RoomTrafficPolicy& b) {
    return a.transportMode == b.transportMode &&
           a.fecMode == b.fecMode &&
           a.kcpProfile == b.kcpProfile;
}

static bool deadlineReached(uint32_t deadlineMs) {
    return deadlineMs != 0 &&
           static_cast<int32_t>(currentTimeMs() - deadlineMs) >= 0;
}

static bool parseTransportModeValue(const std::string& text, TransportMode* out) {
    std::string v = toLowerCopy(text);
    v.erase(std::remove(v.begin(), v.end(), '_'), v.end());
    v.erase(std::remove(v.begin(), v.end(), '-'), v.end());
    if (v == "raw" || v == "rawudp" || v == "udp") {
        *out = MODE_RELAY_RAW_UDP;
        return true;
    }
    if (v == "kcp") {
        *out = MODE_RELAY_KCP;
        return true;
    }
    if (v == "tcp" || v == "tcprelay" || v == "relaytcp") {
        *out = MODE_RELAY_TCP;
        return true;
    }
    int raw = 0;
    if (parseIntStrict(v, &raw) && isValidTransportModeValue(static_cast<uint8_t>(raw))) {
        *out = static_cast<TransportMode>(raw);
        return true;
    }
    return false;
}

static bool parseFecModeValue(const std::string& text, FecMode* out) {
    std::string v = toLowerCopy(text);
    v.erase(std::remove(v.begin(), v.end(), '%'), v.end());
    if (v == "none" || v == "off" || v == "false") {
        *out = FEC_NONE;
        return true;
    }
    int raw = 0;
    if (!parseIntStrict(v, &raw)) return false;
    switch (raw) {
    case 0:   *out = FEC_NONE; return true;
    case 1:
    case 10:  *out = FEC_10; return true;
    case 2:
    case 30:  *out = FEC_30; return true;
    case 3:
    case 50:  *out = FEC_50; return true;
    case 4:
    case 70:  *out = FEC_70; return true;
    case 5:
    case 100: *out = FEC_100; return true;
    case 6:
    case 200: *out = FEC_200; return true;
    default:  return false;
    }
}

static bool parseKcpProfileValue(const std::string& text, KcpProfile* out) {
    std::string v = toLowerCopy(text);
    if (v == "realtime" || v == "rt" || v == "latency") {
        *out = KCP_PROFILE_REALTIME;
        return true;
    }
    if (v == "bulk" || v == "throughput" || v == "file") {
        *out = KCP_PROFILE_BULK;
        return true;
    }
    int raw = 0;
    if (parseIntStrict(v, &raw) && isValidKcpProfileValue(static_cast<uint8_t>(raw))) {
        *out = static_cast<KcpProfile>(raw);
        return true;
    }
    return false;
}

static bool applyPolicyToken(const std::string& key,
                             const std::string& value,
                             RoomTrafficPolicy* tcpPolicy,
                             RoomTrafficPolicy* udpPolicy,
                             uint16_t* roomMtu,
                             std::string* roomPassword,
                             std::string* error)
{
    std::string k = toLowerCopy(key);
    k.erase(std::remove(k.begin(), k.end(), '_'), k.end());
    k.erase(std::remove(k.begin(), k.end(), '-'), k.end());

    if (k == "mtu") {
        int mtu = 0;
        if (!parseIntStrict(value, &mtu) || !isValidRoomMtuValue(mtu)) {
            *error = "Invalid MTU. Use 1280, 1400 or 1420";
            return false;
        }
        *roomMtu = normalizeRoomMtu(mtu);
        return true;
    }

    if (k == "password" || k == "pass" || k == "roompassword") {
        *roomPassword = value;
        return true;
    }

    RoomTrafficPolicy* policy = nullptr;
    std::string suffix;
    if (k.rfind("tcp", 0) == 0) {
        policy = tcpPolicy;
        suffix = k.substr(3);
    } else if (k.rfind("udp", 0) == 0) {
        policy = udpPolicy;
        suffix = k.substr(3);
    } else {
        *error = "Unknown create option: " + key;
        return false;
    }

    if (suffix.empty() || suffix == "mode" || suffix == "protocol" || suffix == "transport") {
        TransportMode mode;
        if (!parseTransportModeValue(value, &mode)) {
            *error = "Invalid transport for " + key + ": use raw, kcp or tcp";
            return false;
        }
        policy->transportMode = mode;
        return true;
    }

    if (suffix == "fec") {
        FecMode fec;
        if (!parseFecModeValue(value, &fec)) {
            *error = "Invalid FEC for " + key + ": use none, 10, 30, 50, 70, 100 or 200";
            return false;
        }
        policy->fecMode = fec;
        return true;
    }

    if (suffix == "profile" || suffix == "kcpprofile") {
        KcpProfile profile;
        if (!parseKcpProfileValue(value, &profile)) {
            *error = "Invalid KCP profile for " + key + ": use realtime or bulk";
            return false;
        }
        policy->kcpProfile = profile;
        return true;
    }

    *error = "Unknown create option: " + key;
    return false;
}

CliApp::CliApp()
    : m_port(DEFAULT_PORT), m_tun(nullptr),
      m_currentRoomId(0), m_myVirtualIP(0),
      m_tcpPolicy(makeDefaultTcpPolicy()),
      m_udpPolicy(makeDefaultUdpPolicy()),
      m_roomMtu(ROOM_MTU_DEFAULT),
      m_roomPasswordProtected(false),
      m_running(false),
      m_lastPingTime(0), m_lastKcpUpdateTime(0),
      m_lastUdpKeepaliveTime(0), m_lastTcpRelayCheckTime(0),
      m_lastLatencyCheckTime(0), m_lastDataChannelPingTime(0),
      m_wantReconnect(false), m_reconnectAttempts(0),
      m_nextReconnectTime(0), m_wasInRoom(false),
      m_pendingResumeRoom(false), m_manualDisconnecting(false),
      m_logoutPending(false), m_exitAfterDisconnect(false),
      m_logoutDeadline(0), m_hasResumeLease(false),
      m_resumeRoomId(0), m_resumePeerId(0), m_resumeVirtualIP(0),
      m_resumeLeaseDeadlineMs(0),
      m_savedRoomId(0), m_savedMaxPlayers(8),
      m_savedTcpPolicy(makeDefaultTcpPolicy()),
      m_savedUdpPolicy(makeDefaultUdpPolicy()),
      m_savedRoomMtu(ROOM_MTU_DEFAULT),
      m_savedRoomPasswordProtected(false)
{
}

CliApp::~CliApp() {
    teardownTun();
}

void CliApp::setServer(const std::string& host, uint16_t port) {
    m_serverHost = host;
    m_port = port;
    m_serverPassword.clear();
    clearResumeLease();
}

void CliApp::setServerPassword(const std::string& password) { m_serverPassword = password; }
void CliApp::setPlayerName(const std::string& name) { m_playerName = name; }
void CliApp::setVerbose(bool v) { CliLog::instance().setVerbose(v); }

void CliApp::requestStop() { beginGracefulDisconnect(true); }

void CliApp::rememberResumeLease(uint32_t roomId, uint32_t peerId,
                                 uint32_t virtualIP, const Buffer& token) {
    if (token.size() != RECONNECT_TOKEN_SIZE) {
        clearResumeLease();
        return;
    }
    m_hasResumeLease = true;
    m_resumeRoomId = roomId;
    m_resumePeerId = peerId;
    m_resumeVirtualIP = virtualIP;
    m_resumeLeaseDeadlineMs = 0;
    m_resumeToken = token;
}

void CliApp::clearResumeLease() {
    m_hasResumeLease = false;
    m_resumeRoomId = 0;
    m_resumePeerId = 0;
    m_resumeVirtualIP = 0;
    m_resumeLeaseDeadlineMs = 0;
    if (!m_resumeToken.empty()) {
        crypto_wipe(m_resumeToken.data(), m_resumeToken.size());
        m_resumeToken.clear();
    }
}

void CliApp::startResumeLeaseDeadline() {
    if (!m_hasResumeLease || m_resumeToken.size() != RECONNECT_TOKEN_SIZE)
        return;
    m_resumeLeaseDeadlineMs = currentTimeMs() +
        static_cast<uint32_t>(RECONNECT_LEASE_TIMEOUT_SEC * 1000);
}

void CliApp::expireResumeLeaseIfNeeded() {
    if (deadlineReached(m_resumeLeaseDeadlineMs))
        clearResumeLease();
}

bool CliApp::hasUsableResumeLease() {
    expireResumeLeaseIfNeeded();
    return m_hasResumeLease &&
           m_resumeToken.size() == RECONNECT_TOKEN_SIZE &&
           !deadlineReached(m_resumeLeaseDeadlineMs);
}

void CliApp::setupCallbacks() {
    m_signal.onConnected     = [this]() { onSignalConnected(); };
    m_signal.onDisconnected  = [this]() { onSignalDisconnected(); };
    m_signal.onConnectFailed = [this](const std::string& reason) {
        fprintf(stdout, "* Connection failed: %s\n", reason.c_str());
        fflush(stdout);
        if (m_wantReconnect)
            m_nextReconnectTime = currentTimeMs() + RECONNECT_INTERVAL_MS;
    };
    m_signal.onLoginResponse = [this](uint32_t pid, bool resumeAccepted) {
        onLoginResponse(pid, resumeAccepted);
    };
    m_signal.onRoomCreated   = [this](uint32_t rid, uint32_t vip,
                                      RoomTrafficPolicy tcpPolicy,
                                      RoomTrafficPolicy udpPolicy,
                                      uint16_t mtu, bool passwordProtected,
                                      const Buffer& leaseToken) {
        onRoomCreated(rid, vip, tcpPolicy, udpPolicy, mtu, passwordProtected, leaseToken);
    };
    m_signal.onJoinResponse  = [this](uint32_t rid, uint32_t vip,
                                      RoomTrafficPolicy tcpPolicy,
                                      RoomTrafficPolicy udpPolicy,
                                      uint16_t mtu, bool passwordProtected,
                                      const std::vector<PeerInfo>& members,
                                      const Buffer& leaseToken) {
        onJoinResponse(rid, vip, tcpPolicy, udpPolicy, mtu, passwordProtected, members, leaseToken);
    };
    m_signal.onPeerJoined    = [this](PeerInfo info) { onPeerJoined(info); };
    m_signal.onPeerLeft      = [this](uint32_t pid) { onPeerLeft(pid); };
    m_signal.onRoomList      = [this](const std::vector<CliRoomListItem>& rooms, bool pushed) {
        m_cachedRoomList = rooms;

        if (!m_pendingResumeRoom &&
            m_wantReconnect && m_wasInRoom && !m_savedRoomName.empty()) {
            handleReconnectRoomList(rooms);
            return;
        }

        if (pushed)
            return;

        fprintf(stdout, "\n=== Room List (%zu rooms) ===\n", rooms.size());
        for (size_t i = 0; i < rooms.size(); ++i) {
            const CliRoomListItem& r = rooms[i];
            std::string tcpText = policyText(r.tcpPolicy);
            std::string udpText = policyText(r.udpPolicy);
            fprintf(stdout, "  [%u] \"%s\"  %u/%u  tcp=%s udp=%s mtu=%u %s\n",
                    r.roomId, r.roomName.c_str(), r.playerCount, r.maxPlayers,
                    tcpText.c_str(), udpText.c_str(),
                    r.mtu, r.passwordProtected ? "[password]" : "");
        }
        fprintf(stdout, "=============================\n> ");
        fflush(stdout);
    };
    m_signal.onRelayReady    = [this](uint32_t pid) { onRelayReady(pid); };
    m_signal.onLogoutAck     = [this]() { onLogoutAck(); };
    m_signal.onServerPasswordRequired = [this]() {
        fprintf(stdout, "\n* Server requires auth password. Type: server-password <password>\n> ");
        fflush(stdout);
    };
    m_signal.onSecureSessionEstablished = [this](uint32_t sessionId, const Buffer& master) {
        m_tunnel.setSecureSession(sessionId, master);
        m_dataChannel.setSecureSession(sessionId, master);
    };
    m_signal.onServerError   = [this](const std::string& msg) {
        fprintf(stdout, "* Server error: %s\n> ", msg.c_str());
        fflush(stdout);
        if (m_pendingResumeRoom) {
            m_pendingResumeRoom = false;
            clearResumeLease();
            if (m_wasInRoom && !m_savedRoomName.empty()) {
                m_wantReconnect = true;
                m_signal.listRooms();
                return;
            }
        }
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
    m_dataChannel.onRelayData      = [this](uint32_t src, TrafficClass cls, Buffer data) {
        handleTcpRelayReceived(src, cls, data);
    };

    m_tunnel.onTunnelDead = [this](uint32_t pid, TrafficClass cls) { onTransportDead(pid, cls); };
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
        if (m_hasResumeLease && m_resumeLeaseDeadlineMs != 0)
            expireResumeLeaseIfNeeded();

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

        if (m_currentRoomId != 0 &&
            now - m_lastTcpRelayCheckTime >= static_cast<uint32_t>(TCP_RELAY_KEEPALIVE_MS / 2)) {
            m_lastTcpRelayCheckTime = now;
            for (CliPeerConnection* peer : m_tunnel.allPeers()) {
                peer->sendTcpRelayKeepalive();
                for (int cls = TRAFFIC_TCP; cls <= TRAFFIC_UDP; ++cls) {
                    TrafficClass trafficClass = static_cast<TrafficClass>(cls);
                    if (peer->isTcpRelayDead(trafficClass)) {
                        onTransportDead(peer->peerId(), trafficClass);
                        break;
                    }
                }
            }
        }

        if (m_currentRoomId != 0 && now - m_lastLatencyCheckTime >= 3000) {
            m_lastLatencyCheckTime = now;
            for (CliPeerConnection* peer : m_tunnel.allPeers())
                peer->sendLatencyPing();
        }

        if (m_logoutPending && now >= m_logoutDeadline) {
            finishGracefulDisconnect();
        }

        if (m_wantReconnect && m_signal.fd() == SOCK_INVALID
            && now >= m_nextReconnectTime) {
            if (m_reconnectAttempts >= MAX_RECONNECT_ATTEMPTS) {
                m_wantReconnect = false;
                m_pendingResumeRoom = false;
                expireResumeLeaseIfNeeded();
                if (m_wasInRoom && hasUsableResumeLease()) {
                    fprintf(stdout, "* Auto-reconnect stopped after %d attempts. "
                            "Type 'connect' while the %d-second lease is valid to try restoring the original IP.\n> ",
                            MAX_RECONNECT_ATTEMPTS, RECONNECT_LEASE_TIMEOUT_SEC);
                } else {
                    fprintf(stdout, "* Auto-reconnect failed after %d attempts. "
                            "Type 'connect' to retry manually.\n> ",
                            MAX_RECONNECT_ATTEMPTS);
                }
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
    } else if (cmd == "disconnect") {
        doDisconnect();
    } else if (cmd == "server") {
        std::string first, second, password;
        iss >> first >> second >> password;
        if (first.empty()) {
            fprintf(stdout, "* Usage: server <host[:port]> [password] OR server <host> <port> [password]\n> ");
            fflush(stdout);
            return;
        }
        std::string host = first;
        uint16_t port = DEFAULT_PORT;
        int parsedPort = 0;
        if (!second.empty() && parseIntStrict(second, &parsedPort)) {
            if (parsedPort < 1 || parsedPort > 65535) {
                fprintf(stdout, "* Invalid port\n> ");
                fflush(stdout);
                return;
            }
            port = static_cast<uint16_t>(parsedPort);
        } else {
            size_t colon = first.rfind(':');
            if (colon != std::string::npos) {
                host = first.substr(0, colon);
                std::string portText = first.substr(colon + 1);
                if (!parseIntStrict(portText, &parsedPort) || parsedPort < 1 || parsedPort > 65535) {
                    fprintf(stdout, "* Invalid port\n> ");
                    fflush(stdout);
                    return;
                }
                port = static_cast<uint16_t>(parsedPort);
            }
            password = second;
        }
        if (host.empty()) {
            fprintf(stdout, "* Invalid server host\n> ");
            fflush(stdout);
            return;
        }
        if (m_signal.isConnected())
            beginGracefulDisconnect(false);
        teardownTun();
        m_currentRoomId = 0;
        m_myVirtualIP = 0;
        setServer(host, port);
        if (!password.empty()) {
            setServerPassword(password);
            m_signal.setServerPassword(password);
        }
        fprintf(stdout, "* Server set to %s:%u%s\n> ", m_serverHost.c_str(), m_port,
                m_serverPassword.empty() ? "" : " (password cached)");
        fflush(stdout);
    } else if (cmd == "server-password") {
        std::string password;
        iss >> password;
        if (password.empty()) {
            fprintf(stdout, "* Usage: server-password <password>\n> ");
            fflush(stdout);
            return;
        }
        setServerPassword(password);
        m_signal.setServerPassword(password);
        m_signal.continueServerAuth();
        fprintf(stdout, "* Server password cached for this process\n> ");
        fflush(stdout);
    } else if (cmd == "quit" || cmd == "exit" || cmd == "q") {
        fprintf(stdout, "Exiting...\n");
        beginGracefulDisconnect(true);
    } else {
        fprintf(stdout, "Unknown command: %s (type 'help')\n> ", cmd.c_str());
        fflush(stdout);
    }
}

void CliApp::printHelp() {
    fprintf(stdout,
        "\n=== VLan CLI Commands ===\n"
        "  connect                                    - (Re)connect to server\n"
        "  disconnect                                 - Disconnect from server\n"
        "  server <host[:port]> [password]            - Set server endpoint\n"
        "  server <host> <port> [password]            - Set server endpoint\n"
        "  server-password <password>                 - Set cached server auth password\n"
        "  list                                       - List rooms\n"
        "  create <name> [max] [password] [mtu] [opts] - Create room\n"
        "      opts: tcp=raw|kcp|tcp udp=raw|kcp|tcp tcp-fec=none|30 udp-fec=none|30\n"
        "            tcp-profile=realtime|bulk udp-profile=realtime|bulk mtu=1280|1400|1420\n"
        "      fec:  none,10,30,50,70,100,200 (or 0..6)\n"
        "      mtu:  1280=safe 1400=balanced 1420=aggressive (default 1400)\n"
        "      password: optional room access password\n"
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
    std::string tcpText = policyText(m_tcpPolicy);
    std::string udpText = policyText(m_udpPolicy);
    fprintf(stdout, "  TCP policy: %s\n", tcpText.c_str());
    fprintf(stdout, "  UDP policy: %s\n", udpText.c_str());
    fprintf(stdout, "  MTU:       %u\n", m_roomMtu);
    fprintf(stdout, "--------------\n> ");
    fflush(stdout);
}

void CliApp::printPeers() {
    auto peers = m_tunnel.allPeers();
    fprintf(stdout, "\n--- Peers (%zu) ---\n", peers.size());
    for (CliPeerConnection* p : peers) {
        fprintf(stdout, "  [%u] %s  IP=%s  TCP=%s RTT=%dms  UDP=%s RTT=%dms\n",
                p->peerId(), p->name().c_str(),
                ipToString(p->virtualIP()).c_str(),
                transportName(p->transport(TRAFFIC_TCP)),
                p->latencyMs(TRAFFIC_TCP),
                transportName(p->transport(TRAFFIC_UDP)),
                p->latencyMs(TRAFFIC_UDP));
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
    if (!m_wantReconnect) {
        expireResumeLeaseIfNeeded();
        if (!(m_wasInRoom && hasUsableResumeLease()))
            clearResumeLease();
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
    m_dataChannel.disconnect();
    m_tunnel.setSecureSession(0, Buffer());
    m_dataChannel.setSecureSession(0, Buffer());
    m_signal.setServerPassword(m_serverPassword);

    fprintf(stdout, "* Connecting to %s:%u ...\n", m_resolvedIP.c_str(), m_port);
    fflush(stdout);

    if (!m_signal.connectTo(m_resolvedIP, m_port)) {
        fprintf(stdout, "* Connect failed\n> ");
        fflush(stdout);
        if (m_wantReconnect)
            m_nextReconnectTime = currentTimeMs() + RECONNECT_INTERVAL_MS;
    }
}

void CliApp::doDisconnect() {
    if (!m_signal.isConnected() && m_signal.fd() == SOCK_INVALID) {
        fprintf(stdout, "* Already disconnected\n> ");
        fflush(stdout);
        return;
    }
    beginGracefulDisconnect(false);
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
    uint16_t roomMtu = ROOM_MTU_DEFAULT;
    std::string password;
    RoomTrafficPolicy tcpPolicy = makeDefaultTcpPolicy();
    RoomTrafficPolicy udpPolicy = makeDefaultUdpPolicy();

    iss >> roomName;
    if (roomName.empty()) roomName = "CLIRoom";

    std::string token;
    bool maxSet = false;
    bool passwordSet = false;
    while (iss >> token) {
        size_t eq = token.find('=');
        if (eq != std::string::npos) {
            std::string key = token.substr(0, eq);
            std::string value = token.substr(eq + 1);
            std::string error;
            if (!applyPolicyToken(key, value, &tcpPolicy, &udpPolicy,
                                  &roomMtu, &password, &error)) {
                fprintf(stdout, "* %s\n> ", error.c_str());
                fflush(stdout);
                return;
            }
            std::string normalizedKey = toLowerCopy(key);
            normalizedKey.erase(std::remove(normalizedKey.begin(), normalizedKey.end(), '_'), normalizedKey.end());
            normalizedKey.erase(std::remove(normalizedKey.begin(), normalizedKey.end(), '-'), normalizedKey.end());
            if (normalizedKey == "password" || normalizedKey == "pass" || normalizedKey == "roompassword")
                passwordSet = true;
            continue;
        }

        int parsed = 0;
        if (!maxSet && parseIntStrict(token, &parsed)) {
            if (parsed >= 2 && parsed <= MAX_PLAYERS) {
                maxPlayers = parsed;
                maxSet = true;
                continue;
            }
            if (isValidRoomMtuValue(parsed)) {
                roomMtu = normalizeRoomMtu(parsed);
                continue;
            }
            maxPlayers = parsed;
            maxSet = true;
            continue;
        }

        if (!passwordSet && parseIntStrict(token, &parsed) && isValidRoomMtuValue(parsed)) {
            roomMtu = normalizeRoomMtu(parsed);
            continue;
        }

        if (!passwordSet) {
            password = token;
            passwordSet = true;
            continue;
        }

        if (!parseIntStrict(token, &parsed) || !isValidRoomMtuValue(parsed)) {
            fprintf(stdout, "* Invalid create argument: %s\n> ", token.c_str());
            fflush(stdout);
            return;
        }
        roomMtu = normalizeRoomMtu(parsed);
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

    bool passwordProtected = !password.empty();
    uint8_t hashBuf[CIPHER_KEY_SIZE];
    const uint8_t* pwdHash = nullptr;
    if (passwordProtected) {
        if (!isValidRoomPassword(password)) {
            fprintf(stdout, "* Password must be %d-%d ASCII letters or digits\n> ",
                    MIN_ROOM_PASSWORD_LEN, MAX_ROOM_PASSWORD_LEN);
            fflush(stdout);
            return;
        }
        uint8_t intermediate[CIPHER_KEY_SIZE];
        computeIntermediate(reinterpret_cast<const uint8_t*>(password.data()),
                            password.size(), intermediate);
        authHashFromIntermediate(intermediate, hashBuf);
        crypto_wipe(intermediate, sizeof(intermediate));
        pwdHash = hashBuf;
        m_roomPassword = password;
    } else {
        m_roomPassword.clear();
    }

    m_savedRoomName = roomName;
    m_savedMaxPlayers = static_cast<uint8_t>(maxPlayers);
    m_savedRoomMtu = roomMtu;
    m_savedTcpPolicy = normalizeTrafficPolicy(
        tcpPolicy.transportMode, tcpPolicy.fecMode, tcpPolicy.kcpProfile,
        makeDefaultTcpPolicy());
    m_savedUdpPolicy = normalizeTrafficPolicy(
        udpPolicy.transportMode, udpPolicy.fecMode, udpPolicy.kcpProfile,
        makeDefaultUdpPolicy());
    m_savedRoomPasswordProtected = passwordProtected;

    std::string tcpText = policyText(m_savedTcpPolicy);
    std::string udpText = policyText(m_savedUdpPolicy);
    fprintf(stdout, "* Creating room \"%s\" tcp=%s udp=%s mtu=%u %s...\n",
            roomName.c_str(), tcpText.c_str(), udpText.c_str(), roomMtu,
            passwordProtected ? "[password]" : "");
    fflush(stdout);

    m_signal.createRoom(roomName, static_cast<uint8_t>(maxPlayers),
                        m_savedTcpPolicy, m_savedUdpPolicy,
                        roomMtu, passwordProtected, pwdHash);
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
            m_savedTcpPolicy = m_cachedRoomList[i].tcpPolicy;
            m_savedUdpPolicy = m_cachedRoomList[i].udpPolicy;
            m_savedRoomPasswordProtected = m_cachedRoomList[i].passwordProtected != 0;
            m_savedRoomId = m_cachedRoomList[i].roomId;
            m_roomMtu = m_savedRoomMtu;
            if (m_cachedRoomList[i].passwordProtected && password.empty()) {
                fprintf(stdout, "* This room requires a password. Usage: join <roomId> <password>\n> ");
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
        uint8_t intermediate[CIPHER_KEY_SIZE];
        computeIntermediate(reinterpret_cast<const uint8_t*>(password.data()),
                            password.size(), intermediate);
        authHashFromIntermediate(intermediate, hashBuf);
        crypto_wipe(intermediate, sizeof(intermediate));
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
    m_tunnel.setSecureSession(0, Buffer());
    m_pendingResumeRoom = false;
    clearResumeLease();
    m_currentRoomId = 0;
    m_myVirtualIP = 0;
    m_roomMtu = ROOM_MTU_DEFAULT;
    m_roomPasswordProtected = false;
    m_roomPassword.clear();
    m_savedRoomName.clear();
    m_wasInRoom = false;
    fprintf(stdout, "* Left room\n> ");
    fflush(stdout);
}

// ───────── Signal callbacks ─────────

void CliApp::beginGracefulDisconnect(bool exitAfterDisconnect) {
    m_wantReconnect = false;
    m_pendingResumeRoom = false;
    clearResumeLease();
    m_manualDisconnecting = true;
    m_exitAfterDisconnect = exitAfterDisconnect;

    if (m_signal.isConnected() && m_signal.myPeerId() != 0) {
        if (!m_logoutPending) {
            m_logoutPending = true;
            m_logoutDeadline = currentTimeMs() + 1000;
            m_signal.logout();
        }
        return;
    }

    finishGracefulDisconnect();
}

void CliApp::finishGracefulDisconnect() {
    m_logoutPending = false;
    m_logoutDeadline = 0;
    m_dataChannel.disconnect();
    m_dataChannel.setSecureSession(0, Buffer());
    m_tunnel.setSecureSession(0, Buffer());
    teardownTun();
    if (m_signal.fd() != SOCK_INVALID)
        m_signal.disconnect();
    if (m_exitAfterDisconnect) {
        m_running = false;
    } else {
        m_manualDisconnecting = false;
        m_exitAfterDisconnect = false;
        m_currentRoomId = 0;
        m_myVirtualIP = 0;
        m_roomMtu = ROOM_MTU_DEFAULT;
        m_roomPasswordProtected = false;
        m_roomPassword.clear();
        m_wasInRoom = false;
        m_savedRoomName.clear();
        fprintf(stdout, "* Disconnected from server\n> ");
        fflush(stdout);
    }
}

void CliApp::onSignalConnected() {
    fprintf(stdout, "* Connected, logging in as \"%s\"...\n", m_playerName.c_str());
    fflush(stdout);
    bool canResume = m_wasInRoom && hasUsableResumeLease();
    m_signal.login(m_playerName, canResume,
                   canResume ? m_resumeRoomId : 0,
                   canResume ? m_resumePeerId : 0,
                   canResume ? m_resumeToken.data() : nullptr);
}

void CliApp::onSignalDisconnected() {
    bool wasInRoom = (m_currentRoomId != 0);
    bool manualDisconnect = m_manualDisconnecting;

    if (manualDisconnect) {
        m_wasInRoom = false;
        m_savedRoomId = 0;
        m_savedRoomName.clear();
        m_savedRoomPasswordProtected = false;
    } else if (!m_wantReconnect) {
        if (wasInRoom) {
            m_wasInRoom = true;
            m_savedRoomId = m_currentRoomId;
            m_savedTcpPolicy = m_tcpPolicy;
            m_savedUdpPolicy = m_udpPolicy;
            m_savedRoomMtu = m_roomMtu;
            m_savedRoomPasswordProtected = m_roomPasswordProtected;
        } else {
            m_wasInRoom = false;
        }
    }
    if (!manualDisconnect && wasInRoom && m_hasResumeLease)
        startResumeLeaseDeadline();

    teardownTun();
    m_dataChannel.disconnect();
    m_dataChannel.setSecureSession(0, Buffer());
    m_tunnel.setSecureSession(0, Buffer());
    m_currentRoomId = 0;
    m_myVirtualIP = 0;
    m_roomMtu = ROOM_MTU_DEFAULT;
    m_logoutPending = false;

    if (manualDisconnect) {
        m_manualDisconnecting = false;
        if (m_exitAfterDisconnect) {
            m_running = false;
        } else {
            m_exitAfterDisconnect = false;
            fprintf(stdout, "* Disconnected from server\n> ");
            fflush(stdout);
        }
    } else if (!m_wantReconnect && !m_playerName.empty()) {
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

void CliApp::onLoginResponse(uint32_t peerId, bool resumeAccepted) {
    fprintf(stdout, "* Logged in, peerId=%u\n", peerId);
    fflush(stdout);
    m_tunnel.setMyPeerId(peerId);

    bool canResume = m_wasInRoom && hasUsableResumeLease();
    if (m_wantReconnect || canResume) {
        if (resumeAccepted && canResume) {
            m_pendingResumeRoom = true;
            fprintf(stdout, "* Reconnected. Resuming room %u...\n", m_resumeRoomId);
            fflush(stdout);
            m_signal.resumeRoom(m_resumeRoomId, m_resumePeerId, m_resumeToken.data());
        } else if (m_wasInRoom && !m_savedRoomName.empty()) {
            if (canResume && !resumeAccepted)
                clearResumeLease();
            m_wantReconnect = true;
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
    if (m_signal.secureEnabled())
        m_dataChannel.setSecureSession(m_signal.secureSessionId(), m_signal.secureMaster());
    m_dataChannel.connectTo(m_resolvedIP, m_port, peerId);
}

void CliApp::onRoomCreated(uint32_t roomId, uint32_t virtualIP,
                           RoomTrafficPolicy tcpPolicy,
                           RoomTrafficPolicy udpPolicy,
                           uint16_t mtu,
                           bool passwordProtected,
                           const Buffer& leaseToken) {
    m_currentRoomId = roomId;
    m_savedRoomId = roomId;
    m_myVirtualIP = virtualIP;
    m_tcpPolicy = tcpPolicy;
    m_udpPolicy = udpPolicy;
    m_roomMtu = normalizeRoomMtu(mtu);
    m_savedRoomMtu = m_roomMtu;
    m_roomPasswordProtected = passwordProtected;
    rememberResumeLease(roomId, m_signal.myPeerId(), virtualIP, leaseToken);
    m_tunnel.setMyVirtualIP(virtualIP);

    setupTun();
    fprintf(stdout, "* Room created (ID=%u, IP=%s, MTU=%u)\n> ",
            roomId, ipToString(virtualIP).c_str(), m_roomMtu);
    fflush(stdout);
}

void CliApp::onJoinResponse(uint32_t roomId, uint32_t virtualIP,
                            RoomTrafficPolicy tcpPolicy,
                            RoomTrafficPolicy udpPolicy,
                            uint16_t mtu,
                            bool passwordProtected,
                            const std::vector<PeerInfo>& members,
                            const Buffer& leaseToken) {
    m_pendingResumeRoom = false;
    m_wantReconnect = false;
    m_currentRoomId = roomId;
    m_savedRoomId = roomId;
    m_myVirtualIP = virtualIP;
    m_tcpPolicy = tcpPolicy;
    m_udpPolicy = udpPolicy;
    m_roomMtu = normalizeRoomMtu(mtu);
    m_savedRoomMtu = m_roomMtu;
    m_roomPasswordProtected = passwordProtected;
    rememberResumeLease(roomId, m_signal.myPeerId(), virtualIP, leaseToken);
    m_tunnel.setMyVirtualIP(virtualIP);

    setupTun();

    fprintf(stdout, "* Joined room (ID=%u, IP=%s, MTU=%u, %zu members)\n> ",
            roomId, ipToString(virtualIP).c_str(), m_roomMtu, members.size());
    fflush(stdout);

    for (const PeerInfo& pi : members) {
        if (pi.peerId == m_signal.myPeerId()) continue;
        std::string name = pi.name.empty() ? ("Peer" + std::to_string(pi.peerId)) : pi.name;
        m_tunnel.addPeer(pi.peerId, pi.virtualIP, name);
        m_signal.requestRelay(pi.peerId);
    }
}

void CliApp::onPeerJoined(PeerInfo info) {
    std::string name = info.name.empty() ? ("Peer" + std::to_string(info.peerId)) : info.name;
    m_tunnel.addPeer(info.peerId, info.virtualIP, name);

    fprintf(stdout, "* Player %s joined (IP=%s)\n> ",
            name.c_str(), ipToString(info.virtualIP).c_str());
    fflush(stdout);
    m_signal.requestRelay(info.peerId);
}

void CliApp::onRelayReady(uint32_t peerId) {
    CliPeerConnection* peer = m_tunnel.peerById(peerId);
    if (!peer) return;

    bool rebuildTcp = takePendingRebuild(peerId, TRAFFIC_TCP);
    bool rebuildUdp = takePendingRebuild(peerId, TRAFFIC_UDP);
    bool initialSetup = !rebuildTcp && !rebuildUdp;
    bool changed = false;

    if ((initialSetup || rebuildTcp) && peer->transport(TRAFFIC_TCP) == TRANSPORT_NONE) {
        setupPolicyTunnel(peerId, TRAFFIC_TCP);
        changed = true;
    }
    if ((initialSetup || rebuildUdp) && peer->transport(TRAFFIC_UDP) == TRANSPORT_NONE) {
        setupPolicyTunnel(peerId, TRAFFIC_UDP);
        changed = true;
    }

    if (!changed)
        return;

    fprintf(stdout, "* %s relay ready (TCP=%s UDP=%s IP=%s)\n> ",
            peer->name().c_str(), transportName(peer->transport(TRAFFIC_TCP)),
            transportName(peer->transport(TRAFFIC_UDP)),
            ipToString(peer->virtualIP()).c_str());
    fflush(stdout);
}

void CliApp::onLogoutAck() {
    if (m_logoutPending)
        finishGracefulDisconnect();
}

void CliApp::onPeerLeft(uint32_t peerId) {
    CliPeerConnection* peer = m_tunnel.peerById(peerId);
    std::string name = peer ? peer->name() : ("Peer" + std::to_string(peerId));
    clearPendingRebuild(peerId);
    m_tunnel.removePeer(peerId);
    fprintf(stdout, "* Player %s left\n> ", name.c_str());
    fflush(stdout);
}

void CliApp::onTransportDead(uint32_t peerId, TrafficClass cls) {
    CliPeerConnection* peer = m_tunnel.peerById(peerId);
    if (!peer || peer->transport(cls) == TRANSPORT_NONE) return;

    uint64_t key = transportKey(peerId, cls);
    if (m_pendingRebuild.count(key))
        return;

    std::string name = peer->name();
    TransportType deadTransport = peer->transport(cls);
    m_pendingRebuild.insert(key);

    LOG_INFO("%s %s transport dead, rebuilding class=%u...",
             name.c_str(), transportName(deadTransport), static_cast<unsigned>(cls));

    m_tunnel.removeTransport(peerId, cls);

    m_signal.requestRelay(peerId);
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
    m_pendingRebuild.clear();
    m_tunnel.removeAllPeers();
    if (m_tun) {
        m_tunnel.setTunAdapter(nullptr);
        m_tun->shutdown();
        delete m_tun;
        m_tun = nullptr;
    }
}

// ───────── Relay tunnel setup ─────────

void CliApp::setupPolicyTunnel(uint32_t peerId, TrafficClass cls) {
    CliPeerConnection* peer = m_tunnel.peerById(peerId);
    if (!peer || peer->transport(cls) != TRANSPORT_NONE)
        return;
    RoomTrafficPolicy policy = cls == TRAFFIC_TCP ? m_tcpPolicy : m_udpPolicy;
    policy = normalizeTrafficPolicy(policy.transportMode, policy.fecMode,
                                    policy.kcpProfile,
                                    cls == TRAFFIC_TCP ? makeDefaultTcpPolicy()
                                                       : makeDefaultUdpPolicy());
    if (policy.transportMode == MODE_RELAY_TCP) {
        setupTcpRelayTunnel(peerId, cls);
    } else if (policy.transportMode == MODE_RELAY_RAW_UDP) {
        setupRawUdpRelayTunnel(peerId, cls);
    } else {
        setupRelayTunnel(peerId, cls);
    }
}

void CliApp::setupRelayTunnel(uint32_t peerId, TrafficClass cls) {
    CliPeerConnection* peer = m_tunnel.peerById(peerId);
    if (!peer || peer->transport(cls) != TRANSPORT_NONE) return;

    uint32_t serverIPn;
    inet_pton(AF_INET, m_resolvedIP.c_str(), &serverIPn);
    uint32_t serverIP = ntohl(serverIPn);
    RoomTrafficPolicy policy = cls == TRAFFIC_TCP ? m_tcpPolicy : m_udpPolicy;

    CliKcpTunnel* kcp = m_tunnel.createKcpTunnel(
        peer, serverIP, m_port, TRANSPORT_RELAY_KCP,
        policy.fecMode, m_roomMtu, policy.kcpProfile, cls);
    kcp->setRelayMode(m_signal.myPeerId(), peerId);
}

void CliApp::setupRawUdpRelayTunnel(uint32_t peerId, TrafficClass cls) {
    CliPeerConnection* peer = m_tunnel.peerById(peerId);
    if (!peer || peer->transport(cls) != TRANSPORT_NONE) return;

    uint32_t serverIPn;
    inet_pton(AF_INET, m_resolvedIP.c_str(), &serverIPn);
    uint32_t serverIP = ntohl(serverIPn);
    RoomTrafficPolicy policy = cls == TRAFFIC_TCP ? m_tcpPolicy : m_udpPolicy;

    CliRawUdpTunnel* tunnel = m_tunnel.createRawUdpTunnel(
        peer, serverIP, m_port, policy.fecMode, m_roomMtu, cls);
    tunnel->setRelayMode(m_signal.myPeerId(), peerId);
}

void CliApp::setupTcpRelayTunnel(uint32_t peerId, TrafficClass cls) {
    CliPeerConnection* peer = m_tunnel.peerById(peerId);
    if (!peer || peer->transport(cls) != TRANSPORT_NONE) return;

    uint32_t myId = m_signal.myPeerId();
    peer->setTcpRelaySender([this, myId](uint32_t dstPeerId, TrafficClass trafficClass,
                                         const Buffer& data) {
        if (m_dataChannel.isConnected())
            m_dataChannel.sendRelayData(myId, dstPeerId, trafficClass, data);
    });
    peer->setTransport(cls, TRANSPORT_RELAY_TCP);
}

void CliApp::handleReconnectRoomList(const std::vector<CliRoomListItem>& rooms) {
    m_wantReconnect = false;

    std::vector<uint32_t> matches;
    for (size_t i = 0; i < rooms.size(); ++i) {
        const CliRoomListItem& item = rooms[i];
        if (item.roomName == m_savedRoomName &&
            item.maxPlayers == m_savedMaxPlayers &&
            normalizeRoomMtu(item.mtu) == m_savedRoomMtu &&
            (item.passwordProtected != 0) == m_savedRoomPasswordProtected &&
            samePolicy(item.tcpPolicy, m_savedTcpPolicy) &&
            samePolicy(item.udpPolicy, m_savedUdpPolicy)) {
            matches.push_back(item.roomId);
        }
    }

    uint8_t hashBuf[CIPHER_KEY_SIZE];
    const uint8_t* hash = nullptr;
    if (m_savedRoomPasswordProtected && !m_roomPassword.empty()) {
        uint8_t intermediate[CIPHER_KEY_SIZE];
        computeIntermediate(reinterpret_cast<const uint8_t*>(m_roomPassword.data()),
                            m_roomPassword.size(), intermediate);
        authHashFromIntermediate(intermediate, hashBuf);
        crypto_wipe(intermediate, sizeof(intermediate));
        hash = hashBuf;
    }

    if (matches.size() == 1) {
        uint32_t foundRoomId = matches.front();
        fprintf(stdout, "* Found room \"%s\" (ID=%u), rejoining...\n",
                m_savedRoomName.c_str(), foundRoomId);
        fflush(stdout);
        m_signal.joinRoom(foundRoomId, hash);
    } else if (matches.size() > 1) {
        fprintf(stdout, "* Multiple matching rooms found. Type 'list' and choose manually.\n> ");
        fflush(stdout);
    } else {
        fprintf(stdout, "* Room \"%s\" not found, creating with same settings...\n",
                m_savedRoomName.c_str());
        fflush(stdout);
        m_signal.createRoom(m_savedRoomName, m_savedMaxPlayers,
                            m_savedTcpPolicy, m_savedUdpPolicy,
                            m_savedRoomMtu, m_savedRoomPasswordProtected, hash);
    }
}

void CliApp::handleTcpRelayReceived(uint32_t srcPeerId, TrafficClass cls, Buffer data) {
    CliPeerConnection* peer = m_tunnel.peerById(srcPeerId);
    if (!peer) return;
    peer->onTcpRelayDataReceived(cls);
    if (data.empty()) return;

    if (data.size() >= 2 && data[0] == CLI_LATENCY_PROBE_MARKER) {
        peer->handleLatencyProbe(cls, data);
        return;
    }

    if (m_tun) m_tun->writePacket(data);
}

uint64_t CliApp::transportKey(uint32_t peerId, TrafficClass cls) {
    return (static_cast<uint64_t>(peerId) << 8) | static_cast<uint8_t>(cls);
}

void CliApp::clearPendingRebuild(uint32_t peerId) {
    for (auto it = m_pendingRebuild.begin(); it != m_pendingRebuild.end(); ) {
        if ((*it >> 8) == peerId)
            it = m_pendingRebuild.erase(it);
        else
            ++it;
    }
}

bool CliApp::takePendingRebuild(uint32_t peerId, TrafficClass cls) {
    uint64_t key = transportKey(peerId, cls);
    auto it = m_pendingRebuild.find(key);
    if (it == m_pendingRebuild.end())
        return false;
    m_pendingRebuild.erase(it);
    return true;
}
} // namespace VLan

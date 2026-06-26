#include "signal_server.h"
#include "payload_cipher.h"
#include "server_logger.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <exception>
#include <fcntl.h>
#include <unistd.h>

namespace VLan {

// ───────── Construction / destruction ─────────

SignalServer::SignalServer()
    : m_epfd(-1), m_tcpListenFd(-1),
      m_stunFd(-1),
      m_running(false), m_nextPeerId(1)
{}

SignalServer::~SignalServer() { stop(); }

// ───────── Socket helpers ─────────

int SignalServer::createTcpListener(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    setReuseAddr(fd);
    setNonBlocking(fd);
    struct sockaddr_in addr = makeAddr(INADDR_ANY, port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { close(fd); return -1; }
    if (listen(fd, 64) < 0) { close(fd); return -1; }
    return fd;
}

int SignalServer::createUdpSocket(uint16_t port) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    setReuseAddr(fd);
    setNonBlocking(fd);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { close(fd); return -1; }
    return fd;
}

void SignalServer::epollAdd(int fd, uint32_t events) {
    struct epoll_event ev;
    ev.events  = events;
    ev.data.fd = fd;
    epoll_ctl(m_epfd, EPOLL_CTL_ADD, fd, &ev);
}

void SignalServer::epollDel(int fd) {
    epoll_ctl(m_epfd, EPOLL_CTL_DEL, fd, nullptr);
}

// ───────── Init ─────────

bool SignalServer::init(uint16_t port)
{
    m_epfd = epoll_create1(0);
    if (m_epfd < 0) { LOG_ERROR("epoll_create1 failed"); return false; }

    m_tcpListenFd = createTcpListener(port);
    if (m_tcpListenFd < 0) { LOG_ERROR("Cannot bind TCP %u", port); return false; }

    m_stunFd = createUdpSocket(port);
    if (m_stunFd < 0) { LOG_ERROR("Cannot bind UDP %u", port); return false; }

    epollAdd(m_tcpListenFd, EPOLLIN);
    epollAdd(m_stunFd,      EPOLLIN);

    LOG_INFO("[server] Listening on port %u (TCP+UDP) tcpFd=%d udpFd=%d",
             port, m_tcpListenFd, m_stunFd);
    return true;
}

// ───────── Main loop ─────────

void SignalServer::run() {
    m_running = true;
    const int MAX_EV = 128;
    struct epoll_event events[MAX_EV];
    time_t lastCheck = time(nullptr);

    while (m_running) {
        int n = epoll_wait(m_epfd, events, MAX_EV, 200);
        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            if (fd == m_tcpListenFd) {
                handleTcpAccept();
            } else if (fd == m_stunFd) {
                handleUdpPacket(fd);
            } else if (m_pending.count(fd)) {
                if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                    epollDel(fd); close(fd); m_pending.erase(fd);
                } else if (events[i].events & EPOLLIN) {
                    handlePendingData(fd);
                }
            } else if (m_dataFdMap.count(fd)) {
                if (events[i].events & (EPOLLERR | EPOLLHUP))
                    handleDataChannelDisconnect(fd);
                else {
                    if (events[i].events & EPOLLOUT)
                        handleWritable(fd);
                    if (events[i].events & EPOLLIN)
                        handleDataChannelData(fd);
                }
            } else {
                if (events[i].events & (EPOLLERR | EPOLLHUP))
                    handleClientDisconnect(fd);
                else {
                    if (events[i].events & EPOLLOUT)
                        handleWritable(fd);
                    if (events[i].events & EPOLLIN)
                        handleClientData(fd);
                }
            }
        }

        time_t now = time(nullptr);
        if (now - lastCheck >= 5) {
            checkTimeouts();
            lastCheck = now;
        }
    }
}

void SignalServer::stop() {
    m_running = false;
    if (m_tcpListenFd >= 0) { close(m_tcpListenFd); m_tcpListenFd = -1; }
    if (m_stunFd >= 0)      { close(m_stunFd);      m_stunFd = -1; }
    for (auto& kv : m_clients) {
        if (kv.second.dataFd >= 0) close(kv.second.dataFd);
        close(kv.first);
    }
    for (auto& kv : m_pending) close(kv.first);
    m_clients.clear();
    m_dataFdMap.clear();
    m_pending.clear();
    m_peerMap.clear();
    if (m_epfd >= 0) { close(m_epfd); m_epfd = -1; }
}

// ───────── TCP signaling ─────────

void SignalServer::handleTcpAccept() {
    struct sockaddr_in addr;
    socklen_t addrLen = sizeof(addr);
    int fd = accept(m_tcpListenFd, (struct sockaddr*)&addr, &addrLen);
    if (fd < 0) return;

    setNonBlocking(fd);
    setTcpNoDelay(fd);
    epollAdd(fd, EPOLLIN | EPOLLERR | EPOLLHUP);

    PendingConn& pc = m_pending[fd];
    pc.fd      = fd;
    pc.created = time(nullptr);
    LOG_INFO("[server] New connection fd=%d from %s (pending classification)",
             fd, addrToString(addr).c_str());
}

void SignalServer::handlePendingData(int fd) {
    auto it = m_pending.find(fd);
    if (it == m_pending.end()) return;
    PendingConn& pc = it->second;

    char buf[4096];
    int n = recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) {
        epollDel(fd);
        close(fd);
        m_pending.erase(it);
        return;
    }

    pc.recvBuf.insert(pc.recvBuf.end(), buf, buf + n);

    if (pc.recvBuf.size() < sizeof(TcpMsgHeader)) return;

    const TcpMsgHeader* hdr = reinterpret_cast<const TcpMsgHeader*>(pc.recvBuf.data());
    uint16_t payloadLen = ntohs(hdr->length);
    if (payloadLen > MAX_TCP_MSG_PAYLOAD) {
        epollDel(fd);
        close(fd);
        m_pending.erase(it);
        return;
    }
    size_t frameLen = sizeof(TcpMsgHeader) + payloadLen;
    if (pc.recvBuf.size() < frameLen) return;

    uint8_t msgType = hdr->msgType;

    if (msgType == MSG_DATA_CHANNEL_INIT) {
        onDataChannelInit(fd, pc.recvBuf.data() + sizeof(TcpMsgHeader), payloadLen);
        m_pending.erase(it);
        return;
    }

    // Signaling connection: promote to m_clients
    ClientSession& c = m_clients[fd];
    c.tcpFd    = fd;
    c.lastPing = time(nullptr);
    c.alive    = true;
    c.recvBuf  = std::move(pc.recvBuf);
    m_pending.erase(it);

    LOG_DETAIL("[server] Connection fd=%d classified as signaling", fd);

    // Process buffered first message and any remaining
    while (c.recvBuf.size() >= sizeof(TcpMsgHeader)) {
        const TcpMsgHeader* h = reinterpret_cast<const TcpMsgHeader*>(c.recvBuf.data());
        uint16_t pLen = ntohs(h->length);
        if (pLen > MAX_TCP_MSG_PAYLOAD) {
            handleClientDisconnect(fd);
            return;
        }
        size_t fLen = sizeof(TcpMsgHeader) + pLen;
        if (c.recvBuf.size() < fLen) break;
        processMessage(c, h->msgType, c.recvBuf.data() + sizeof(TcpMsgHeader), pLen);
        if (!c.alive) {
            handleClientDisconnect(fd);
            return;
        }
        c.recvBuf.erase(c.recvBuf.begin(), c.recvBuf.begin() + fLen);
    }
}

void SignalServer::handleClientData(int fd) {
    auto it = m_clients.find(fd);
    if (it == m_clients.end()) return;
    ClientSession& c = it->second;

    char buf[65536];
    int n = recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) { handleClientDisconnect(fd); return; }

    c.recvBuf.insert(c.recvBuf.end(), buf, buf + n);

    while (c.recvBuf.size() >= sizeof(TcpMsgHeader)) {
        const TcpMsgHeader* hdr = reinterpret_cast<const TcpMsgHeader*>(c.recvBuf.data());
        uint16_t payloadLen = ntohs(hdr->length);
        if (payloadLen > MAX_TCP_MSG_PAYLOAD) {
            LOG_ERROR("[server] Stream corrupted from peer %u, disconnecting", c.peerId);
            handleClientDisconnect(fd);
            return;
        }
        size_t frameLen = sizeof(TcpMsgHeader) + payloadLen;
        if (c.recvBuf.size() < frameLen) break;

        processMessage(c, hdr->msgType,
                       c.recvBuf.data() + sizeof(TcpMsgHeader), payloadLen);
        if (!c.alive) {
            handleClientDisconnect(fd);
            return;
        }

        c.recvBuf.erase(c.recvBuf.begin(), c.recvBuf.begin() + frameLen);
    }
}

void SignalServer::handleDataChannelData(int fd) {
    auto dit = m_dataFdMap.find(fd);
    if (dit == m_dataFdMap.end()) return;
    ClientSession& c = *dit->second;

    char buf[65536];
    int n = recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) { handleDataChannelDisconnect(fd); return; }

    c.dataRecvBuf.insert(c.dataRecvBuf.end(), buf, buf + n);

    while (c.dataRecvBuf.size() >= sizeof(TcpMsgHeader)) {
        const TcpMsgHeader* hdr = reinterpret_cast<const TcpMsgHeader*>(c.dataRecvBuf.data());
        uint16_t payloadLen = ntohs(hdr->length);
        if (payloadLen > MAX_TCP_MSG_PAYLOAD) {
            LOG_ERROR("[server] Data channel stream corrupted from peer %u", c.peerId);
            handleDataChannelDisconnect(fd);
            return;
        }
        size_t frameLen = sizeof(TcpMsgHeader) + payloadLen;
        if (c.dataRecvBuf.size() < frameLen) break;

        processDataMessage(c, hdr->msgType,
                           c.dataRecvBuf.data() + sizeof(TcpMsgHeader), payloadLen);
        if (c.dataFd < 0 || c.dataRecvBuf.size() < frameLen)
            return;

        c.dataRecvBuf.erase(c.dataRecvBuf.begin(), c.dataRecvBuf.begin() + frameLen);
    }
}

void SignalServer::handleClientDisconnect(int fd) {
    auto it = m_clients.find(fd);
    if (it == m_clients.end()) return;
    ClientSession& c = it->second;

    LOG_INFO("[server] Client disconnected peer=%u fd=%d roomId=%u", c.peerId, fd, c.roomId);

    if (c.dataFd >= 0) {
        m_dataFdMap.erase(c.dataFd);
        epollDel(c.dataFd);
        close(c.dataFd);
        c.dataFd = -1;
    }

    if (c.roomId != 0) onLeaveRoom(c);
    m_peerMap.erase(c.peerId);
    c.sendBuf.clear();
    c.recvBuf.clear();
    c.dataSendBuf.clear();
    c.dataRecvBuf.clear();
    epollDel(fd);
    close(fd);
    m_clients.erase(it);
}

void SignalServer::handleDataChannelDisconnect(int fd) {
    auto dit = m_dataFdMap.find(fd);
    if (dit == m_dataFdMap.end()) return;
    ClientSession& c = *dit->second;

    LOG_INFO("[server] Data channel disconnected for peer=%u fd=%d", c.peerId, fd);

    m_dataFdMap.erase(dit);
    epollDel(fd);
    close(fd);
    c.dataFd = -1;
    c.dataSendBuf.clear();
    c.dataRecvBuf.clear();
}

// ───────── UDP packets ─────────

void SignalServer::handleUdpPacket(int fd) {
    uint8_t buf[65536];
    struct sockaddr_in from;
    socklen_t fromLen = sizeof(from);
    ssize_t n = recvfrom(fd, buf, sizeof(buf), 0,
                         (struct sockaddr*)&from, &fromLen);
    if (n <= 0) return;

    uint8_t pktType = buf[0];
    LOG_DETAIL("[server] UDP pkt type=0x%02X size=%zd from %s", pktType, n, addrToString(from).c_str());

    switch (pktType) {
    case UDP_STUN_REQUEST:
        StunHandler::processRequest(fd, buf, n, from, m_peerMap);
        break;
    case UDP_RELAY_DATA:
    case UDP_RAW_RELAY_DATA:
        RelayHandler::processUdpRelay(fd, buf, n, from, m_peerMap);
        break;
    case UDP_KEEPALIVE:
        break;
    default:
        LOG_DETAIL("[server] Unknown UDP pkt type=0x%02X", pktType);
        break;
    }
}

// ───────── Timeouts ─────────

void SignalServer::checkTimeouts() {
    time_t now = time(nullptr);
    std::vector<int> dead;

    for (auto& kv : m_clients) {
        if (!kv.second.alive ||
            now - kv.second.lastPing > HEARTBEAT_TIMEOUT_MS / 1000) {
            dead.push_back(kv.first);
        }
    }
    for (int fd : dead) handleClientDisconnect(fd);

    // Clean up pending connections that never classified (10s timeout)
    std::vector<int> deadPending;
    for (auto& kv : m_pending) {
        if (now - kv.second.created > 10)
            deadPending.push_back(kv.first);
    }
    for (int fd : deadPending) {
        epollDel(fd);
        close(fd);
        m_pending.erase(fd);
    }

    m_rooms.cleanupEmptyRooms();
}

// ───────── Message dispatch ─────────

void SignalServer::processMessage(ClientSession& c, uint8_t msgType,
                                  const uint8_t* payload, size_t len)
{
    c.lastPing = time(nullptr);

    try {
        switch (msgType) {
        case MSG_LOGIN:         onLogin(c, payload, len);        break;
        case MSG_CREATE_ROOM:   onCreateRoom(c, payload, len);   break;
        case MSG_JOIN_ROOM:     onJoinRoom(c, payload, len);     break;
        case MSG_LEAVE_ROOM:    onLeaveRoom(c);                  break;
        case MSG_LIST_ROOMS:    onListRooms(c);                  break;
        case MSG_NAT_REPORT:    onNatReport(c, payload, len);    break;
        case MSG_PUNCH_RESULT:  onPunchResult(c, payload, len);  break;
        case MSG_REQUEST_RELAY: onRequestRelay(c, payload, len); break;
        case MSG_AUTH_RESPONSE: onAuthResponse(c, payload, len); break;
        case MSG_PING:          onPing(c);                       break;
        case MSG_TCP_RELAY_DATA:onTcpRelayData(c, payload, len); break;
        default:
            LOG_ERROR("[server] Unknown msg 0x%02X from peer %u len=%zu", msgType, c.peerId, len);
            break;
        }
    } catch (const std::exception& e) {
        LOG_ERROR("[server] Malformed msg 0x%02X from peer %u len=%zu: %s",
                  msgType, c.peerId, len, e.what());
        c.alive = false;
    }
}

void SignalServer::processDataMessage(ClientSession& c, uint8_t msgType,
                                      const uint8_t* payload, size_t len)
{
    c.dataLastPing = time(nullptr);
    LOG_DETAIL("[server] DataMsg type=0x%02X peer=%u len=%zu", msgType, c.peerId, len);

    try {
        switch (msgType) {
        case MSG_TCP_RELAY_DATA:
            onTcpRelayData(c, payload, len);
            break;
        case MSG_PING:
            if (c.dataFd >= 0) {
                ByteBuffer empty;
                sendDataMsg(c, MSG_PONG, empty);
            }
            break;
        default:
            LOG_ERROR("[server] Unexpected msg 0x%02X on data channel from peer %u",
                      msgType, c.peerId);
            break;
        }
    } catch (const std::exception& e) {
        LOG_ERROR("[server] Malformed data msg 0x%02X from peer %u len=%zu: %s",
                  msgType, c.peerId, len, e.what());
        if (c.dataFd >= 0)
            handleDataChannelDisconnect(c.dataFd);
    }
}

void SignalServer::onDataChannelInit(int fd, const uint8_t* p, size_t len) {
    if (len < 4) {
        LOG_ERROR("[server] DATA_CHANNEL_INIT too short (len=%zu), closing fd=%d", len, fd);
        epollDel(fd);
        close(fd);
        return;
    }

    ByteBuffer bb(p, len);
    uint32_t peerId = bb.readU32();

    auto pit = m_peerMap.find(peerId);
    if (pit == m_peerMap.end()) {
        LOG_ERROR("[server] DATA_CHANNEL_INIT unknown peer %u, closing fd=%d", peerId, fd);
        epollDel(fd);
        close(fd);
        return;
    }

    ClientSession& c = *pit->second;

    if (c.dataFd >= 0) {
        LOG_INFO("[server] Replacing old data channel fd=%d for peer %u", c.dataFd, peerId);
        m_dataFdMap.erase(c.dataFd);
        epollDel(c.dataFd);
        close(c.dataFd);
    }

    c.dataFd = fd;
    c.dataLastPing = time(nullptr);
    c.dataSendBuf.clear();
    c.dataRecvBuf.clear();
    m_dataFdMap[fd] = &c;

    ByteBuffer ack;
    sendDataMsg(c, MSG_DATA_CHANNEL_ACK, ack);

    LOG_INFO("[server] Data channel established for peer %u fd=%d", peerId, fd);
}

// ───────── Message handlers ─────────

void SignalServer::onLogin(ClientSession& c, const uint8_t* p, size_t len) {
    ByteBuffer bb(p, len);
    std::string name = bb.readString();

    uint16_t clientVersion = bb.remaining() >= 2 ? bb.readU16() : 1;
    if (clientVersion != PROTOCOL_VERSION) {
        LOG_INFO("[server] Client version %u != server version %u (peer name='%s')",
                 clientVersion, PROTOCOL_VERSION, name.c_str());
    }

    if (!isValidPlayerName(name)) {
        sendError(c.tcpFd, "Invalid player name");
        LOG_INFO("[server] Login rejected: invalid name='%s'", name.c_str());
        return;
    }

    for (auto& kv : m_clients) {
        if (kv.second.peerId != 0 && kv.second.name == name && &kv.second != &c) {
            sendError(c.tcpFd, "Name already in use");
            LOG_INFO("[server] Login rejected: name='%s' already in use (by peer=%u)",
                     name.c_str(), kv.second.peerId);
            return;
        }
    }

    c.name   = name;
    c.peerId = m_nextPeerId++;
    m_peerMap[c.peerId] = &c;

    ByteBuffer resp;
    resp.writeU32(c.peerId);
    resp.writeU16(PROTOCOL_VERSION);
    sendMsg(c.tcpFd, MSG_LOGIN_RESP, resp);
    LOG_INFO("[server] Login: peer=%u name='%s' fd=%d version=%u",
             c.peerId, c.name.c_str(), c.tcpFd, clientVersion);
}

void SignalServer::onCreateRoom(ClientSession& c, const uint8_t* p, size_t len) {
    if (c.peerId == 0) { sendError(c.tcpFd, "Not logged in"); return; }
    if (c.roomId != 0) { sendError(c.tcpFd, "Already in a room"); return; }

    ByteBuffer bb(p, len);
    std::string roomName = bb.readString();
    if (!isValidRoomName(roomName)) {
        sendError(c.tcpFd, "Invalid room name");
        return;
    }

    uint8_t maxPlayers = bb.readU8();
    if (maxPlayers < 2 || maxPlayers > MAX_PLAYERS) {
        sendError(c.tcpFd, "Invalid max players");
        return;
    }

    uint8_t rawMode = bb.remaining() > 0 ? bb.readU8() : MODE_RELAY_KCP;
    if (!isValidTransportModeValue(rawMode)) {
        sendError(c.tcpFd, "Invalid transport mode");
        return;
    }
    TransportMode tmode = normalizeTransportMode(rawMode);

    uint8_t rawFec = bb.remaining() > 0 ? bb.readU8() : FEC_NONE;
    if (!isValidFecModeValue(rawFec)) {
        sendError(c.tcpFd, "Invalid FEC mode");
        return;
    }
    FecMode fec = normalizeFecMode(rawFec, tmode);

    uint8_t encrypted = 0;
    uint8_t pwdHash[32] = {};
    uint8_t salt[16] = {};
    uint8_t sessionSeed[16] = {};
    if (bb.remaining() > 0) {
        encrypted = bb.readU8();
        if (encrypted && bb.remaining() >= 32) {
            bb.readBytes(pwdHash, 32);
            int fd = open("/dev/urandom", O_RDONLY);
            if (fd >= 0) {
                read(fd, salt, 16);
                read(fd, sessionSeed, 16);
                close(fd);
            }
        } else {
            encrypted = 0;
        }
    }

    uint16_t roomMtu = ROOM_MTU_DEFAULT;
    if (bb.remaining() >= 2) {
        roomMtu = normalizeRoomMtu(bb.readU16());
    }

    Room* room = m_rooms.createRoom(roomName, c.peerId, maxPlayers, tmode, fec,
                                    roomMtu, encrypted, pwdHash, salt, sessionSeed);
    c.virtualIP = room->allocateVirtualIP();
    c.roomId    = room->id;

    ByteBuffer resp;
    resp.writeU32(room->id);
    resp.writeU32(c.virtualIP);
    resp.writeU8(static_cast<uint8_t>(room->transportMode));
    resp.writeU8(static_cast<uint8_t>(room->fecMode));
    resp.writeU8(room->encrypted);
    if (room->encrypted) {
        resp.writeBytes(room->salt, 16);
        resp.writeBytes(room->sessionSeed, 16);
    }
    resp.writeU16(room->mtu);
    sendMsg(c.tcpFd, MSG_ROOM_CREATED, resp);
    LOG_INFO("[server] Room created: id=%u name='%s' mode=%u fec=%u mtu=%u encrypted=%u maxPlayers=%u by peer=%u vip=%s",
             room->id, roomName.c_str(), room->transportMode, room->fecMode,
             room->mtu, room->encrypted, maxPlayers, c.peerId, ipToString(c.virtualIP).c_str());
}

void SignalServer::onJoinRoom(ClientSession& c, const uint8_t* p, size_t len) {
    if (c.peerId == 0) { sendError(c.tcpFd, "Not logged in"); return; }
    if (c.roomId != 0) { sendError(c.tcpFd, "Already in a room"); return; }

    ByteBuffer bb(p, len);
    uint32_t roomId = bb.readU32();

    Room* room = m_rooms.getRoom(roomId);
    if (!room)        { sendError(c.tcpFd, "Room not found"); return; }
    if (room->isFull()){ sendError(c.tcpFd, "Room is full");  return; }

    if (room->encrypted) {
        c.pendingJoinRoomId = roomId;
        c.awaitingAuth = true;
        int fd = open("/dev/urandom", O_RDONLY);
        if (fd >= 0) { read(fd, c.authChallenge, 32); close(fd); }

        ByteBuffer challenge;
        challenge.writeBytes(c.authChallenge, 32);
        sendMsg(c.tcpFd, MSG_AUTH_CHALLENGE, challenge);
        return;
    }

    completeJoin(c, room);
}

void SignalServer::onAuthResponse(ClientSession& c, const uint8_t* p, size_t len) {
    if (!c.awaitingAuth || c.pendingJoinRoomId == 0) {
        sendError(c.tcpFd, "Unexpected auth response");
        return;
    }

    Room* room = m_rooms.getRoom(c.pendingJoinRoomId);
    if (!room) {
        c.awaitingAuth = false;
        c.pendingJoinRoomId = 0;
        sendError(c.tcpFd, "Room not found");
        return;
    }

    if (len < 32) {
        c.awaitingAuth = false;
        c.pendingJoinRoomId = 0;
        sendError(c.tcpFd, "\xe5\xaf\x86\xe7\xa0\x81\xe9\x94\x99\xe8\xaf\xaf");
        return;
    }

    uint8_t expected[32];
    VLan::computeChallengeResponse(room->passwordHash, c.authChallenge, expected);

    if (crypto_verify32(expected, p) != 0) {
        crypto_wipe(expected, 32);
        c.awaitingAuth = false;
        c.pendingJoinRoomId = 0;
        sendError(c.tcpFd, "\xe5\xaf\x86\xe7\xa0\x81\xe9\x94\x99\xe8\xaf\xaf");
        return;
    }
    crypto_wipe(expected, 32);

    c.awaitingAuth = false;
    uint32_t roomId = c.pendingJoinRoomId;
    c.pendingJoinRoomId = 0;

    if (room->isFull()) { sendError(c.tcpFd, "Room is full"); return; }
    completeJoin(c, room);
}

void SignalServer::completeJoin(ClientSession& c, Room* room) {
    uint32_t roomId = room->id;
    if (!m_rooms.joinRoom(roomId, c.peerId)) {
        sendError(c.tcpFd, "Cannot join room");
        return;
    }

    c.virtualIP = room->allocateVirtualIP();
    c.roomId    = roomId;

    ByteBuffer resp;
    resp.writeU32(roomId);
    resp.writeU32(c.virtualIP);
    resp.writeU8(static_cast<uint8_t>(room->transportMode));
    resp.writeU8(static_cast<uint8_t>(room->fecMode));
    resp.writeU8(room->encrypted);
    if (room->encrypted) {
        resp.writeBytes(room->salt, 16);
        resp.writeBytes(room->sessionSeed, 16);
    }
    resp.writeU16(room->mtu);
    resp.writeU8(static_cast<uint8_t>(room->members.size()));
    for (uint32_t mid : room->members) {
        auto pit = m_peerMap.find(mid);
        if (pit == m_peerMap.end()) continue;
        ClientSession* member = pit->second;
        resp.writeU32(member->peerId);
        resp.writeU32(member->virtualIP);
        resp.writeString(member->name);
        resp.writeU8(member->natType);
        resp.writeU32(member->udpAddrKnown ? ntohl(member->udpAddr.sin_addr.s_addr) : 0);
        resp.writeU16(member->udpAddrKnown ? ntohs(member->udpAddr.sin_port) : 0);
    }
    sendMsg(c.tcpFd, MSG_JOIN_RESP, resp);

    ByteBuffer notify;
    notify.writeU32(c.peerId);
    notify.writeU32(c.virtualIP);
    notify.writeString(c.name);
    notify.writeU8(c.natType);
    broadcastToRoom(roomId, MSG_PEER_JOINED, notify, c.peerId);

    LOG_INFO("[server] Peer %u joined room %u, vip=%s members=%u/%u mode=%u fec=%u mtu=%u",
             c.peerId, roomId, ipToString(c.virtualIP).c_str(),
             (unsigned)room->members.size(), room->maxPlayers,
             room->transportMode, room->fecMode, room->mtu);

    notifyPunchPeers(c);
}

void SignalServer::onLeaveRoom(ClientSession& c) {
    if (c.roomId == 0) return;
    uint32_t roomId = c.roomId;

    ByteBuffer notify;
    notify.writeU32(c.peerId);
    broadcastToRoom(roomId, MSG_PEER_LEFT, notify, c.peerId);

    m_rooms.leaveRoom(roomId, c.peerId);
    c.roomId    = 0;
    c.virtualIP = 0;
    LOG_INFO("[server] Peer %u left room %u", c.peerId, roomId);
}

void SignalServer::onListRooms(ClientSession& c) {
    auto rooms = m_rooms.listRooms();
    LOG_DETAIL("[server] ListRooms request from peer %u, rooms=%zu", c.peerId, rooms.size());
    ByteBuffer resp;
    resp.writeU16(static_cast<uint16_t>(rooms.size()));
    for (Room* r : rooms) {
        resp.writeU32(r->id);
        resp.writeString(r->name);
        resp.writeU8(static_cast<uint8_t>(r->members.size()));
        resp.writeU8(r->maxPlayers);
        resp.writeU8(static_cast<uint8_t>(r->transportMode));
        resp.writeU8(static_cast<uint8_t>(r->fecMode));
        resp.writeU8(r->encrypted);
        resp.writeU16(r->mtu);
    }
    sendMsg(c.tcpFd, MSG_ROOM_LIST, resp);
}

void SignalServer::onNatReport(ClientSession& c, const uint8_t* p, size_t len) {
    ByteBuffer bb(p, len);
    c.natType = static_cast<NatType>(bb.readU8());
    LOG_INFO("[server] Peer %u NAT type: %s (%u)", c.peerId, natTypeName(c.natType), c.natType);
}

void SignalServer::onPunchResult(ClientSession& c, const uint8_t* p, size_t len) {
    ByteBuffer bb(p, len);
    uint32_t targetPeerId = bb.readU32();
    uint8_t  success      = bb.readU8();
    LOG_INFO("[server] Punch %s: peer %u -> peer %u",
             success ? "OK" : "FAIL", c.peerId, targetPeerId);

    auto it = m_peerMap.find(targetPeerId);
    if (it == m_peerMap.end() || c.roomId == 0 ||
        it->second->roomId == 0 || it->second->roomId != c.roomId) {
        LOG_ERROR("[server] Punch result rejected: peer %u room=%u -> peer %u",
                  c.peerId, c.roomId, targetPeerId);
        return;
    }

    if (!success) {
        // Inform both sides to use relay
        ByteBuffer relayMsg;
        relayMsg.writeU32(targetPeerId);
        sendMsg(c.tcpFd, MSG_RELAY_READY, relayMsg);

        ByteBuffer relayMsg2;
        relayMsg2.writeU32(c.peerId);
        sendMsg(it->second->tcpFd, MSG_RELAY_READY, relayMsg2);
    }
}

void SignalServer::onRequestRelay(ClientSession& c, const uint8_t* p, size_t len) {
    ByteBuffer bb(p, len);
    uint32_t targetPeerId = bb.readU32();

    LOG_DETAIL("[server] Relay request: peer %u -> peer %u", c.peerId, targetPeerId);

    auto it = m_peerMap.find(targetPeerId);
    if (it == m_peerMap.end()) {
        LOG_ERROR("[server] Relay target peer %u not found", targetPeerId);
        return;
    }
    if (c.roomId == 0 || it->second->roomId == 0 || it->second->roomId != c.roomId) {
        LOG_ERROR("[server] Relay request rejected: peer %u room=%u -> peer %u room=%u",
                  c.peerId, c.roomId, targetPeerId, it->second->roomId);
        return;
    }

    ByteBuffer resp;
    resp.writeU32(targetPeerId);
    sendMsg(c.tcpFd, MSG_RELAY_READY, resp);

    ByteBuffer resp2;
    resp2.writeU32(c.peerId);
    sendMsg(it->second->tcpFd, MSG_RELAY_READY, resp2);
}

void SignalServer::onPing(ClientSession& c) {
    sendMsg(c.tcpFd, MSG_PONG);
}

void SignalServer::onTcpRelayData(ClientSession& c, const uint8_t* p, size_t len) {
    if (len < 8) return;
    ByteBuffer bb(p, len);
    bb.readU32();
    uint32_t dstId = bb.readU32();

    LOG_DETAIL("[server] TCP relay: peer %u -> peer %u dataSize=%zu", c.peerId, dstId, len - 8);

    auto it = m_peerMap.find(dstId);
    if (it == m_peerMap.end()) {
        LOG_DETAIL("[server] TCP relay dst peer %u not found, dropping", dstId);
        return;
    }

    ClientSession& dst = *it->second;
    if (c.roomId == 0 || dst.roomId == 0 || c.roomId != dst.roomId) {
        LOG_ERROR("[server] TCP relay rejected: peer %u room=%u -> peer %u room=%u",
                  c.peerId, c.roomId, dstId, dst.roomId);
        return;
    }

    ByteBuffer fwd;
    fwd.writeU32(c.peerId);
    fwd.writeU32(dstId);
    fwd.writeBytes(p + 8, len - 8);

    if (dst.dataFd >= 0) {
        sendDataMsg(dst, MSG_TCP_RELAY_DATA, fwd);
    } else {
        sendMsg(dst.tcpFd, MSG_TCP_RELAY_DATA, fwd);
    }
}

// ───────── Punch coordination ─────────

void SignalServer::notifyPunchPeers(ClientSession& c) {
    Room* room = m_rooms.getRoom(c.roomId);
    if (!room) return;

    std::vector<uint32_t> membersCopy = room->members;
    for (uint32_t mid : membersCopy) {
        if (mid == c.peerId) continue;
        auto it = m_peerMap.find(mid);
        if (it == m_peerMap.end()) continue;
        ClientSession* other = it->second;

        // Tell both sides about each other so they can try punching
        {
            ByteBuffer msg;
            msg.writeU32(other->peerId);
            msg.writeU32(other->virtualIP);
            msg.writeU8(other->natType);
            msg.writeU32(other->udpAddrKnown ? ntohl(other->udpAddr.sin_addr.s_addr) : 0);
            msg.writeU16(other->udpAddrKnown ? ntohs(other->udpAddr.sin_port) : 0);
            sendMsg(c.tcpFd, MSG_PUNCH_NOTIFY, msg);
        }
        {
            ByteBuffer msg;
            msg.writeU32(c.peerId);
            msg.writeU32(c.virtualIP);
            msg.writeU8(c.natType);
            msg.writeU32(c.udpAddrKnown ? ntohl(c.udpAddr.sin_addr.s_addr) : 0);
            msg.writeU16(c.udpAddrKnown ? ntohs(c.udpAddr.sin_port) : 0);
            sendMsg(other->tcpFd, MSG_PUNCH_NOTIFY, msg);
        }
    }
}

// ───────── Send helpers ─────────

void SignalServer::epollMod(int fd, uint32_t events) {
    struct epoll_event ev;
    ev.events  = events;
    ev.data.fd = fd;
    epoll_ctl(m_epfd, EPOLL_CTL_MOD, fd, &ev);
}

void SignalServer::flushSendBuf(ClientSession& c) {
    while (!c.sendBuf.empty()) {
        ssize_t n = send(c.tcpFd, c.sendBuf.data(), c.sendBuf.size(), MSG_NOSIGNAL);
        if (n > 0) {
            c.sendBuf.erase(c.sendBuf.begin(), c.sendBuf.begin() + n);
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            epollMod(c.tcpFd, EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLHUP);
            return;
        } else {
            return;
        }
    }
    epollMod(c.tcpFd, EPOLLIN | EPOLLERR | EPOLLHUP);
}

void SignalServer::handleWritable(int fd) {
    auto cit = m_clients.find(fd);
    if (cit != m_clients.end()) {
        flushSendBuf(cit->second);
        if (cit->second.sendBuf.size() > MAX_TCP_SEND_BUF) {
            LOG_ERROR("[server] Send buffer overflow for peer %u (%zu bytes), marking dead",
                      cit->second.peerId, cit->second.sendBuf.size());
            cit->second.alive = false;
        }
        return;
    }

    auto dit = m_dataFdMap.find(fd);
    if (dit != m_dataFdMap.end()) {
        flushDataSendBuf(*dit->second);
        if (dit->second->dataSendBuf.size() > MAX_TCP_SEND_BUF) {
            LOG_ERROR("[server] Data send buffer overflow for peer %u (%zu bytes), dropping data channel",
                      dit->second->peerId, dit->second->dataSendBuf.size());
            handleDataChannelDisconnect(fd);
        }
    }
}

void SignalServer::sendMsg(int fd, uint8_t msgType, const ByteBuffer& body) {
    auto it = m_clients.find(fd);
    if (it == m_clients.end()) return;
    ClientSession& c = it->second;
    if (!c.alive) return;

    TcpMsgHeader hdr;
    hdr.msgType = msgType;
    hdr.length  = htons(static_cast<uint16_t>(body.size()));

    const uint8_t* hp = reinterpret_cast<const uint8_t*>(&hdr);
    c.sendBuf.insert(c.sendBuf.end(), hp, hp + sizeof(hdr));
    if (body.size() > 0)
        c.sendBuf.insert(c.sendBuf.end(), body.data(), body.data() + body.size());

    flushSendBuf(c);

    if (c.sendBuf.size() > MAX_TCP_SEND_BUF) {
        LOG_ERROR("[server] Send buffer overflow for peer %u (%zu bytes), marking dead",
                  c.peerId, c.sendBuf.size());
        c.alive = false;
    }
}

void SignalServer::sendMsg(int fd, uint8_t msgType) {
    ByteBuffer empty;
    sendMsg(fd, msgType, empty);
}

void SignalServer::sendError(int fd, const std::string& text) {
    ByteBuffer body;
    body.writeString(text);
    sendMsg(fd, MSG_ERROR, body);
}

void SignalServer::sendDataMsg(ClientSession& c, uint8_t msgType, const ByteBuffer& body) {
    if (c.dataFd < 0 || !c.alive) return;

    TcpMsgHeader hdr;
    hdr.msgType = msgType;
    hdr.length  = htons(static_cast<uint16_t>(body.size()));

    const uint8_t* hp = reinterpret_cast<const uint8_t*>(&hdr);
    c.dataSendBuf.insert(c.dataSendBuf.end(), hp, hp + sizeof(hdr));
    if (body.size() > 0)
        c.dataSendBuf.insert(c.dataSendBuf.end(), body.data(), body.data() + body.size());

    flushDataSendBuf(c);
}

void SignalServer::flushDataSendBuf(ClientSession& c) {
    if (c.dataFd < 0) return;
    while (!c.dataSendBuf.empty()) {
        ssize_t n = send(c.dataFd, c.dataSendBuf.data(), c.dataSendBuf.size(), MSG_NOSIGNAL);
        if (n > 0) {
            c.dataSendBuf.erase(c.dataSendBuf.begin(), c.dataSendBuf.begin() + n);
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            epollMod(c.dataFd, EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLHUP);
            return;
        } else {
            return;
        }
    }
    epollMod(c.dataFd, EPOLLIN | EPOLLERR | EPOLLHUP);
}

void SignalServer::broadcastToRoom(uint32_t roomId, uint8_t msgType,
                                   const ByteBuffer& body, uint32_t excludePeerId)
{
    Room* room = m_rooms.getRoom(roomId);
    if (!room) return;
    std::vector<uint32_t> membersCopy = room->members;
    for (uint32_t mid : membersCopy) {
        if (mid == excludePeerId) continue;
        auto it = m_peerMap.find(mid);
        if (it != m_peerMap.end())
            sendMsg(it->second->tcpFd, msgType, body);
    }
}

} // namespace VLan

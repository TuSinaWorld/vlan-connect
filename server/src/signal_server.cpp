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

namespace {

std::string hexPreview(const uint8_t* data, size_t len, size_t maxLen = 8) {
    if (!data || len == 0) return "-";
    static const char* hex = "0123456789ABCDEF";
    size_t n = std::min(len, maxLen);
    std::string out;
    out.reserve(n * 3 + 4);
    for (size_t i = 0; i < n; ++i) {
        if (i) out.push_back(' ');
        uint8_t b = data[i];
        out.push_back(hex[(b >> 4) & 0x0F]);
        out.push_back(hex[b & 0x0F]);
    }
    if (len > maxLen) out += " ...";
    return out;
}

const char* tcpMsgName(uint8_t msgType) {
    switch (msgType) {
    case MSG_LOGIN: return "LOGIN";
    case MSG_CREATE_ROOM: return "CREATE_ROOM";
    case MSG_JOIN_ROOM: return "JOIN_ROOM";
    case MSG_RESUME_ROOM: return "RESUME_ROOM";
    case MSG_LEAVE_ROOM: return "LEAVE_ROOM";
    case MSG_LOGOUT: return "LOGOUT";
    case MSG_LOGOUT_ACK: return "LOGOUT_ACK";
    case MSG_LIST_ROOMS: return "LIST_ROOMS";
    case MSG_ROOM_LIST: return "ROOM_LIST";
    case MSG_ROOM_LIST_PUSH: return "ROOM_LIST_PUSH";
    case MSG_REQUEST_RELAY: return "REQUEST_RELAY";
    case MSG_TCP_RELAY_DATA: return "TCP_RELAY_DATA";
    case MSG_DATA_CHANNEL_INIT: return "DATA_CHANNEL_INIT";
    case MSG_CLIENT_HELLO: return "CLIENT_HELLO";
    case MSG_SERVER_AUTH: return "SERVER_AUTH";
    case MSG_SERVER_AUTH_OK: return "SERVER_AUTH_OK";
    case MSG_ENCRYPTED: return "ENCRYPTED";
    case MSG_AUTH_RESPONSE: return "AUTH_RESPONSE";
    case MSG_PING: return "PING";
    case MSG_PONG: return "PONG";
    case MSG_ERROR: return "ERROR";
    default: return "UNKNOWN";
    }
}

bool tokenIsZero(const uint8_t token[RECONNECT_TOKEN_SIZE]) {
    uint8_t acc = 0;
    for (int i = 0; i < RECONNECT_TOKEN_SIZE; ++i)
        acc |= token[i];
    return acc == 0;
}

bool tokenEquals(const uint8_t a[RECONNECT_TOKEN_SIZE],
                 const uint8_t b[RECONNECT_TOKEN_SIZE]) {
    uint8_t diff = 0;
    for (int i = 0; i < RECONNECT_TOKEN_SIZE; ++i)
        diff |= static_cast<uint8_t>(a[i] ^ b[i]);
    return diff == 0;
}

} // namespace

// ───────── Construction / destruction ─────────

SignalServer::SignalServer()
    : m_epfd(-1), m_tcpListenFd(-1),
      m_udpFd(-1),
      m_running(false), m_nextPeerId(1),
      m_authEnabled(false)
{
    memset(m_serverAuthHash, 0, sizeof(m_serverAuthHash));
}

SignalServer::~SignalServer() { stop(); }

void SignalServer::setAuthPassword(const std::string& password) {
    memset(m_serverAuthHash, 0, sizeof(m_serverAuthHash));
    if (password.empty()) {
        m_authEnabled = false;
        return;
    }
    hashPassword(reinterpret_cast<const uint8_t*>(password.data()),
                 password.size(), m_serverAuthHash);
    m_authEnabled = true;
}

// ───────── Socket helpers ─────────

uint32_t SignalServer::allocatePeerId() {
    if (!m_freePeerIds.empty()) {
        uint32_t peerId = *m_freePeerIds.begin();
        m_freePeerIds.erase(m_freePeerIds.begin());
        return peerId;
    }
    return m_nextPeerId++;
}

void SignalServer::releasePeerId(uint32_t peerId) {
    if (peerId == 0) return;
    if (m_peerMap.find(peerId) != m_peerMap.end()) return;
    m_freePeerIds.insert(peerId);
}

void SignalServer::generateLeaseToken(uint8_t token[RECONNECT_TOKEN_SIZE]) {
    secureRandomBytes(token, RECONNECT_TOKEN_SIZE);
    if (tokenIsZero(token))
        token[0] = 1;
}

bool SignalServer::validateResumeLease(uint32_t roomId, uint32_t peerId,
                                       const uint8_t token[RECONNECT_TOKEN_SIZE],
                                       Room** roomOut, RoomLease** leaseOut) {
    if (tokenIsZero(token)) return false;
    Room* room = m_rooms.getRoom(roomId);
    if (!room) return false;
    RoomLease* lease = room->leaseByPeerId(peerId);
    if (!lease) return false;
    if (!tokenEquals(lease->token, token)) return false;
    time_t now = time(nullptr);
    if (!lease->online && lease->expiresAt > 0 && now > lease->expiresAt)
        return false;
    if (roomOut) *roomOut = room;
    if (leaseOut) *leaseOut = lease;
    return true;
}

void SignalServer::sendJoinResponse(ClientSession& c, Room* room,
                                    const uint8_t token[RECONNECT_TOKEN_SIZE]) {
    ByteBuffer resp;
    resp.writeU32(room->id);
    resp.writeU32(c.virtualIP);
    resp.writeU8(static_cast<uint8_t>(room->tcpPolicy.transportMode));
    resp.writeU8(static_cast<uint8_t>(room->tcpPolicy.fecMode));
    resp.writeU8(static_cast<uint8_t>(room->tcpPolicy.kcpProfile));
    resp.writeU8(static_cast<uint8_t>(room->udpPolicy.transportMode));
    resp.writeU8(static_cast<uint8_t>(room->udpPolicy.fecMode));
    resp.writeU8(static_cast<uint8_t>(room->udpPolicy.kcpProfile));
    resp.writeU8(room->passwordProtected);
    resp.writeU16(room->mtu);
    resp.writeU8(static_cast<uint8_t>(room->leases.size()));
    for (size_t i = 0; i < room->leases.size(); ++i) {
        const RoomLease& lease = room->leases[i];
        resp.writeU32(lease.peerId);
        resp.writeU32(lease.virtualIP);
        resp.writeString(lease.name);
    }
    resp.writeBytes(token, RECONNECT_TOKEN_SIZE);
    sendClientMsg(c, MSG_JOIN_RESP, resp);
}

void SignalServer::markSessionOffline(ClientSession& c) {
    if (c.roomId == 0) return;
    Room* room = m_rooms.getRoom(c.roomId);
    if (room)
        room->markLeaseOffline(c.peerId, time(nullptr));
    c.roomId = 0;
    c.virtualIP = 0;
}

void SignalServer::closeClientForTakeover(ClientSession& c) {
    int fd = c.tcpFd;
    LOG_INFO("[server] Closing old connection for peer takeover peer=%u fd=%d roomId=%u",
             c.peerId, fd, c.roomId);

    if (c.dataFd >= 0) {
        m_dataFdMap.erase(c.dataFd);
        epollDel(c.dataFd);
        close(c.dataFd);
        c.dataFd = -1;
    }
    if (c.roomId != 0)
        markSessionOffline(c);
    m_peerMap.erase(c.peerId);
    if (c.secureSessionId != 0)
        m_secureSessionMap.erase(c.secureSessionId);
    c.sendBuf.clear();
    c.recvBuf.clear();
    c.dataSendBuf.clear();
    c.dataRecvBuf.clear();
    epollDel(fd);
    close(fd);
    m_clients.erase(fd);
}

void SignalServer::cleanupExpiredLeases(time_t now) {
    bool changed = false;
    std::vector<uint32_t> emptyRooms;
    std::vector<Room*> rooms = m_rooms.listRooms();
    for (Room* room : rooms) {
        for (size_t i = 0; i < room->leases.size(); ) {
            RoomLease& lease = room->leases[i];
            if (!lease.online && lease.expiresAt > 0 && now > lease.expiresAt) {
                uint32_t peerId = lease.peerId;
                ByteBuffer notify;
                notify.writeU32(peerId);
                broadcastToRoom(room->id, MSG_PEER_LEFT, notify, peerId);
                room->leases.erase(room->leases.begin() + i);
                if (m_peerMap.find(peerId) == m_peerMap.end())
                    releasePeerId(peerId);
                changed = true;
                continue;
            }
            ++i;
        }
        room->ensureHost();
        if (room->leases.empty())
            emptyRooms.push_back(room->id);
    }
    for (uint32_t roomId : emptyRooms) {
        m_rooms.eraseRoom(roomId);
        changed = true;
    }
    if (changed)
        broadcastRoomListPush();
}

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

    m_udpFd = createUdpSocket(port);
    if (m_udpFd < 0) { LOG_ERROR("Cannot bind UDP %u", port); return false; }

    epollAdd(m_tcpListenFd, EPOLLIN);
    epollAdd(m_udpFd,      EPOLLIN);

    LOG_INFO("[server] Listening on port %u (TCP+UDP) tcpFd=%d udpFd=%d",
             port, m_tcpListenFd, m_udpFd);
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
            } else if (fd == m_udpFd) {
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
    if (m_udpFd >= 0)       { close(m_udpFd);       m_udpFd = -1; }
    for (auto& kv : m_clients) {
        if (kv.second.dataFd >= 0) close(kv.second.dataFd);
        close(kv.first);
    }
    for (auto& kv : m_pending) close(kv.first);
    m_clients.clear();
    m_dataFdMap.clear();
    m_pending.clear();
    m_peerMap.clear();
    m_secureSessionMap.clear();
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
        if (m_authEnabled)
            onSecureDataChannelInit(fd, pc.recvBuf.data() + sizeof(TcpMsgHeader), payloadLen);
        else
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
    c.serverAuthOk = !m_authEnabled;
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
    c.alive = false;

    if (c.dataFd >= 0) {
        m_dataFdMap.erase(c.dataFd);
        epollDel(c.dataFd);
        close(c.dataFd);
        c.dataFd = -1;
    }

    if (c.roomId != 0) {
        markSessionOffline(c);
    } else if (c.resumeAccepted && c.resumeRoomId != 0) {
        Room* room = m_rooms.getRoom(c.resumeRoomId);
        if (room)
            room->markLeaseOffline(c.peerId, time(nullptr));
    } else {
        uint32_t peerId = c.peerId;
        m_peerMap.erase(peerId);
        releasePeerId(peerId);
    }
    m_peerMap.erase(c.peerId);
    if (c.secureSessionId != 0)
        m_secureSessionMap.erase(c.secureSessionId);
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
    case UDP_RELAY_DATA:
    case UDP_RAW_RELAY_DATA:
        if (!m_authEnabled)
            RelayHandler::processUdpRelay(fd, buf, n, from, m_peerMap);
        else
            LOG_DETAIL("[server] Dropping plaintext UDP relay while auth is enabled");
        break;
    case UDP_ENCRYPTED: {
        ClientSession* src = nullptr;
        std::vector<uint8_t> plain;
        if (!decryptUdpPacket(buf, static_cast<size_t>(n), &src, &plain) || !src) {
            LOG_DETAIL("[server] Dropping undecryptable UDP packet");
            break;
        }
        if (!plain.empty() && plain[0] == UDP_KEEPALIVE) {
            src->udpAddr = from;
            src->udpAddrKnown = true;
            break;
        }
        if (plain.size() < sizeof(UdpRelayHeader)) break;
        const UdpRelayHeader* hdr = reinterpret_cast<const UdpRelayHeader*>(plain.data());
        if (hdr->type != UDP_RELAY_DATA && hdr->type != UDP_RAW_RELAY_DATA) break;
        uint32_t srcId = ntohl(hdr->srcPeerId);
        uint32_t dstId = ntohl(hdr->dstPeerId);
        if (srcId != src->peerId || src->roomId == 0) {
            LOG_ERROR("[server] Encrypted UDP src mismatch sessionPeer=%u hdrPeer=%u",
                      src->peerId, srcId);
            break;
        }
        src->udpAddr = from;
        src->udpAddrKnown = true;
        auto dstIt = m_peerMap.find(dstId);
        if (dstIt == m_peerMap.end() || !dstIt->second->udpAddrKnown) break;
        ClientSession& dst = *dstIt->second;
        if (dst.roomId == 0 || dst.roomId != src->roomId) {
            LOG_ERROR("[server] Encrypted UDP relay rejected: peer %u room=%u -> peer %u room=%u",
                      src->peerId, src->roomId, dstId, dst.roomId);
            break;
        }
        sendEncryptedUdp(fd, dst, plain, dst.udpAddr);
        break;
    }
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
        if (m_authEnabled && kv.second.helloDone && !kv.second.serverAuthOk &&
            kv.second.authDeadline > 0 && now > kv.second.authDeadline) {
            LOG_ERROR("[server] Auth timeout fd=%d", kv.first);
            kv.second.alive = false;
        }
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

    cleanupExpiredLeases(now);
}

// ───────── Message dispatch ─────────

void SignalServer::processMessage(ClientSession& c, uint8_t msgType,
                                  const uint8_t* payload, size_t len)
{
    c.lastPing = time(nullptr);
    LOG_DETAIL("[server] Signal msg fd=%d peer=%u type=0x%02X(%s) len=%zu hello=%u authOk=%u secure=%u preview=%s",
               c.tcpFd, c.peerId, msgType, tcpMsgName(msgType), len,
               c.helloDone ? 1 : 0, c.serverAuthOk ? 1 : 0, c.secureEnabled ? 1 : 0,
               hexPreview(payload, len).c_str());

    try {
        std::vector<uint8_t> decryptedSignalPayload;
        if (msgType == MSG_CLIENT_HELLO) {
            onClientHello(c, payload, len);
            return;
        }
        if (!c.helloDone) {
            LOG_ERROR("[server] Non-hello message before protocol negotiation from fd=%d", c.tcpFd);
            c.alive = false;
            return;
        }
        if (m_authEnabled) {
            if (!c.serverAuthOk) {
                if (msgType == MSG_SERVER_AUTH) {
                    onServerAuth(c, payload, len);
                } else {
                    LOG_ERROR("[server] Non-auth message before auth from fd=%d", c.tcpFd);
                    c.alive = false;
                }
                return;
            }
            if (msgType != MSG_ENCRYPTED) {
                LOG_ERROR("[server] Plaintext business msg 0x%02X after auth from peer %u",
                          msgType, c.peerId);
                c.alive = false;
                return;
            }
            uint8_t innerType = 0;
            if (!decryptClientFrame(c, payload, len, &innerType, &decryptedSignalPayload)) {
                LOG_ERROR("[server] Cannot decrypt client frame from peer %u fd=%d encLen=%zu preview=%s",
                          c.peerId, c.tcpFd, len, hexPreview(payload, len).c_str());
                c.alive = false;
                return;
            }
            LOG_DETAIL("[server] Decrypted signal frame peer=%u inner=0x%02X(%s) bodyLen=%zu preview=%s",
                       c.peerId, innerType, tcpMsgName(innerType), decryptedSignalPayload.size(),
                       hexPreview(decryptedSignalPayload.empty() ? nullptr : decryptedSignalPayload.data(),
                                  decryptedSignalPayload.size()).c_str());
            msgType = innerType;
            payload = decryptedSignalPayload.empty() ? nullptr : decryptedSignalPayload.data();
            len = decryptedSignalPayload.size();
        }

        switch (msgType) {
        case MSG_LOGIN:         onLogin(c, payload, len);        break;
        case MSG_CREATE_ROOM:   onCreateRoom(c, payload, len);   break;
        case MSG_JOIN_ROOM:     onJoinRoom(c, payload, len);     break;
        case MSG_RESUME_ROOM:   onResumeRoom(c, payload, len);   break;
        case MSG_LEAVE_ROOM:    onLeaveRoom(c);                  break;
        case MSG_LOGOUT:        onLogout(c);                     break;
        case MSG_LIST_ROOMS:    onListRooms(c);                  break;
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
        std::vector<uint8_t> decryptedDataPayload;
        if (m_authEnabled) {
            if (!c.serverAuthOk || msgType != MSG_ENCRYPTED) {
                LOG_ERROR("[server] Plain/unauth data msg 0x%02X from peer %u",
                          msgType, c.peerId);
                if (c.dataFd >= 0)
                    handleDataChannelDisconnect(c.dataFd);
                return;
            }
            if (!c.dataCipher.decrypt(payload, len, &decryptedDataPayload) ||
                decryptedDataPayload.empty()) {
                LOG_ERROR("[server] Cannot decrypt data msg from peer %u", c.peerId);
                if (c.dataFd >= 0)
                    handleDataChannelDisconnect(c.dataFd);
                return;
            }
            msgType = decryptedDataPayload[0];
            payload = decryptedDataPayload.size() > 1 ? decryptedDataPayload.data() + 1 : nullptr;
            len = decryptedDataPayload.size() > 1 ? decryptedDataPayload.size() - 1 : 0;
        }

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

void SignalServer::onSecureDataChannelInit(int fd, const uint8_t* p, size_t len) {
    if (len < SECURE_SESSION_ID_SIZE + SECURE_FRAME_OVERHEAD) {
        LOG_ERROR("[server] Secure DATA_CHANNEL_INIT too short (len=%zu), closing fd=%d", len, fd);
        epollDel(fd);
        close(fd);
        return;
    }

    uint32_t sessionId = readU32BE(p);
    auto sit = m_secureSessionMap.find(sessionId);
    if (sit == m_secureSessionMap.end() || !sit->second || !sit->second->serverAuthOk) {
        LOG_ERROR("[server] Secure DATA_CHANNEL_INIT unknown session %u, closing fd=%d", sessionId, fd);
        epollDel(fd);
        close(fd);
        return;
    }

    ClientSession& c = *sit->second;
    std::vector<uint8_t> plain;
    if (!c.dataCipher.decrypt(p + SECURE_SESSION_ID_SIZE,
                              len - SECURE_SESSION_ID_SIZE,
                              &plain)) {
        LOG_ERROR("[server] Secure DATA_CHANNEL_INIT decrypt failed for session %u", sessionId);
        epollDel(fd);
        close(fd);
        return;
    }
    if (plain.size() < 4) {
        epollDel(fd);
        close(fd);
        return;
    }
    ByteBuffer bb(plain.data(), plain.size());
    uint32_t peerId = bb.readU32();
    if (peerId != c.peerId) {
        LOG_ERROR("[server] Secure DATA_CHANNEL_INIT peer mismatch %u != %u", peerId, c.peerId);
        epollDel(fd);
        close(fd);
        return;
    }

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
    LOG_INFO("[server] Secure data channel established for peer %u fd=%d", peerId, fd);
}

// ───────── Message handlers ─────────

void SignalServer::onClientHello(ClientSession& c, const uint8_t* p, size_t len) {
    if (len < 2 + 16 + 32) {
        LOG_ERROR("[server] CLIENT_HELLO too short fd=%d len=%zu preview=%s",
                  c.tcpFd, len, hexPreview(p, len).c_str());
        c.alive = false;
        return;
    }
    ByteBuffer bb(p, len);
    uint16_t clientVersion = bb.readU16();
    bb.readBytes(c.clientNonce, 16);
    bb.readBytes(c.clientPubKey, 32);
    c.helloDone = true;
    c.serverAuthOk = !m_authEnabled;
    LOG_INFO("[server] Client hello fd=%d version=%u authRequired=%u len=%zu",
             c.tcpFd, clientVersion, m_authEnabled ? 1 : 0, len);

    ByteBuffer resp;
    resp.writeU16(PROTOCOL_VERSION);
    resp.writeU8(m_authEnabled ? 1 : 0);

    if (clientVersion != PROTOCOL_VERSION) {
        LOG_INFO("[server] Client hello version %u != server version %u",
                 clientVersion, PROTOCOL_VERSION);
        sendError(c.tcpFd, "Protocol version mismatch");
        c.alive = false;
        return;
    }

    if (m_authEnabled) {
        secureRandomBytes(c.serverNonce, 16);
        secureRandomBytes(c.serverPrivKey, 32);
        crypto_x25519_public_key(c.serverPubKey, c.serverPrivKey);
        c.authDeadline = time(nullptr) + SERVER_AUTH_TIMEOUT_SEC;
        LOG_DETAIL("[server] Auth challenge prepared fd=%d deadline=%ld",
                   c.tcpFd, static_cast<long>(c.authDeadline));
        resp.writeBytes(c.serverNonce, 16);
        resp.writeBytes(c.serverPubKey, 32);
    }

    sendMsg(c.tcpFd, MSG_SERVER_HELLO, resp);
}

void SignalServer::onServerAuth(ClientSession& c, const uint8_t* p, size_t len) {
    if (!m_authEnabled || !c.helloDone || c.serverAuthOk) {
        LOG_ERROR("[server] Unexpected SERVER_AUTH fd=%d hello=%u authOk=%u len=%zu",
                  c.tcpFd, c.helloDone ? 1 : 0, c.serverAuthOk ? 1 : 0, len);
        c.alive = false;
        return;
    }
    if (time(nullptr) > c.authDeadline || len < 32) {
        LOG_ERROR("[server] SERVER_AUTH rejected fd=%d len=%zu deadline=%ld now=%ld",
                  c.tcpFd, len, static_cast<long>(c.authDeadline),
                  static_cast<long>(time(nullptr)));
        sendError(c.tcpFd, "Server authentication timeout");
        c.alive = false;
        return;
    }

    uint8_t shared[32];
    uint8_t master[32];
    uint8_t expected[32];
    crypto_x25519(shared, c.serverPrivKey, c.clientPubKey);
    deriveSecureMaster(master, shared, m_serverAuthHash,
                       c.clientNonce, c.serverNonce,
                       c.clientPubKey, c.serverPubKey);
    computeClientAuthProof(expected, master, m_serverAuthHash);
    if (crypto_verify32(expected, p) != 0) {
        LOG_ERROR("[server] SERVER_AUTH invalid password fd=%d len=%zu", c.tcpFd, len);
        crypto_wipe(shared, sizeof(shared));
        crypto_wipe(master, sizeof(master));
        crypto_wipe(expected, sizeof(expected));
        sendError(c.tcpFd, "Invalid server password");
        c.alive = false;
        return;
    }

    memcpy(c.secureMaster, master, 32);
    c.secureSessionId = deriveSessionId(master);
    c.secureCipher.init(master, false, "signal");
    c.dataCipher.init(master, false, "data");
    c.udpCipher.init(master, false, "udp");
    c.secureEnabled = true;
    c.serverAuthOk = true;
    m_secureSessionMap[c.secureSessionId] = &c;

    uint8_t serverProof[32];
    computeServerAuthProof(serverProof, master, m_serverAuthHash);
    ByteBuffer ok;
    ok.writeU32(c.secureSessionId);
    ok.writeBytes(serverProof, 32);
    sendMsg(c.tcpFd, MSG_SERVER_AUTH_OK, ok);
    LOG_INFO("[server] Server auth OK session=%u fd=%d authPayloadLen=%zu",
             c.secureSessionId, c.tcpFd, len);

    crypto_wipe(shared, sizeof(shared));
    crypto_wipe(master, sizeof(master));
    crypto_wipe(expected, sizeof(expected));
    crypto_wipe(serverProof, sizeof(serverProof));
}

void SignalServer::onLogin(ClientSession& c, const uint8_t* p, size_t len) {
    std::string name;
    uint16_t clientVersion = 1;
    bool parsedLogin = false;
    bool hasResume = false;
    uint32_t resumeRoomId = 0;
    uint32_t resumePeerId = 0;
    uint8_t resumeToken[RECONNECT_TOKEN_SIZE] = {};

    try {
        ByteBuffer bb(p, len);
        name = bb.readString();
        clientVersion = bb.remaining() >= 2 ? bb.readU16() : 1;
        if (bb.remaining() >= 1) {
            hasResume = bb.readU8() != 0;
            if (hasResume) {
                resumeRoomId = bb.readU32();
                resumePeerId = bb.readU32();
                bb.readBytes(resumeToken, RECONNECT_TOKEN_SIZE);
            }
        }
        parsedLogin = true;
        LOG_INFO("[server] Parsed login payload peer=%u fd=%d mode=length-prefixed name='%s' version=%u resume=%u bodyLen=%zu",
                 c.peerId, c.tcpFd, name.c_str(), clientVersion, hasResume ? 1 : 0, len);
    } catch (const std::exception& e) {
        LOG_ERROR("[server] Login length-prefixed parse failed fd=%d peer=%u len=%zu reason=%s preview=%s",
                  c.tcpFd, c.peerId, len, e.what(), hexPreview(p, len).c_str());
    }

    if (!parsedLogin) {
        LOG_ERROR("[server] Malformed login fd=%d peer=%u len=%zu preview=%s",
                  c.tcpFd, c.peerId, len, hexPreview(p, len).c_str());
        sendError(c.tcpFd, "Malformed login");
        c.alive = false;
        return;
    }

    if (clientVersion != PROTOCOL_VERSION) {
        LOG_INFO("[server] Client version %u != server version %u (peer name='%s')",
                 clientVersion, PROTOCOL_VERSION, name.c_str());
        sendError(c.tcpFd, "Protocol version mismatch");
        c.alive = false;
        return;
    }

    if (!isValidPlayerName(name)) {
        sendError(c.tcpFd, "Invalid player name");
        LOG_INFO("[server] Login rejected: invalid name='%s'", name.c_str());
        return;
    }

    Room* resumeRoom = nullptr;
    RoomLease* resumeLease = nullptr;
    if (hasResume &&
        validateResumeLease(resumeRoomId, resumePeerId, resumeToken,
                            &resumeRoom, &resumeLease)) {
        auto oldIt = m_peerMap.find(resumePeerId);
        if (oldIt != m_peerMap.end() && oldIt->second != &c)
            closeClientForTakeover(*oldIt->second);

        c.name = resumeLease->name.empty() ? name : resumeLease->name;
        c.peerId = resumePeerId;
        c.resumeAccepted = true;
        c.resumeRoomId = resumeRoomId;
        c.resumePeerId = resumePeerId;
        memcpy(c.resumeToken, resumeToken, RECONNECT_TOKEN_SIZE);
        resumeRoom->markLeaseOffline(resumePeerId, time(nullptr));
        m_peerMap[c.peerId] = &c;

        ByteBuffer resp;
        resp.writeU32(c.peerId);
        resp.writeU16(PROTOCOL_VERSION);
        resp.writeU8(1);
        sendClientMsg(c, MSG_LOGIN_RESP, resp);
        LOG_INFO("[server] Resume login accepted: peer=%u room=%u name='%s' fd=%d",
                 c.peerId, resumeRoomId, c.name.c_str(), c.tcpFd);
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
    c.peerId = allocatePeerId();
    c.resumeAccepted = false;
    c.resumeRoomId = 0;
    c.resumePeerId = 0;
    memset(c.resumeToken, 0, sizeof(c.resumeToken));
    m_peerMap[c.peerId] = &c;

    ByteBuffer resp;
    resp.writeU32(c.peerId);
    resp.writeU16(PROTOCOL_VERSION);
    resp.writeU8(0);
    sendClientMsg(c, MSG_LOGIN_RESP, resp);
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

    if (bb.remaining() < 6) {
        sendError(c.tcpFd, "Missing traffic policies");
        return;
    }
    uint8_t tcpMode = bb.readU8();
    uint8_t tcpFec = bb.readU8();
    uint8_t tcpProfile = bb.readU8();
    uint8_t udpMode = bb.readU8();
    uint8_t udpFec = bb.readU8();
    uint8_t udpProfile = bb.readU8();
    if (!isValidTransportModeValue(tcpMode) || !isValidTransportModeValue(udpMode) ||
        !isValidFecModeValue(tcpFec) || !isValidFecModeValue(udpFec) ||
        !isValidKcpProfileValue(tcpProfile) || !isValidKcpProfileValue(udpProfile)) {
        sendError(c.tcpFd, "Invalid traffic policy");
        return;
    }
    RoomTrafficPolicy tcpPolicy = normalizeTrafficPolicy(
        tcpMode, tcpFec, tcpProfile, makeDefaultTcpPolicy());
    RoomTrafficPolicy udpPolicy = normalizeTrafficPolicy(
        udpMode, udpFec, udpProfile, makeDefaultUdpPolicy());

    uint8_t passwordProtected = 0;
    uint8_t pwdHash[32] = {};
    if (bb.remaining() > 0) {
        passwordProtected = bb.readU8();
        if (passwordProtected && bb.remaining() >= 32) {
            bb.readBytes(pwdHash, 32);
        } else {
            passwordProtected = 0;
        }
    }

    uint16_t roomMtu = ROOM_MTU_DEFAULT;
    if (bb.remaining() >= 2) {
        roomMtu = normalizeRoomMtu(bb.readU16());
    }

    Room* room = m_rooms.createRoom(roomName, c.peerId, maxPlayers,
                                    tcpPolicy, udpPolicy,
                                    roomMtu, passwordProtected, pwdHash);
    uint8_t leaseToken[RECONNECT_TOKEN_SIZE];
    generateLeaseToken(leaseToken);
    RoomLease* lease = room->addLease(c.peerId, c.name, leaseToken);
    if (!lease) {
        m_rooms.eraseRoom(room->id);
        sendError(c.tcpFd, "Cannot allocate virtual IP");
        return;
    }
    c.virtualIP = lease->virtualIP;
    c.roomId    = room->id;
    c.resumeAccepted = false;
    c.resumeRoomId = room->id;
    c.resumePeerId = c.peerId;
    memcpy(c.resumeToken, leaseToken, RECONNECT_TOKEN_SIZE);

    ByteBuffer resp;
    resp.writeU32(room->id);
    resp.writeU32(c.virtualIP);
    resp.writeU8(static_cast<uint8_t>(room->tcpPolicy.transportMode));
    resp.writeU8(static_cast<uint8_t>(room->tcpPolicy.fecMode));
    resp.writeU8(static_cast<uint8_t>(room->tcpPolicy.kcpProfile));
    resp.writeU8(static_cast<uint8_t>(room->udpPolicy.transportMode));
    resp.writeU8(static_cast<uint8_t>(room->udpPolicy.fecMode));
    resp.writeU8(static_cast<uint8_t>(room->udpPolicy.kcpProfile));
    resp.writeU8(room->passwordProtected);
    resp.writeU16(room->mtu);
    resp.writeBytes(leaseToken, RECONNECT_TOKEN_SIZE);
    sendClientMsg(c, MSG_ROOM_CREATED, resp);
    LOG_INFO("[server] Room created: id=%u name='%s' tcp=(%u,%u,%u) udp=(%u,%u,%u) mtu=%u password=%u maxPlayers=%u by peer=%u vip=%s",
             room->id, roomName.c_str(),
             room->tcpPolicy.transportMode, room->tcpPolicy.fecMode, room->tcpPolicy.kcpProfile,
             room->udpPolicy.transportMode, room->udpPolicy.fecMode, room->udpPolicy.kcpProfile,
             room->mtu, room->passwordProtected, maxPlayers, c.peerId, ipToString(c.virtualIP).c_str());
    broadcastRoomListPush();
}

void SignalServer::onJoinRoom(ClientSession& c, const uint8_t* p, size_t len) {
    if (c.peerId == 0) { sendError(c.tcpFd, "Not logged in"); return; }
    if (c.roomId != 0) { sendError(c.tcpFd, "Already in a room"); return; }

    ByteBuffer bb(p, len);
    uint32_t roomId = bb.readU32();

    Room* room = m_rooms.getRoom(roomId);
    if (!room)        { sendError(c.tcpFd, "Room not found"); return; }
    if (room->isFull()){ sendError(c.tcpFd, "Room is full");  return; }

    if (room->passwordProtected) {
        c.pendingJoinRoomId = roomId;
        c.awaitingAuth = true;
        int fd = open("/dev/urandom", O_RDONLY);
        if (fd >= 0) { read(fd, c.authChallenge, 32); close(fd); }

        ByteBuffer challenge;
        challenge.writeBytes(c.authChallenge, 32);
        sendClientMsg(c, MSG_AUTH_CHALLENGE, challenge);
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
    uint8_t leaseToken[RECONNECT_TOKEN_SIZE];
    generateLeaseToken(leaseToken);
    RoomLease* lease = room->addLease(c.peerId, c.name, leaseToken);
    if (!lease) {
        sendError(c.tcpFd, "Cannot join room");
        return;
    }

    c.virtualIP = lease->virtualIP;
    c.roomId    = roomId;
    c.resumeAccepted = false;
    c.resumeRoomId = roomId;
    c.resumePeerId = c.peerId;
    memcpy(c.resumeToken, leaseToken, RECONNECT_TOKEN_SIZE);

    sendJoinResponse(c, room, leaseToken);

    ByteBuffer notify;
    notify.writeU32(c.peerId);
    notify.writeU32(c.virtualIP);
    notify.writeString(c.name);
    broadcastToRoom(roomId, MSG_PEER_JOINED, notify, c.peerId);

    LOG_INFO("[server] Peer %u joined room %u, vip=%s members=%u/%u tcp=(%u,%u,%u) udp=(%u,%u,%u) mtu=%u",
             c.peerId, roomId, ipToString(c.virtualIP).c_str(),
             (unsigned)room->leases.size(), room->maxPlayers,
             room->tcpPolicy.transportMode, room->tcpPolicy.fecMode, room->tcpPolicy.kcpProfile,
             room->udpPolicy.transportMode, room->udpPolicy.fecMode, room->udpPolicy.kcpProfile,
             room->mtu);

    broadcastRoomListPush();
    notifyRelayPeers(c);
}

void SignalServer::completeResume(ClientSession& c, Room* room, RoomLease* lease) {
    room->markLeaseOnline(lease->peerId);
    c.roomId = room->id;
    c.virtualIP = lease->virtualIP;
    c.name = lease->name;
    c.resumeAccepted = false;
    c.resumeRoomId = room->id;
    c.resumePeerId = c.peerId;
    memcpy(c.resumeToken, lease->token, RECONNECT_TOKEN_SIZE);
    m_peerMap[c.peerId] = &c;

    sendJoinResponse(c, room, lease->token);
    LOG_INFO("[server] Peer %u resumed room %u, vip=%s members=%u/%u",
             c.peerId, room->id, ipToString(c.virtualIP).c_str(),
             (unsigned)room->leases.size(), room->maxPlayers);

    notifyRelayPeers(c);
}

void SignalServer::onResumeRoom(ClientSession& c, const uint8_t* p, size_t len) {
    if (c.peerId == 0) { sendError(c.tcpFd, "Not logged in"); return; }

    ByteBuffer bb(p, len);
    uint32_t roomId = bb.readU32();
    uint32_t peerId = bb.readU32();
    uint8_t token[RECONNECT_TOKEN_SIZE];
    bb.readBytes(token, RECONNECT_TOKEN_SIZE);

    if (!c.resumeAccepted || c.peerId != peerId ||
        c.resumeRoomId != roomId || c.resumePeerId != peerId ||
        !tokenEquals(c.resumeToken, token)) {
        sendError(c.tcpFd, "Resume rejected");
        return;
    }

    Room* room = nullptr;
    RoomLease* lease = nullptr;
    if (!validateResumeLease(roomId, peerId, token, &room, &lease)) {
        Room* staleRoom = m_rooms.getRoom(roomId);
        RoomLease* staleLease = staleRoom ? staleRoom->leaseByPeerId(peerId) : nullptr;
        if (staleRoom && staleLease &&
            !staleLease->online && staleLease->expiresAt > 0 &&
            time(nullptr) > staleLease->expiresAt &&
            tokenEquals(staleLease->token, token)) {
            ByteBuffer notify;
            notify.writeU32(peerId);
            broadcastToRoom(roomId, MSG_PEER_LEFT, notify, peerId);
            staleRoom->removeLease(peerId);
            if (staleRoom->leases.empty())
                m_rooms.eraseRoom(roomId);
            broadcastRoomListPush();
        }
        c.resumeAccepted = false;
        c.resumeRoomId = 0;
        c.resumePeerId = 0;
        memset(c.resumeToken, 0, sizeof(c.resumeToken));
        sendError(c.tcpFd, "Resume expired");
        return;
    }

    completeResume(c, room, lease);
}

void SignalServer::onLeaveRoom(ClientSession& c) {
    if (c.roomId == 0) return;
    uint32_t roomId = c.roomId;

    ByteBuffer notify;
    notify.writeU32(c.peerId);
    broadcastToRoom(roomId, MSG_PEER_LEFT, notify, c.peerId);

    Room* room = m_rooms.getRoom(roomId);
    if (room) {
        room->removeLease(c.peerId);
        if (room->leases.empty())
            m_rooms.eraseRoom(roomId);
    }
    c.roomId    = 0;
    c.virtualIP = 0;
    c.resumeAccepted = false;
    c.resumeRoomId = 0;
    c.resumePeerId = 0;
    memset(c.resumeToken, 0, sizeof(c.resumeToken));
    LOG_INFO("[server] Peer %u left room %u", c.peerId, roomId);
    broadcastRoomListPush();
}

void SignalServer::onLogout(ClientSession& c) {
    uint32_t peerId = c.peerId;
    bool roomChanged = false;

    if (c.dataFd >= 0) {
        m_dataFdMap.erase(c.dataFd);
        epollDel(c.dataFd);
        close(c.dataFd);
        c.dataFd = -1;
        c.dataSendBuf.clear();
        c.dataRecvBuf.clear();
    }

    if (c.roomId != 0 && peerId != 0) {
        uint32_t roomId = c.roomId;
        ByteBuffer notify;
        notify.writeU32(peerId);
        broadcastToRoom(roomId, MSG_PEER_LEFT, notify, peerId);

        Room* room = m_rooms.getRoom(roomId);
        if (room) {
            room->removeLease(peerId);
            if (room->leases.empty())
                m_rooms.eraseRoom(roomId);
        }
        roomChanged = true;
        LOG_INFO("[server] Peer %u logout left room %u", peerId, roomId);
    } else if (c.resumeAccepted && c.resumeRoomId != 0 && c.resumePeerId != 0) {
        Room* room = m_rooms.getRoom(c.resumeRoomId);
        RoomLease* lease = room ? room->leaseByPeerId(c.resumePeerId) : nullptr;
        if (room && lease && tokenEquals(lease->token, c.resumeToken)) {
            ByteBuffer notify;
            notify.writeU32(c.resumePeerId);
            broadcastToRoom(c.resumeRoomId, MSG_PEER_LEFT, notify, c.resumePeerId);
            room->removeLease(c.resumePeerId);
            if (room->leases.empty())
                m_rooms.eraseRoom(c.resumeRoomId);
            roomChanged = true;
            LOG_INFO("[server] Peer %u logout released pending resume room %u",
                     c.resumePeerId, c.resumeRoomId);
        }
    }

    ByteBuffer ack;
    sendClientMsg(c, MSG_LOGOUT_ACK, ack);

    if (peerId != 0) {
        m_peerMap.erase(peerId);
        releasePeerId(peerId);
    }
    if (c.secureSessionId != 0)
        m_secureSessionMap.erase(c.secureSessionId);

    c.peerId = 0;
    c.roomId = 0;
    c.virtualIP = 0;
    c.resumeAccepted = false;
    c.resumeRoomId = 0;
    c.resumePeerId = 0;
    memset(c.resumeToken, 0, sizeof(c.resumeToken));
    c.udpAddrKnown = false;

    LOG_INFO("[server] Logout complete for peer %u fd=%d", peerId, c.tcpFd);
    if (roomChanged)
        broadcastRoomListPush();
}

ByteBuffer SignalServer::buildRoomListPayload() {
    auto rooms = m_rooms.listRooms();
    ByteBuffer resp;
    resp.writeU16(static_cast<uint16_t>(rooms.size()));
    for (Room* r : rooms) {
        resp.writeU32(r->id);
        resp.writeString(r->name);
        resp.writeU8(static_cast<uint8_t>(r->leases.size()));
        resp.writeU8(r->maxPlayers);
        resp.writeU8(static_cast<uint8_t>(r->tcpPolicy.transportMode));
        resp.writeU8(static_cast<uint8_t>(r->tcpPolicy.fecMode));
        resp.writeU8(static_cast<uint8_t>(r->tcpPolicy.kcpProfile));
        resp.writeU8(static_cast<uint8_t>(r->udpPolicy.transportMode));
        resp.writeU8(static_cast<uint8_t>(r->udpPolicy.fecMode));
        resp.writeU8(static_cast<uint8_t>(r->udpPolicy.kcpProfile));
        resp.writeU8(r->passwordProtected);
        resp.writeU16(r->mtu);
    }
    return resp;
}

void SignalServer::broadcastRoomListPush() {
    ByteBuffer resp = buildRoomListPayload();
    size_t sent = 0;
    for (auto& kv : m_clients) {
        ClientSession& client = kv.second;
        if (client.peerId == 0 || !client.alive || !client.helloDone || !client.serverAuthOk)
            continue;
        sendClientMsg(client, MSG_ROOM_LIST_PUSH, resp);
        ++sent;
    }
    LOG_DETAIL("[server] Room list pushed to %zu clients", sent);
}

void SignalServer::onListRooms(ClientSession& c) {
    auto rooms = m_rooms.listRooms();
    LOG_DETAIL("[server] ListRooms request from peer %u, rooms=%zu", c.peerId, rooms.size());
    ByteBuffer resp = buildRoomListPayload();
    sendClientMsg(c, MSG_ROOM_LIST, resp);
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
    sendClientMsg(c, MSG_RELAY_READY, resp);

    ByteBuffer resp2;
    resp2.writeU32(c.peerId);
    sendClientMsg(*it->second, MSG_RELAY_READY, resp2);
}

void SignalServer::onPing(ClientSession& c) {
    sendClientMsg(c, MSG_PONG);
}

void SignalServer::onTcpRelayData(ClientSession& c, const uint8_t* p, size_t len) {
    if (len < 9) return;
    ByteBuffer bb(p, len);
    bb.readU32();
    uint32_t dstId = bb.readU32();
    uint8_t trafficClass = bb.readU8();

    LOG_DETAIL("[server] TCP relay: peer %u -> peer %u class=%u dataSize=%zu",
               c.peerId, dstId, trafficClass, len - 9);

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
    fwd.writeU8(trafficClass == TRAFFIC_TCP ? TRAFFIC_TCP : TRAFFIC_UDP);
    fwd.writeBytes(p + 9, len - 9);

    if (dst.dataFd >= 0) {
        sendDataMsg(dst, MSG_TCP_RELAY_DATA, fwd);
    } else {
        sendClientMsg(dst, MSG_TCP_RELAY_DATA, fwd);
    }
}

// ───────── Relay coordination ─────────

void SignalServer::notifyRelayPeers(ClientSession& c) {
    Room* room = m_rooms.getRoom(c.roomId);
    if (!room) return;

    std::vector<uint32_t> membersCopy = room->peerIds();
    for (uint32_t mid : membersCopy) {
        if (mid == c.peerId) continue;
        auto it = m_peerMap.find(mid);
        if (it == m_peerMap.end()) continue;
        ClientSession* other = it->second;

        sendRelayReady(c, other->peerId);
        sendRelayReady(*other, c.peerId);
    }
}

void SignalServer::sendRelayReady(ClientSession& c, uint32_t peerId) {
    ByteBuffer msg;
    msg.writeU32(peerId);
    sendClientMsg(c, MSG_RELAY_READY, msg);
}

bool SignalServer::decryptClientFrame(ClientSession& c, const uint8_t* payload, size_t len,
                                      uint8_t* innerType, std::vector<uint8_t>* innerPayload)
{
    std::vector<uint8_t> plain;
    if (!c.secureCipher.decrypt(payload, len, &plain) || plain.empty())
        return false;
    *innerType = plain[0];
    innerPayload->assign(plain.begin() + 1, plain.end());
    return true;
}

bool SignalServer::decryptUdpPacket(const uint8_t* data, size_t len, ClientSession** src,
                                    std::vector<uint8_t>* plain)
{
    if (len < 1 + SECURE_SESSION_ID_SIZE + SECURE_FRAME_OVERHEAD) return false;
    if (data[0] != UDP_ENCRYPTED) return false;
    uint32_t sessionId = readU32BE(data + 1);
    auto it = m_secureSessionMap.find(sessionId);
    if (it == m_secureSessionMap.end() || !it->second || !it->second->serverAuthOk)
        return false;
    ClientSession* c = it->second;
    if (!c->udpCipher.decrypt(data + 1 + SECURE_SESSION_ID_SIZE,
                              len - 1 - SECURE_SESSION_ID_SIZE,
                              plain))
        return false;
    *src = c;
    return true;
}

void SignalServer::sendEncryptedUdp(int udpFd, ClientSession& dst,
                                    const std::vector<uint8_t>& plain,
                                    const struct sockaddr_in& dstAddr)
{
    if (!dst.serverAuthOk || dst.secureSessionId == 0 || !dst.udpAddrKnown)
        return;
    std::vector<uint8_t> enc = dst.udpCipher.encrypt(plain.data(), plain.size());
    std::vector<uint8_t> pkt;
    pkt.resize(1 + SECURE_SESSION_ID_SIZE + enc.size());
    pkt[0] = UDP_ENCRYPTED;
    writeU32BE(pkt.data() + 1, dst.secureSessionId);
    if (!enc.empty())
        memcpy(pkt.data() + 1 + SECURE_SESSION_ID_SIZE, enc.data(), enc.size());
    ssize_t n = sendto(udpFd, reinterpret_cast<const char*>(pkt.data()), pkt.size(), 0,
                       reinterpret_cast<const struct sockaddr*>(&dstAddr), sizeof(dstAddr));
    if (n < 0)
        LOG_ERROR("[server] encrypted UDP sendto peer %u failed: %s", dst.peerId, strerror(errno));
}

ByteBuffer SignalServer::encryptForClient(ClientSession& c, uint8_t msgType,
                                          const ByteBuffer& body)
{
    std::vector<uint8_t> plain;
    plain.reserve(1 + body.size());
    plain.push_back(msgType);
    if (body.size() > 0)
        plain.insert(plain.end(), body.data(), body.data() + body.size());
    std::vector<uint8_t> enc = c.secureCipher.encrypt(plain.data(), plain.size());
    ByteBuffer wrapped;
    if (!enc.empty())
        wrapped.writeBytes(enc.data(), enc.size());
    return wrapped;
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

void SignalServer::sendClientMsg(ClientSession& c, uint8_t msgType, const ByteBuffer& body) {
    if (m_authEnabled && c.secureEnabled && c.serverAuthOk) {
        ByteBuffer wrapped = encryptForClient(c, msgType, body);
        sendMsg(c.tcpFd, MSG_ENCRYPTED, wrapped);
        return;
    }
    sendMsg(c.tcpFd, msgType, body);
}

void SignalServer::sendClientMsg(ClientSession& c, uint8_t msgType) {
    ByteBuffer empty;
    sendClientMsg(c, msgType, empty);
}

void SignalServer::sendError(int fd, const std::string& text) {
    ByteBuffer body;
    body.writeString(text);
    auto it = m_clients.find(fd);
    if (it != m_clients.end())
        sendClientMsg(it->second, MSG_ERROR, body);
    else
        sendMsg(fd, MSG_ERROR, body);
}

void SignalServer::sendDataMsg(ClientSession& c, uint8_t msgType, const ByteBuffer& body) {
    if (c.dataFd < 0 || !c.alive) return;

    ByteBuffer outBody = body;
    uint8_t outType = msgType;
    if (m_authEnabled && c.secureEnabled && c.serverAuthOk) {
        std::vector<uint8_t> plain;
        plain.reserve(1 + body.size());
        plain.push_back(msgType);
        if (body.size() > 0)
            plain.insert(plain.end(), body.data(), body.data() + body.size());
        std::vector<uint8_t> enc = c.dataCipher.encrypt(plain.data(), plain.size());
        outBody.clear();
        if (!enc.empty())
            outBody.writeBytes(enc.data(), enc.size());
        outType = MSG_ENCRYPTED;
    }

    TcpMsgHeader hdr;
    hdr.msgType = outType;
    hdr.length  = htons(static_cast<uint16_t>(outBody.size()));

    const uint8_t* hp = reinterpret_cast<const uint8_t*>(&hdr);
    c.dataSendBuf.insert(c.dataSendBuf.end(), hp, hp + sizeof(hdr));
    if (outBody.size() > 0)
        c.dataSendBuf.insert(c.dataSendBuf.end(), outBody.data(), outBody.data() + outBody.size());

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
    std::vector<uint32_t> membersCopy = room->peerIds();
    for (uint32_t mid : membersCopy) {
        if (mid == excludePeerId) continue;
        auto it = m_peerMap.find(mid);
        if (it != m_peerMap.end())
            sendClientMsg(*it->second, msgType, body);
    }
}

} // namespace VLan

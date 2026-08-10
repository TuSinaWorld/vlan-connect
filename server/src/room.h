#ifndef VLAN_ROOM_H
#define VLAN_ROOM_H

#include "protocol.h"
#include "net_common.h"
#include "secure_frame.h"
#include <map>
#include <vector>
#include <string>
#include <set>
#include <cstring>
#include <ctime>

namespace VLan {

enum class SessionState : uint8_t {
    AwaitHello = 0,
    AwaitServerAuth,
    AwaitLogin,
    LoggedIn,
    AwaitRoomPassword,
    ResumePending,
    InRoom,
    Closing
};

inline const char* sessionStateName(SessionState state) {
    switch (state) {
    case SessionState::AwaitHello:        return "AwaitHello";
    case SessionState::AwaitServerAuth:   return "AwaitServerAuth";
    case SessionState::AwaitLogin:        return "AwaitLogin";
    case SessionState::LoggedIn:          return "LoggedIn";
    case SessionState::AwaitRoomPassword: return "AwaitRoomPassword";
    case SessionState::ResumePending:     return "ResumePending";
    case SessionState::InRoom:            return "InRoom";
    case SessionState::Closing:           return "Closing";
    }
    return "Unknown";
}

inline bool sessionHasPeerIdentity(SessionState state) {
    return state == SessionState::LoggedIn ||
           state == SessionState::AwaitRoomPassword ||
           state == SessionState::ResumePending ||
           state == SessionState::InRoom;
}

inline bool sessionCanBindDataChannel(SessionState state) {
    return sessionHasPeerIdentity(state);
}

inline bool isClientHandshakeMessage(uint8_t msgType) {
    return msgType == MSG_CLIENT_HELLO ||
           msgType == MSG_SERVER_AUTH ||
           msgType == MSG_LOGIN;
}

inline bool isKnownClientSignalMessage(uint8_t msgType) {
    switch (msgType) {
    case MSG_CLIENT_HELLO:
    case MSG_SERVER_AUTH:
    case MSG_LOGIN:
    case MSG_CREATE_ROOM:
    case MSG_JOIN_ROOM:
    case MSG_RESUME_ROOM:
    case MSG_LEAVE_ROOM:
    case MSG_LOGOUT:
    case MSG_LIST_ROOMS:
    case MSG_REQUEST_RELAY:
    case MSG_AUTH_RESPONSE:
    case MSG_PING:
    case MSG_TCP_RELAY_DATA:
    case MSG_ENCRYPTED:
        return true;
    default:
        return false;
    }
}

enum class SessionMessageAction : uint8_t {
    Dispatch = 0,
    SendStateError,
    Close,
    IgnoreUnknown
};

inline bool isClientSignalMessageAllowed(SessionState state, uint8_t msgType) {
    switch (state) {
    case SessionState::AwaitHello:
        return msgType == MSG_CLIENT_HELLO;
    case SessionState::AwaitServerAuth:
        return msgType == MSG_SERVER_AUTH;
    case SessionState::AwaitLogin:
        return msgType == MSG_LOGIN || msgType == MSG_PING;
    case SessionState::LoggedIn:
        return msgType == MSG_CREATE_ROOM || msgType == MSG_JOIN_ROOM ||
               msgType == MSG_LIST_ROOMS || msgType == MSG_LOGOUT ||
               msgType == MSG_PING;
    case SessionState::AwaitRoomPassword:
        return msgType == MSG_AUTH_RESPONSE || msgType == MSG_LIST_ROOMS ||
               msgType == MSG_LOGOUT || msgType == MSG_PING;
    case SessionState::ResumePending:
        return msgType == MSG_RESUME_ROOM || msgType == MSG_LIST_ROOMS ||
               msgType == MSG_LOGOUT || msgType == MSG_PING;
    case SessionState::InRoom:
        return msgType == MSG_LEAVE_ROOM || msgType == MSG_LOGOUT ||
               msgType == MSG_LIST_ROOMS || msgType == MSG_REQUEST_RELAY ||
               msgType == MSG_PING || msgType == MSG_TCP_RELAY_DATA;
    case SessionState::Closing:
        return false;
    }
    return false;
}

inline SessionMessageAction classifyClientSignalMessage(
    SessionState state, uint8_t msgType)
{
    if (!isKnownClientSignalMessage(msgType))
        return SessionMessageAction::IgnoreUnknown;
    if (isClientSignalMessageAllowed(state, msgType))
        return SessionMessageAction::Dispatch;
    if (isClientHandshakeMessage(msgType))
        return SessionMessageAction::Close;
    return SessionMessageAction::SendStateError;
}

template <typename Key>
inline bool bindUniqueSessionIndex(std::map<Key, int>& index,
                                   const Key& key, int signalFd) {
    typename std::map<Key, int>::iterator it = index.find(key);
    if (it != index.end() && it->second != signalFd)
        return false;
    index[key] = signalFd;
    return true;
}

template <typename Key>
inline void unbindSessionIndex(std::map<Key, int>& index,
                               const Key& key, int signalFd) {
    typename std::map<Key, int>::iterator it = index.find(key);
    if (it != index.end() && it->second == signalFd)
        index.erase(it);
}

struct ClientSession {
    int              tcpFd;
    int              dataFd;
    uint32_t         peerId;
    uint32_t         virtualIP;
    uint32_t         roomId;
    uint32_t         remoteIpv4;
    struct sockaddr_in udpAddr;
    bool             udpAddrKnown;
    std::string      name;
    std::vector<uint8_t> recvBuf;
    std::vector<uint8_t> sendBuf;
    std::vector<uint8_t> dataRecvBuf;
    std::vector<uint8_t> dataSendBuf;
    time_t           lastPing;
    time_t           dataLastPing;
    bool             alive;
    SessionState     state;
    bool             serverAuthOk;
    bool             secureEnabled;
    uint32_t         secureSessionId;
    uint8_t          clientNonce[16];
    uint8_t          serverNonce[16];
    uint8_t          clientPubKey[32];
    uint8_t          serverPrivKey[32];
    uint8_t          serverPubKey[32];
    uint8_t          secureMaster[32];
    SecureFrameCipher secureCipher;
    SecureFrameCipher dataCipher;
    SecureFrameCipher udpCipher;
    time_t           helloDeadline;
    time_t           authDeadline;
    uint8_t          sendBudgetFailures;

    uint32_t         pendingJoinRoomId;
    uint8_t          authChallenge[32];
    uint32_t         resumeRoomId;
    uint32_t         resumePeerId;
    uint8_t          resumeToken[RECONNECT_TOKEN_SIZE];

    ClientSession()
        : tcpFd(-1), dataFd(-1), peerId(0), virtualIP(0), roomId(0),
          remoteIpv4(0),
          udpAddrKnown(false),
          lastPing(0), dataLastPing(0), alive(true),
          state(SessionState::AwaitHello), serverAuthOk(false), secureEnabled(false),
          secureSessionId(0), helloDeadline(0), authDeadline(0),
          sendBudgetFailures(0),
          pendingJoinRoomId(0), resumeRoomId(0), resumePeerId(0) {
        memset(&udpAddr, 0, sizeof(udpAddr));
        memset(clientNonce, 0, sizeof(clientNonce));
        memset(serverNonce, 0, sizeof(serverNonce));
        memset(clientPubKey, 0, sizeof(clientPubKey));
        memset(serverPrivKey, 0, sizeof(serverPrivKey));
        memset(serverPubKey, 0, sizeof(serverPubKey));
        memset(secureMaster, 0, sizeof(secureMaster));
        memset(authChallenge, 0, sizeof(authChallenge));
        memset(resumeToken, 0, sizeof(resumeToken));
    }

    int relayFd() const { return (dataFd >= 0) ? dataFd : tcpFd; }
};

struct RoomLease {
    uint32_t    peerId;
    uint32_t    virtualIP;
    std::string name;
    uint8_t     token[RECONNECT_TOKEN_SIZE];
    bool        online;
    time_t      offlineSince;
    time_t      expiresAt;

    RoomLease()
        : peerId(0), virtualIP(0), online(false),
          offlineSince(0), expiresAt(0) {
        memset(token, 0, sizeof(token));
    }
};

struct Room {
    uint32_t              id;
    std::string           name;
    uint32_t              hostPeerId;
    std::vector<RoomLease> leases;
    uint8_t               maxPlayers;
    RoomTrafficPolicy     tcpPolicy;
    RoomTrafficPolicy     udpPolicy;
    uint16_t              mtu;
    uint8_t               passwordProtected;
    uint8_t               passwordHash[32];
    time_t                emptyTimestamp;

    Room() : id(0), hostPeerId(0), maxPlayers(MAX_PLAYERS),
             tcpPolicy(makeDefaultTcpPolicy()), udpPolicy(makeDefaultUdpPolicy()),
             mtu(ROOM_MTU_DEFAULT), passwordProtected(0), emptyTimestamp(0) {
        memset(passwordHash, 0, sizeof(passwordHash));
    }

    bool hasVirtualIP(uint32_t ip) const {
        for (size_t i = 0; i < leases.size(); ++i) {
            if (leases[i].virtualIP == ip) return true;
        }
        return false;
    }

    uint32_t allocateVirtualIP() const {
        for (uint32_t suffix = VNET_FIRST_HOST_SUFFIX;
             suffix <= VNET_LAST_HOST_SUFFIX; ++suffix) {
            uint32_t ip = VNET_SUBNET | suffix;
            if (!hasVirtualIP(ip)) return ip;
        }
        return 0;
    }

    bool isFull() const {
        return leases.size() >= maxPlayers;
    }

    bool hasLease(uint32_t peerId) const {
        return leaseByPeerId(peerId) != nullptr;
    }

    RoomLease* leaseByPeerId(uint32_t peerId) {
        for (size_t i = 0; i < leases.size(); ++i) {
            if (leases[i].peerId == peerId) return &leases[i];
        }
        return nullptr;
    }

    const RoomLease* leaseByPeerId(uint32_t peerId) const {
        for (size_t i = 0; i < leases.size(); ++i) {
            if (leases[i].peerId == peerId) return &leases[i];
        }
        return nullptr;
    }

    RoomLease* addLease(uint32_t peerId, const std::string& peerName,
                        const uint8_t token[RECONNECT_TOKEN_SIZE]) {
        if (isFull() || hasLease(peerId)) return nullptr;
        uint32_t ip = allocateVirtualIP();
        if (ip == 0) return nullptr;

        RoomLease lease;
        lease.peerId = peerId;
        lease.virtualIP = ip;
        lease.name = peerName;
        lease.online = true;
        lease.offlineSince = 0;
        lease.expiresAt = 0;
        if (token)
            memcpy(lease.token, token, RECONNECT_TOKEN_SIZE);
        leases.push_back(lease);
        emptyTimestamp = 0;
        ensureHost();
        return &leases.back();
    }

    bool removeLease(uint32_t peerId) {
        for (auto it = leases.begin(); it != leases.end(); ++it) {
            if (it->peerId == peerId) {
                leases.erase(it);
                if (leases.empty()) emptyTimestamp = time(nullptr);
                ensureHost();
                return true;
            }
        }
        return false;
    }

    bool markLeaseOffline(uint32_t peerId, time_t now) {
        RoomLease* lease = leaseByPeerId(peerId);
        if (!lease) return false;
        lease->online = false;
        lease->offlineSince = now;
        lease->expiresAt = now + RECONNECT_LEASE_TIMEOUT_SEC;
        return true;
    }

    bool markLeaseOnline(uint32_t peerId) {
        RoomLease* lease = leaseByPeerId(peerId);
        if (!lease) return false;
        lease->online = true;
        lease->offlineSince = 0;
        lease->expiresAt = 0;
        ensureHost();
        return true;
    }

    std::vector<uint32_t> peerIds() const {
        std::vector<uint32_t> result;
        result.reserve(leases.size());
        for (size_t i = 0; i < leases.size(); ++i)
            result.push_back(leases[i].peerId);
        return result;
    }

    void ensureHost() {
        if (leases.empty()) {
            hostPeerId = 0;
            return;
        }
        if (leaseByPeerId(hostPeerId)) return;
        for (size_t i = 0; i < leases.size(); ++i) {
            if (leases[i].online) {
                hostPeerId = leases[i].peerId;
                return;
            }
        }
        hostPeerId = leases.front().peerId;
    }
};

class RoomManager {
public:
    RoomManager() : m_nextRoomId(1) {}

    Room* createRoom(const std::string& name, uint32_t hostPeerId,
                     uint8_t maxPlayers,
                     const RoomTrafficPolicy& tcpPolicy,
                     const RoomTrafficPolicy& udpPolicy,
                     uint16_t mtu = ROOM_MTU_DEFAULT,
                     uint8_t passwordProtected = 0,
                     const uint8_t* pwdHash = nullptr) {
        uint32_t rid = allocateRoomId();
        Room& r = m_rooms[rid];
        r = Room();
        r.id            = rid;
        r.name          = name;
        r.hostPeerId    = hostPeerId;
        r.maxPlayers    = maxPlayers;
        r.tcpPolicy     = tcpPolicy;
        r.udpPolicy     = udpPolicy;
        r.mtu           = normalizeRoomMtu(mtu);
        r.passwordProtected = passwordProtected;
        if (passwordProtected && pwdHash) {
            memcpy(r.passwordHash, pwdHash, 32);
        }
        r.emptyTimestamp = 0;
        return &r;
    }

    Room* getRoom(uint32_t roomId) {
        auto it = m_rooms.find(roomId);
        return (it != m_rooms.end()) ? &it->second : nullptr;
    }

    void ensureHost(uint32_t roomId) {
        Room* r = getRoom(roomId);
        if (r) r->ensureHost();
    }

    bool cleanupEmptyRooms() {
        bool changed = false;
        for (auto it = m_rooms.begin(); it != m_rooms.end(); ) {
            if (it->second.leases.empty()) {
                m_freeRoomIds.insert(it->first);
                it = m_rooms.erase(it);
                changed = true;
            } else {
                ++it;
            }
        }
        return changed;
    }

    std::vector<Room*> listRooms() {
        std::vector<Room*> result;
        for (auto& kv : m_rooms) result.push_back(&kv.second);
        return result;
    }

    size_t size() const { return m_rooms.size(); }

    void eraseRoom(uint32_t roomId) {
        auto it = m_rooms.find(roomId);
        if (it == m_rooms.end()) return;
        m_rooms.erase(it);
        if (roomId != 0) m_freeRoomIds.insert(roomId);
    }

private:
    uint32_t allocateRoomId() {
        if (!m_freeRoomIds.empty()) {
            uint32_t id = *m_freeRoomIds.begin();
            m_freeRoomIds.erase(m_freeRoomIds.begin());
            return id;
        }
        return m_nextRoomId++;
    }

    std::map<uint32_t, Room> m_rooms;
    std::set<uint32_t> m_freeRoomIds;
    uint32_t m_nextRoomId;
};

} // namespace VLan
#endif // VLAN_ROOM_H

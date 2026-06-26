#ifndef VLAN_ROOM_H
#define VLAN_ROOM_H

#include "protocol.h"
#include "net_common.h"
#include <map>
#include <vector>
#include <string>
#include <cstring>
#include <ctime>

namespace VLan {

struct ClientSession {
    int              tcpFd;
    int              dataFd;
    uint32_t         peerId;
    uint32_t         virtualIP;
    uint32_t         roomId;
    struct sockaddr_in udpAddr;
    bool             udpAddrKnown;
    NatType          natType;
    std::string      name;
    std::vector<uint8_t> recvBuf;
    std::vector<uint8_t> sendBuf;
    std::vector<uint8_t> dataRecvBuf;
    std::vector<uint8_t> dataSendBuf;
    time_t           lastPing;
    time_t           dataLastPing;
    bool             alive;

    uint32_t         pendingJoinRoomId;
    uint8_t          authChallenge[32];
    bool             awaitingAuth;

    ClientSession()
        : tcpFd(-1), dataFd(-1), peerId(0), virtualIP(0), roomId(0),
          udpAddrKnown(false), natType(NAT_UNKNOWN),
          lastPing(0), dataLastPing(0), alive(true),
          pendingJoinRoomId(0), awaitingAuth(false) {
        memset(&udpAddr, 0, sizeof(udpAddr));
        memset(authChallenge, 0, sizeof(authChallenge));
    }

    int relayFd() const { return (dataFd >= 0) ? dataFd : tcpFd; }
};

struct Room {
    uint32_t              id;
    std::string           name;
    uint32_t              hostPeerId;
    std::vector<uint32_t> members;
    uint8_t               maxPlayers;
    TransportMode         transportMode;
    FecMode               fecMode;
    uint16_t              mtu;
    uint8_t               nextIPSuffix;
    uint8_t               encrypted;
    uint8_t               passwordHash[32];
    uint8_t               salt[16];
    uint8_t               sessionSeed[16];
    time_t                emptyTimestamp;

    Room() : id(0), hostPeerId(0), maxPlayers(MAX_PLAYERS),
             transportMode(MODE_RELAY_KCP), fecMode(FEC_NONE),
             mtu(ROOM_MTU_DEFAULT), nextIPSuffix(2), encrypted(0), emptyTimestamp(0) {
        memset(passwordHash, 0, sizeof(passwordHash));
        memset(salt, 0, sizeof(salt));
        memset(sessionSeed, 0, sizeof(sessionSeed));
    }

    uint32_t allocateVirtualIP() {
        uint32_t ip = VNET_SUBNET | nextIPSuffix;
        ++nextIPSuffix;
        return ip;
    }

    bool isFull() const {
        return static_cast<uint8_t>(members.size()) >= maxPlayers;
    }

    bool hasMember(uint32_t peerId) const {
        for (size_t i = 0; i < members.size(); ++i)
            if (members[i] == peerId) return true;
        return false;
    }

    void removeMember(uint32_t peerId) {
        for (auto it = members.begin(); it != members.end(); ++it) {
            if (*it == peerId) { members.erase(it); return; }
        }
    }
};

class RoomManager {
public:
    RoomManager() : m_nextRoomId(1) {}

    Room* createRoom(const std::string& name, uint32_t hostPeerId,
                     uint8_t maxPlayers, TransportMode mode,
                     FecMode fec = FEC_NONE,
                     uint16_t mtu = ROOM_MTU_DEFAULT,
                     uint8_t encrypted = 0,
                     const uint8_t* pwdHash = nullptr,
                     const uint8_t* salt = nullptr,
                     const uint8_t* sessionSeed = nullptr) {
        uint32_t rid = m_nextRoomId++;
        Room& r = m_rooms[rid];
        r.id            = rid;
        r.name          = name;
        r.hostPeerId    = hostPeerId;
        r.maxPlayers    = maxPlayers;
        r.transportMode = mode;
        r.fecMode       = fec;
        r.mtu           = normalizeRoomMtu(mtu);
        r.encrypted     = encrypted;
        if (encrypted && pwdHash && salt && sessionSeed) {
            memcpy(r.passwordHash, pwdHash, 32);
            memcpy(r.salt, salt, 16);
            memcpy(r.sessionSeed, sessionSeed, 16);
        }
        r.members.push_back(hostPeerId);
        return &r;
    }

    Room* getRoom(uint32_t roomId) {
        auto it = m_rooms.find(roomId);
        return (it != m_rooms.end()) ? &it->second : nullptr;
    }

    bool joinRoom(uint32_t roomId, uint32_t peerId) {
        Room* r = getRoom(roomId);
        if (!r || r->isFull() || r->hasMember(peerId)) return false;
        r->members.push_back(peerId);
        r->emptyTimestamp = 0;
        return true;
    }

    void leaveRoom(uint32_t roomId, uint32_t peerId) {
        Room* r = getRoom(roomId);
        if (!r) return;
        r->removeMember(peerId);
        if (r->members.empty()) {
            r->emptyTimestamp = time(nullptr);
        } else if (r->hostPeerId == peerId) {
            r->hostPeerId = r->members.front();
        }
    }

    void cleanupEmptyRooms() {
        static const int ROOM_GRACE_PERIOD_SEC = 60;
        time_t now = time(nullptr);
        for (auto it = m_rooms.begin(); it != m_rooms.end(); ) {
            if (it->second.members.empty() &&
                it->second.emptyTimestamp > 0 &&
                (now - it->second.emptyTimestamp) > ROOM_GRACE_PERIOD_SEC) {
                it = m_rooms.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::vector<Room*> listRooms() {
        std::vector<Room*> result;
        for (auto& kv : m_rooms) result.push_back(&kv.second);
        return result;
    }

private:
    std::map<uint32_t, Room> m_rooms;
    uint32_t m_nextRoomId;
};

} // namespace VLan
#endif // VLAN_ROOM_H

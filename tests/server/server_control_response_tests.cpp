// Exercise actual SignalServer state transitions and encrypted send queues.
// The only descriptors are local socketpairs; no listener, TUN or privilege.
#include "../../server/src/signal_server.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

namespace VLan {

class ServerControlResponseTests {
    struct ClientSocket {
        int remote;
        ClientSocket() : remote(-1) {}
        ~ClientSocket() { if (remote >= 0) close(remote); }
    };

    static ClientSession& client(SignalServer& server, ClientSocket& socket,
                                  uint32_t peer, const char* name) {
        int sockets[2];
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
        assert(setNonBlocking(sockets[0]));
        assert(setNonBlocking(sockets[1]));
        socket.remote = sockets[1];
        ClientSession& result = server.m_clients[sockets[0]];
        result.tcpFd = sockets[0];
        result.peerId = peer;
        result.name = name;
        result.state = SessionState::LoggedIn;
        result.serverAuthOk = true;
        result.secureEnabled = true;
        result.secureSessionId = peer + 100;
        for (size_t i = 0; i < sizeof(result.secureMaster); ++i)
            result.secureMaster[i] = static_cast<uint8_t>(peer + i + 17);
        result.secureCipher.init(result.secureMaster, false, "signal");
        assert(server.bindPeerIndex(result, peer));
        assert(server.bindSecureSessionIndex(result, result.secureSessionId));
        return result;
    }

    static bool hasBytes(ClientSocket& socket) {
        uint8_t byte;
        const ssize_t n = recv(socket.remote, &byte, 1, MSG_PEEK);
        assert(n >= 0 || errno == EAGAIN || errno == EWOULDBLOCK);
        return n > 0;
    }

    static ByteBuffer createRequest() {
        ByteBuffer request;
        request.writeString("r"); request.writeU8(8);
        const RoomTrafficPolicy policies[] = {makeDefaultTcpPolicy(), makeDefaultUdpPolicy()};
        for (const RoomTrafficPolicy& policy : policies) {
            request.writeU8(policy.transportMode);
            request.writeU8(policy.fecMode);
            request.writeU8(policy.kcpProfile);
        }
        request.writeU8(0); request.writeU16(ROOM_MTU_DEFAULT);
        return request;
    }

    static Room* seededRoom(SignalServer& server, ClientSession& owner) {
        Room* room = server.m_rooms.createRoom("r", owner.peerId, 8,
            makeDefaultTcpPolicy(), makeDefaultUdpPolicy());
        uint8_t token[RECONNECT_TOKEN_SIZE];
        memset(token, 0x51, sizeof(token));
        RoomLease* lease = room->addLease(owner.peerId, owner.name, token);
        assert(lease);
        owner.roomId = owner.resumeRoomId = room->id;
        owner.resumePeerId = owner.peerId;
        owner.virtualIP = lease->virtualIP;
        memcpy(owner.resumeToken, token, sizeof(token));
        owner.state = SessionState::InRoom;
        server.m_publishedRooms = server.collectRoomListState();
        return room;
    }

    static void createFailureCannotPublishSmallerList() {
        SignalServer server;
        ClientSocket creatorSocket, observerSocket;
        ClientSession& creator = client(server, creatorSocket, 1, "a");
        ClientSession& observer = client(server, observerSocket, 2, "b");
        server.m_limits.maxSendBufferBytes = 70;
        const uint64_t revision = server.m_roomListRevision;
        ByteBuffer request = createRequest();
        server.onCreateRoom(creator, request.data(), request.size());
        assert(!creator.alive && creator.sendBudgetFailures == 1);
        assert(creator.roomId == 0 && creator.virtualIP == 0);
        assert(creator.resumeRoomId == 0 && creator.resumePeerId == 0);
        const uint8_t zero[RECONNECT_TOKEN_SIZE] = {};
        assert(memcmp(creator.resumeToken, zero, sizeof(zero)) == 0);
        assert(server.m_rooms.size() == 0 && server.m_publishedRooms.empty());
        assert(server.m_roomListRevision == revision);
        assert(!hasBytes(creatorSocket) && !hasBytes(observerSocket));

        // Reproduce the old numerical hole: an encrypted creation response is
        // 77 bytes, but a short-name room-list push is only 65 bytes.
        const size_t createdBytes = 49 + 1 + SECURE_FRAME_OVERHEAD + sizeof(TcpMsgHeader);
        assert(createdBytes == 77 && createdBytes > server.m_limits.maxSendBufferBytes);
        SignalServer::RoomListState fake;
        fake.roomId = 1; fake.roomName = "r"; fake.playerCount = 1;
        fake.maxPlayers = 8; fake.tcpPolicy = makeDefaultTcpPolicy();
        fake.udpPolicy = makeDefaultUdpPolicy(); fake.passwordProtected = 0;
        fake.mtu = ROOM_MTU_DEFAULT;
        ByteBuffer smaller;
        smaller.writeU64(1); smaller.writeU64(2); smaller.writeU16(1);
        smaller.writeU8(ROOM_LIST_UPSERT); server.writeRoomListItem(smaller, fake);
        assert(smaller.size() + 1 + SECURE_FRAME_OVERHEAD + sizeof(TcpMsgHeader) == 65);
        assert(server.sendClientMsg(observer, MSG_ROOM_LIST_PUSH, smaller));
        assert(observer.alive && hasBytes(observerSocket));
        assert(!server.sendClientMsg(creator, MSG_ROOM_LIST_PUSH, smaller));
        assert(!hasBytes(creatorSocket));
    }

    static void joinFailureLeavesExistingMembersUntouched() {
        SignalServer server;
        ClientSocket ownerSocket, joiningSocket;
        ClientSession& owner = client(server, ownerSocket, 1, "a");
        Room* room = seededRoom(server, owner);
        const uint32_t roomId = room->id;
        const uint32_t ownerIp = owner.virtualIP;
        const uint64_t revision = server.m_roomListRevision;
        ClientSession& joining = client(server, joiningSocket, 2, "b");
        server.m_limits.maxSendBufferBytes = 70;
        server.completeJoin(joining, room);
        assert(!joining.alive && joining.roomId == 0 && joining.resumeRoomId == 0);
        room = server.m_rooms.getRoom(roomId);
        assert(room && room->leases.size() == 1 && room->hostPeerId == owner.peerId);
        assert(room->leases[0].peerId == owner.peerId && room->leases[0].online);
        assert(owner.alive && owner.roomId == roomId && owner.virtualIP == ownerIp);
        assert(server.m_roomListRevision == revision);
        assert(!hasBytes(ownerSocket) && !hasBytes(joiningSocket));
    }

    static void resumeFailureKeepsExistingLease() {
        SignalServer server;
        ClientSocket ownerSocket;
        ClientSession& owner = client(server, ownerSocket, 1, "a");
        Room* room = seededRoom(server, owner);
        const uint32_t roomId = room->id, vip = owner.virtualIP;
        uint8_t token[RECONNECT_TOKEN_SIZE]; memcpy(token, owner.resumeToken, sizeof(token));
        room->markLeaseOffline(owner.peerId, time(nullptr));
        owner.roomId = 0; owner.virtualIP = 0; owner.state = SessionState::ResumePending;
        const uint64_t revision = server.m_roomListRevision;
        server.m_limits.maxSendBufferBytes = 1;
        server.completeResume(owner, room, room->leaseByPeerId(owner.peerId));
        assert(!owner.alive);
        const int fd = owner.tcpFd;
        server.handleClientDisconnect(fd);
        room = server.m_rooms.getRoom(roomId);
        assert(room && room->leases.size() == 1);
        const RoomLease& lease = room->leases[0];
        assert(!lease.online && lease.expiresAt >= time(nullptr));
        assert(lease.virtualIP == vip && memcmp(lease.token, token, sizeof(token)) == 0);
        assert(server.m_roomListRevision == revision);
        assert(!hasBytes(ownerSocket));
    }

    static void failedSocketFlushRollsBackOnlyNewMembership() {
        for (bool creating : {true, false}) {
            SignalServer server;
            ClientSocket ownerSocket, failingSocket;
            ClientSession& owner = client(server, ownerSocket, 1, "a");
            Room* existing = creating ? nullptr : seededRoom(server, owner);
            const uint32_t existingRoomId = existing ? existing->id : 0;
            ClientSession& failing = client(server, failingSocket, 2, "b");
            close(failingSocket.remote);
            failingSocket.remote = -1;
            const uint64_t revision = server.m_roomListRevision;
            if (creating) {
                ByteBuffer request = createRequest();
                server.onCreateRoom(failing, request.data(), request.size());
            } else {
                server.completeJoin(failing, existing);
            }
            assert(!failing.alive && failing.roomId == 0 && failing.resumeRoomId == 0);
            assert(server.m_roomListRevision == revision && !hasBytes(ownerSocket));
            assert(server.m_globalSendBytes > 0); // Failed flush is still owned until teardown.
            const int fd = failing.tcpFd;
            server.handleClientDisconnect(fd);
            assert(server.m_globalSendBytes == 0);
            if (creating) {
                assert(server.m_rooms.size() == 0);
            } else {
                existing = server.m_rooms.getRoom(existingRoomId);
                assert(existing && existing->leases.size() == 1);
                assert(existing->leases[0].peerId == owner.peerId && existing->leases[0].online);
                assert(owner.alive && owner.roomId == existingRoomId);
            }
        }
    }

    static void requiredFailureIsImmediateOptionalBackpressureRemains() {
        for (uint8_t type : {uint8_t(MSG_LOGIN_RESP), uint8_t(MSG_AUTH_CHALLENGE),
                             uint8_t(MSG_LOGOUT_ACK), uint8_t(MSG_ERROR)}) {
            SignalServer server;
            ClientSocket socket;
            ClientSession& c = client(server, socket, 1, "a");
            server.m_limits.maxSendBufferBytes = 1;
            ByteBuffer response; response.writeU32(1);
            assert(!server.sendClientMsg(c, type, response));
            assert(!c.alive && c.sendBudgetFailures == 1 && !hasBytes(socket));
        }
        SignalServer server;
        ClientSocket socket;
        ClientSession& c = client(server, socket, 1, "a");
        server.m_limits.maxSendBufferBytes = 1;
        ByteBuffer response; response.writeU64(1);
        assert(!server.sendClientMsg(c, MSG_ROOM_LIST_PUSH, response));
        assert(c.alive && c.sendBudgetFailures == 1 && !hasBytes(socket));
        assert(!server.sendClientMsg(c, MSG_TCP_RELAY_DATA, response));
        assert(c.alive && c.sendBudgetFailures == 2 && !hasBytes(socket));
    }

    static void logoutCleansMembershipUnknownToClient() {
        for (bool rejectAck : {false, true}) {
            SignalServer server;
            ClientSocket socket;
            ClientSession& c = client(server, socket, 1, "a");
            ByteBuffer request = createRequest();
            server.onCreateRoom(c, request.data(), request.size());
            assert(c.alive && c.roomId != 0 && server.m_rooms.size() == 1);
            if (rejectAck) server.m_limits.maxSendBufferBytes = 1;
            // No room ID is required. Cleanup still commits when the ACK
            // itself cannot be queued, so timeout recovery cannot orphan a lease.
            server.onLogout(c);
            assert(c.state == SessionState::Closing && c.roomId == 0 && c.peerId == 0);
            assert(c.resumeRoomId == 0 && server.m_rooms.size() == 0);
            assert(server.m_peerMap.empty() && server.m_publishedRooms.empty());
            assert(c.alive == !rejectAck && hasBytes(socket));
        }
    }

public:
    static void run() {
        createFailureCannotPublishSmallerList();
        joinFailureLeavesExistingMembersUntouched();
        resumeFailureKeepsExistingLease();
        failedSocketFlushRollsBackOnlyNewMembership();
        requiredFailureIsImmediateOptionalBackpressureRemains();
        logoutCleansMembershipUnknownToClient();
    }
};
} // namespace VLan

int main() {
    VLan::sock_init();
    VLan::ServerControlResponseTests::run();
    std::puts("Server required control responses: PASS");
    return 0;
}

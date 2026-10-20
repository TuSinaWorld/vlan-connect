#include "cli_net.h"
#include "cli_log.h"
#include "../support/frame_test_utils.h"
#include <cassert>
#include <chrono>
#include <thread>
#include <cstdio>

bool VLan::g_verboseLog = false;
using namespace VLan;
using namespace VLanTest;

namespace {
enum Attack { Normal, OldVersion, UnknownProfile, ZeroPublicKey, TamperedSalt,
              BadServerProof, BadSessionId, WrongPassword, CancelKdf, PlainLogin, PlainCreated, PlainJoin, LegacyError, DuplicateHello, PrematureProof, RoomWrongPassword };

// A wire peer drives the actual production CLI state machine over loopback.
class Fixture {
public:
    explicit Fixture(Attack attack) : mode(attack), accepted(SOCK_INVALID),
        authReceived(false), created(false), joined(false), helloSent(false) {
        memset(priv, 7, sizeof(priv)); memset(nonce, 9, sizeof(nonce));
        memset(salt, 4, sizeof(salt)); memset(roomChallenge, 2, sizeof(roomChallenge));
        crypto_x25519_public_key(pub, priv);
        const std::string password = "server-test-password";
        assert(hashPasswordSalted(reinterpret_cast<const uint8_t*>(password.data()),
            password.size(), salt, authHash));
        listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        assert(listener != SOCK_INVALID);
        sockaddr_in address = {}; address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        assert(::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
        assert(::listen(listener, 1) == 0);
        socklen_t length = sizeof(address);
        assert(getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) == 0);
        port = ntohs(address.sin_port);
        setNonBlocking(listener);
    }
    ~Fixture() {
        if (accepted != SOCK_INVALID) sock_close(accepted);
        sock_close(listener);
    }
    void poll() {
        if (accepted == SOCK_INVALID) {
            accepted = ::accept(listener, nullptr, nullptr);
            if (accepted == SOCK_INVALID) return;
            setNonBlocking(accepted);
        }
        char incoming[8192];
        const int count = ::recv(accepted, incoming, sizeof(incoming), 0);
        if (count > 0) stream.insert(stream.end(), incoming, incoming + count);
        uint8_t type; std::vector<uint8_t> payload;
        while (popTcpFrame(&stream, &type, &payload)) receive(type, payload);
    }
    uint16_t port;
    bool authReceived, created, joined, helloSent;
private:
    void send(uint8_t type, const ByteBuffer& body, bool secure = false) {
        std::vector<uint8_t> payload;
        if (secure) {
            std::vector<uint8_t> plain(1, type);
            if (body.size()) plain.insert(plain.end(), body.data(), body.data() + body.size());
            payload = cipher.encrypt(plain.data(), plain.size()); type = MSG_ENCRYPTED;
        } else if (body.size()) payload.assign(body.data(), body.data() + body.size());
        const std::vector<uint8_t> wire = makeTcpFrame(type, payload);
        assert(::send(accepted, reinterpret_cast<const char*>(wire.data()),
            static_cast<int>(wire.size()), 0) == static_cast<int>(wire.size()));
    }
    void roomResponse(uint8_t type, bool secure = true) {
        ByteBuffer body; body.writeU32(12); body.writeU32(VNET_SUBNET | 2);
        const RoomTrafficPolicy policies[] = {makeDefaultTcpPolicy(), makeDefaultUdpPolicy()};
        for (const RoomTrafficPolicy& policy : policies) {
            body.writeU8(policy.transportMode); body.writeU8(policy.fecMode); body.writeU8(policy.kcpProfile);
        }
        body.writeU8(1); body.writeU16(ROOM_MTU_DEFAULT);
        if (type == MSG_JOIN_RESP) {
            body.writeU8(1); body.writeU32(1); body.writeU32(VNET_SUBNET | 2); body.writeString("Alice");
        }
        uint8_t token[RECONNECT_TOKEN_SIZE] = {}; token[0] = 1;
        body.writeBytes(token, sizeof(token)); send(type, body, secure);
    }
    void receive(uint8_t type, const std::vector<uint8_t>& payload) {
        if (type == MSG_CLIENT_HELLO) {
            if (mode == PlainLogin || mode == PlainCreated || mode == PlainJoin || mode == LegacyError || mode == PrematureProof) {
                ByteBuffer bad;
                if (mode == PlainLogin) { bad.writeU32(1); bad.writeU16(9); bad.writeU8(0); send(MSG_LOGIN_RESP, bad); }
                if (mode == PlainCreated || mode == PlainJoin) roomResponse(mode == PlainCreated ? MSG_ROOM_CREATED : MSG_JOIN_RESP, false);
                if (mode == LegacyError) { bad.writeString("Protocol version mismatch"); send(MSG_ERROR, bad); }
                if (mode == PrematureProof) { uint8_t zero[32] = {}; bad.writeU32(1); bad.writeBytes(zero,32); send(MSG_SERVER_AUTH_OK,bad); }
                return;
            }
            ByteBuffer hello(payload.data(), payload.size()); assert(hello.readU16() == 9);
            uint8_t clientNonce[16], clientPub[32], shared[32];
            hello.readBytes(clientNonce, 16); hello.readBytes(clientPub, 32);
            crypto_x25519(shared, priv, clientPub);
            deriveSecureMaster(master, shared, authHash, clientNonce, nonce, clientPub, pub, salt);
            cipher.init(master, false, "signal");
            ByteBuffer reply; reply.writeU16(mode == OldVersion ? 8 : 9); reply.writeU8(1);
            reply.writeU8(mode == UnknownProfile ? 255 : AUTH_KDF_PROFILE);
            uint8_t wireSalt[16]; memcpy(wireSalt, salt, 16);
            if (mode == TamperedSalt) wireSalt[0] ^= 1;
            reply.writeBytes(wireSalt, 16); reply.writeBytes(nonce, 16);
            const uint8_t zero[32] = {};
            reply.writeBytes(mode == ZeroPublicKey ? zero : pub, 32);
            send(MSG_SERVER_HELLO, reply);
            if (mode == DuplicateHello) send(MSG_SERVER_HELLO, reply);
            helloSent = true; return;
        }
        if (type == MSG_SERVER_AUTH) {
            authReceived = true;
            uint8_t proof[32]; computeClientAuthProof(proof, master, authHash);
            assert(payload.size() == sizeof(proof));
            if (mode == TamperedSalt || mode == WrongPassword) {
                assert(crypto_verify32(proof, payload.data()) != 0);
                ByteBuffer error; error.writeU16(static_cast<uint16_t>(AuthErrorCode::InvalidPassword));
                error.writeString("Invalid server password"); send(MSG_ERROR, error); return;
            }
            assert(crypto_verify32(proof, payload.data()) == 0);
            computeServerAuthProof(proof, master, authHash);
            if (mode == BadServerProof) proof[0] ^= 1;
            ByteBuffer reply; reply.writeU32(deriveSessionId(master) + (mode == BadSessionId ? 1 : 0));
            reply.writeBytes(proof, 32); send(MSG_SERVER_AUTH_OK, reply); return;
        }
        assert(type == MSG_ENCRYPTED);
        std::vector<uint8_t> plain;
        assert(cipher.decrypt(payload.data(), payload.size(), &plain));
        assert(!plain.empty()); type = plain[0];
        ByteBuffer body(plain.data() + 1, plain.size() - 1);
        if (type == MSG_LOGIN) {
            assert(body.readString() == "Alice"); assert(body.readU16() == 9);
            ByteBuffer reply; reply.writeU32(1); reply.writeU16(9); reply.writeU8(0);
            send(MSG_LOGIN_RESP, reply, true); return;
        }
        if (type == MSG_CREATE_ROOM) {
            assert(body.readString() == "Private room"); body.readU8();
            for (int i = 0; i != 6; ++i) body.readU8(); assert(body.readU8() == 1);
            body.readBytes(roomSalt, 16); body.readBytes(roomHash, 32);
            assert(body.readU16() == ROOM_MTU_DEFAULT && body.atEnd());
            const std::string password = "private-room-password";
            uint8_t expected[32];
            assert(hashPasswordSalted(reinterpret_cast<const uint8_t*>(password.data()),
                password.size(), roomSalt, expected));
            assert(crypto_verify32(roomHash, expected) == 0);
            created = true; roomResponse(MSG_ROOM_CREATED); return;
        }
        if (type == MSG_LEAVE_ROOM) return;
        if (type == MSG_JOIN_ROOM) {
            assert(body.readU32() == 12);
            ByteBuffer challenge; challenge.writeU8(AUTH_KDF_PROFILE);
            challenge.writeBytes(roomSalt, 16); challenge.writeBytes(roomChallenge, 32);
            send(MSG_AUTH_CHALLENGE, challenge, true); return;
        }
        if (type == MSG_AUTH_RESPONSE) {
            uint8_t proof[32]; computeChallengeResponse(roomHash, roomChallenge, proof);
            assert(body.size() == 32);
            if (mode == RoomWrongPassword && crypto_verify32(body.data(), proof) != 0) {
                ByteBuffer error; error.writeU16(static_cast<uint16_t>(AuthErrorCode::RoomPassword));
                error.writeString("Invalid room password"); send(MSG_ERROR, error, true); return;
            }
            assert(crypto_verify32(body.data(), proof) == 0);
            joined = true; roomResponse(MSG_JOIN_RESP); return;
        }
        assert(false && "unexpected CLI message");
    }
    Attack mode;
    socket_t listener, accepted;
    std::vector<uint8_t> stream;
    uint8_t priv[32], pub[32], nonce[16], salt[16], authHash[32], master[32];
    uint8_t roomSalt[16], roomHash[32], roomChallenge[32];
    SecureFrameCipher cipher;
};

void test(Attack attack) {
    Fixture server(attack); CliSignalClient client;
    client.setServerPassword(attack == WrongPassword ? "wrong-server-password" : "server-test-password");
    bool authenticated = false, done = false, fatal = false, disconnected = false, cancelled = false;
    int genericErrors = 0, roomErrors = 0;
    client.onConnected = [&]() { authenticated = true; client.login("Alice"); };
    client.onLoginResponse = [&](uint32_t, bool) {
        client.createRoom("Private room", 8, makeDefaultTcpPolicy(), makeDefaultUdpPolicy(),
            ROOM_MTU_DEFAULT, true, "private-room-password");
        assert(client.roomOperationPending());
        client.joinRoom(999, "wrong");
        client.createRoom("Ignored room", 8, makeDefaultTcpPolicy(), makeDefaultUdpPolicy(), ROOM_MTU_DEFAULT, false, "");
        uint8_t token[RECONNECT_TOKEN_SIZE] = {1}; client.resumeRoom(999, 888, token);
    };
    client.onRoomCreated = [&](uint32_t, uint32_t, RoomTrafficPolicy, RoomTrafficPolicy, uint16_t, bool, const Buffer&) {
        assert(!client.roomOperationPending()); client.leaveRoom();
        client.joinRoom(12, attack == RoomWrongPassword ? "wrong-room-password" : "private-room-password");
        assert(client.roomOperationPending()); client.joinRoom(999, "ignored");
    };
    client.onJoinResponse = [&](uint32_t, uint32_t, RoomTrafficPolicy, RoomTrafficPolicy, uint16_t, bool,
        const std::vector<PeerInfo>&, const Buffer&) { assert(!client.roomOperationPending()); done = true; };
    client.onFatalAuthError = [&](uint16_t code, const std::string&) {
        if (code == static_cast<uint16_t>(AuthErrorCode::RoomPassword)) { ++roomErrors; assert(client.isConnected()); return; }
        assert(isFatalAuthError(code)); fatal = true;
    };
    client.onServerError = [&](const std::string&) {
        assert(fatal || roomErrors == 1); ++genericErrors;
        if (roomErrors == 1 && !fatal) { assert(!client.roomOperationPending()); client.joinRoom(12, "private-room-password"); }
    };
    client.onDisconnected = [&]() { disconnected = true; };
    assert(client.connectTo("127.0.0.1", server.port));
    const uint32_t started = currentTimeMs();
    while (currentTimeMs() - started < 8000 && !done && !disconnected) {
        server.poll();
        if (client.fd() != SOCK_INVALID) {
            fd_set readSet, writeSet; FD_ZERO(&readSet); FD_ZERO(&writeSet);
            const socket_t fd = client.fd(); FD_SET(fd, &readSet); FD_SET(fd, &writeSet);
            timeval timeout = {0, 1000};
            assert(select(static_cast<int>(fd + 1), &readSet, &writeSet, nullptr, &timeout) >= 0);
            if (FD_ISSET(fd, &writeSet)) client.onWritable();
            if (client.fd() == fd && FD_ISSET(fd, &readSet)) client.onReadable();
        }
        if (attack == CancelKdf && client.authInProgress() && !cancelled) {
            client.disconnect(); cancelled = true;
        }
        client.checkTimeouts();
        if (cancelled && currentTimeMs() - started > 500) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (attack == Normal || attack == RoomWrongPassword) assert(authenticated && done && server.created && server.joined && !fatal);
    else if (attack == CancelKdf) assert(cancelled && !authenticated && !server.authReceived);
    else assert(fatal && genericErrors == 1 && disconnected && !authenticated);
    client.disconnect();
}
}

int main() {
#ifdef _WIN32
    WSADATA data; assert(WSAStartup(MAKEWORD(2, 2), &data) == 0);
#endif
    for (int attack = Normal; attack <= RoomWrongPassword; ++attack) test(static_cast<Attack>(attack));
    puts("auth_v9_integration_tests: production CLI handshake, room KDF, rejection and cancellation passed");
#ifdef _WIN32
    WSACleanup();
#endif
}

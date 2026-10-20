#include "../../client/src/network/signal_client.h"
#include "../../common/net_common.h"
#include "../../common/payload_cipher.h"
#include "../../common/secure_frame.h"
#include "../support/frame_test_utils.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <cassert>
#include <functional>
#include <vector>

bool VLan::g_verboseLog = false;

namespace {

using namespace VLan;
using namespace VLanTest;

bool samePolicy(const RoomTrafficPolicy& a, const RoomTrafficPolicy& b) {
    return a.transportMode == b.transportMode && a.fecMode == b.fecMode &&
           a.kcpProfile == b.kcpProfile;
}

void writePolicy(ByteBuffer& body, const RoomTrafficPolicy& policy) {
    body.writeU8(policy.transportMode);
    body.writeU8(policy.fecMode);
    body.writeU8(policy.kcpProfile);
}

class PolicyFixtureServer {
public:
    PolicyFixtureServer(const RoomTrafficPolicy& tcp, const RoomTrafficPolicy& udp)
        : m_socket(nullptr), m_tcp(tcp), m_udp(udp) {
        // Deterministic fixture keys are only used on loopback, never in production.
        memset(m_private, 7, sizeof(m_private));
        memset(m_nonce, 9, sizeof(m_nonce));
        crypto_x25519_public_key(m_public, m_private);
        const char password[] = "policy-test-password";
        assert(secureRandomBytes(m_salt, sizeof(m_salt)));
        assert(hashPasswordSalted(reinterpret_cast<const uint8_t*>(password),
                            sizeof(password) - 1, m_salt, m_authHash));
        assert(m_server.listen(QHostAddress::LocalHost, 0));
        QObject::connect(&m_server, &QTcpServer::newConnection, &m_server, [this]() {
            m_socket = m_server.nextPendingConnection();
            QObject::connect(m_socket, &QTcpSocket::readyRead, &m_server, [this]() {
                const QByteArray bytes = m_socket->readAll();
                m_stream.insert(m_stream.end(), bytes.begin(), bytes.end());
                uint8_t type = 0;
                std::vector<uint8_t> payload;
                while (popTcpFrame(&m_stream, &type, &payload))
                    receive(type, payload);
            });
        });
    }

    quint16 port() const { return m_server.serverPort(); }

private:
    void send(uint8_t type, const ByteBuffer& body, bool encrypted = true) {
        std::vector<uint8_t> payload;
        if (encrypted) {
            std::vector<uint8_t> plain(1, type);
            if (body.size()) plain.insert(plain.end(), body.data(), body.data() + body.size());
            payload = m_cipher.encrypt(plain.data(), plain.size());
            type = MSG_ENCRYPTED;
        } else if (body.size()) {
            payload.assign(body.data(), body.data() + body.size());
        }
        const std::vector<uint8_t> frame = makeTcpFrame(type, payload);
        assert(m_socket->write(reinterpret_cast<const char*>(frame.data()),
                               static_cast<qint64>(frame.size())) == static_cast<qint64>(frame.size()));
    }

    void receive(uint8_t type, const std::vector<uint8_t>& payload) {
        if (type == MSG_CLIENT_HELLO) {
            ByteBuffer hello(payload.data(), payload.size());
            assert(hello.readU16() == PROTOCOL_VERSION);
            uint8_t clientNonce[16], clientPublic[32], shared[32];
            hello.readBytes(clientNonce, sizeof(clientNonce));
            hello.readBytes(clientPublic, sizeof(clientPublic));
            assert(hello.atEnd());
            crypto_x25519(shared, m_private, clientPublic);
            deriveSecureMaster(m_master, shared, m_authHash, clientNonce, m_nonce,
                               clientPublic, m_public, m_salt);
            m_cipher.init(m_master, false, "signal");
            ByteBuffer reply;
            reply.writeU16(PROTOCOL_VERSION);
            reply.writeU8(1);
            reply.writeU8(AUTH_KDF_PROFILE);
            reply.writeBytes(m_salt, sizeof(m_salt));
            reply.writeBytes(m_nonce, sizeof(m_nonce));
            reply.writeBytes(m_public, sizeof(m_public));
            send(MSG_SERVER_HELLO, reply, false);
            return;
        }
        if (type == MSG_SERVER_AUTH) {
            uint8_t proof[32];
            computeClientAuthProof(proof, m_master, m_authHash);
            assert(payload.size() == sizeof(proof));
            assert(crypto_verify32(proof, payload.data()) == 0);
            computeServerAuthProof(proof, m_master, m_authHash);
            ByteBuffer reply;
            reply.writeU32(deriveSessionId(m_master));
            reply.writeBytes(proof, sizeof(proof));
            send(MSG_SERVER_AUTH_OK, reply, false);
            return;
        }
        assert(type == MSG_ENCRYPTED);
        std::vector<uint8_t> plain;
        assert(m_cipher.decrypt(payload.data(), payload.size(), &plain));
        assert(!plain.empty() && plain[0] == MSG_LOGIN);
        ByteBuffer login;
        login.writeU32(1);
        login.writeU16(PROTOCOL_VERSION);
        login.writeU8(0);
        send(MSG_LOGIN_RESP, login);
        sendFixtures();
    }

    void writeRoom(ByteBuffer& body, uint32_t id) {
        body.writeU32(id);
        body.writeString("Room");
        body.writeU8(3);
        body.writeU8(8);
        writePolicy(body, m_tcp);
        writePolicy(body, m_udp);
        body.writeU8(0);
        body.writeU16(ROOM_MTU_SAFE);
    }

    void sendFixtures() {
        uint8_t token[RECONNECT_TOKEN_SIZE] = {};
        token[0] = 1;
        ByteBuffer created;
        created.writeU32(10);
        created.writeU32(VNET_SUBNET | 2);
        writePolicy(created, m_tcp);
        writePolicy(created, m_udp);
        created.writeU8(0);
        created.writeU16(ROOM_MTU_SAFE);
        ByteBuffer joined = created;
        created.writeBytes(token, sizeof(token));
        send(MSG_ROOM_CREATED, created);
        joined.writeU8(3);
        const char* names[] = { "Alpha", "Bravo", "Charlie" };
        for (uint32_t i = 0; i < 3; ++i) {
            joined.writeU32(i + 1);
            joined.writeU32(VNET_SUBNET | (i + 2));
            joined.writeString(names[i]);
        }
        joined.writeBytes(token, sizeof(token));
        send(MSG_JOIN_RESP, joined);
        ByteBuffer list;
        list.writeU64(1);
        list.writeU16(0);
        list.writeU16(1);
        list.writeU16(2);
        writeRoom(list, 10);
        writeRoom(list, 11);
        send(MSG_ROOM_LIST, list);
        ByteBuffer delta;
        delta.writeU64(1);
        delta.writeU64(2);
        delta.writeU16(1);
        delta.writeU8(ROOM_LIST_UPSERT);
        writeRoom(delta, 12);
        send(MSG_ROOM_LIST_PUSH, delta);
    }

    QTcpServer m_server;
    QTcpSocket* m_socket;
    RoomTrafficPolicy m_tcp, m_udp;
    std::vector<uint8_t> m_stream;
    uint8_t m_salt[16];
    uint8_t m_private[32], m_public[32], m_nonce[16], m_authHash[32], m_master[32];
    SecureFrameCipher m_cipher;
};

void testGuiPolicyCallbacks(TransportMode mode) {
    RoomTrafficPolicy tcp = makeDefaultTcpPolicy();
    tcp.transportMode = mode;
    tcp.fecMode = mode == MODE_RELAY_TCP ? FEC_NONE : FEC_70;
    tcp.kcpProfile = KCP_PROFILE_BULK;
    RoomTrafficPolicy udp = makeDefaultUdpPolicy();
    udp.transportMode = mode == MODE_RELAY_RAW_UDP ? MODE_RELAY_TCP : MODE_RELAY_RAW_UDP;
    udp.fecMode = udp.transportMode == MODE_RELAY_TCP ? FEC_NONE : FEC_30;
    PolicyFixtureServer server(tcp, udp);
    SignalClient client;
    client.setServerPassword(QStringLiteral("policy-test-password"));
    int created = 0, joined = 0, lists = 0;
    QObject::connect(&client, &SignalClient::serverError, &client,
                     [](const QString&) { assert(false && "unexpected GUI signal error"); });
    QObject::connect(&client, &SignalClient::connected, &client, [&]() {
        client.login(QStringLiteral("Alpha"));
    });
    QObject::connect(&client, &SignalClient::roomCreated, &client,
        [&](uint32_t id, uint32_t ip, RoomTrafficPolicy a, RoomTrafficPolicy b,
            uint16_t mtu, bool protectedRoom, QByteArray token) {
            assert(id == 10 && ip == (VNET_SUBNET | 2) && mtu == ROOM_MTU_SAFE);
            assert(!protectedRoom && token.size() == RECONNECT_TOKEN_SIZE);
            assert(samePolicy(a, tcp) && samePolicy(b, udp));
            ++created;
        });
    QObject::connect(&client, &SignalClient::joinResponse, &client,
        [&](uint32_t id, uint32_t ip, RoomTrafficPolicy a, RoomTrafficPolicy b,
            uint16_t mtu, bool protectedRoom, QList<PeerInfo> members, QByteArray token) {
            assert(id == 10 && ip == (VNET_SUBNET | 2) && mtu == ROOM_MTU_SAFE);
            assert(!protectedRoom && token.size() == RECONNECT_TOKEN_SIZE && members.size() == 3);
            assert(samePolicy(a, tcp) && samePolicy(b, udp));
            for (int i = 0; i < members.size(); ++i) {
                assert(members[i].peerId == static_cast<uint32_t>(i + 1));
                assert(members[i].virtualIP == (VNET_SUBNET | static_cast<uint32_t>(i + 2)));
            }
            ++joined;
        });
    QObject::connect(&client, &SignalClient::roomList, &client,
        [&](QList<RoomListItem> rooms) {
            assert(rooms.size() == (lists == 0 ? 2 : 3));
            for (const RoomListItem& room : rooms) {
                assert(samePolicy(room.tcpPolicy, tcp) && samePolicy(room.udpPolicy, udp));
                assert(room.mtu == ROOM_MTU_SAFE);
            }
            ++lists;
        });
    client.connectToServer(QStringLiteral("127.0.0.1"), server.port());
    QElapsedTimer timer;
    timer.start();
    while ((created != 1 || joined != 1 || lists != 2) && timer.elapsed() < 10000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    assert(created == 1 && joined == 1 && lists == 2);
    client.disconnect();
}

void testGuiPreAuthBusinessRejected(uint8_t type, bool legacy = false) {
    QTcpServer server; assert(server.listen(QHostAddress::LocalHost, 0));
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&]() {
        QTcpSocket* socket = server.nextPendingConnection();
        QObject::connect(socket, &QTcpSocket::readyRead, &server, [socket, type, legacy]() {
            socket->readAll();
            ByteBuffer body;
            if (type == MSG_ERROR) {
                if (!legacy) body.writeU16(static_cast<uint16_t>(AuthErrorCode::InvalidPassword));
                body.writeString("Protocol version mismatch");
            } else if (type == MSG_LOGIN_RESP) {
                body.writeU32(1); body.writeU16(9); body.writeU8(0);
            } else {
                body.writeU32(1); body.writeU32(VNET_SUBNET | 2);
                writePolicy(body, makeDefaultTcpPolicy()); writePolicy(body, makeDefaultUdpPolicy());
                body.writeU8(0); body.writeU16(ROOM_MTU_DEFAULT);
                if (type == MSG_JOIN_RESP) { body.writeU8(1); body.writeU32(1); body.writeU32(VNET_SUBNET | 2); body.writeString("Alice"); }
                uint8_t token[RECONNECT_TOKEN_SIZE] = {1}; body.writeBytes(token, sizeof(token));
            }
            std::vector<uint8_t> bytes(body.data(), body.data() + body.size());
            const std::vector<uint8_t> wire = makeTcpFrame(type, bytes);
            socket->write(reinterpret_cast<const char*>(wire.data()), wire.size());
        });
    });
    SignalClient client; bool fatal = false, error = false, closed = false;
    QObject::connect(&client, &SignalClient::fatalAuthError, &client, [&](uint16_t, QString) { fatal = true; });
    QObject::connect(&client, &SignalClient::serverError, &client, [&](QString) { assert(fatal); error = true; });
    QObject::connect(&client, &SignalClient::disconnected, &client, [&]() { closed = true; });
    QObject::connect(&client, &SignalClient::connected, &client, []() { assert(false && "unauthenticated connected callback"); });
    QObject::connect(&client, &SignalClient::loginResponse, &client, [](uint32_t, bool) { assert(false && "unauthenticated login callback"); });
    QObject::connect(&client, &SignalClient::roomCreated, &client, []() { assert(false && "unauthenticated room callback"); });
    QObject::connect(&client, &SignalClient::joinResponse, &client, []() { assert(false && "unauthenticated join callback"); });
    client.connectToServer(QStringLiteral("127.0.0.1"), server.serverPort());
    QElapsedTimer timer; timer.start();
    while (!closed && timer.elapsed() < 3000) { QCoreApplication::processEvents(); QThread::msleep(1); }
    assert(fatal && error && closed && !client.secureEnabled());
}

void testGuiCancelActiveKdf() {
    QTcpServer server; assert(server.listen(QHostAddress::LocalHost, 0));
    int authFrames = 0; QTcpSocket* peer = nullptr; std::vector<uint8_t> stream;
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&]() {
        peer = server.nextPendingConnection();
        QObject::connect(peer, &QTcpSocket::readyRead, &server, [&]() {
            const QByteArray data = peer->readAll(); stream.insert(stream.end(), data.begin(), data.end());
            uint8_t type; std::vector<uint8_t> payload;
            while (popTcpFrame(&stream, &type, &payload)) {
                if (type == MSG_SERVER_AUTH) { ++authFrames; continue; }
                assert(type == MSG_CLIENT_HELLO);
                uint8_t salt[16] = {3}, nonce[16] = {4}, priv[32] = {5}, pub[32]; crypto_x25519_public_key(pub, priv);
                ByteBuffer reply; reply.writeU16(9); reply.writeU8(1); reply.writeU8(AUTH_KDF_PROFILE);
                reply.writeBytes(salt,16); reply.writeBytes(nonce,16); reply.writeBytes(pub,32);
                std::vector<uint8_t> body(reply.data(), reply.data()+reply.size()); const auto wire = makeTcpFrame(MSG_SERVER_HELLO,body);
                peer->write(reinterpret_cast<const char*>(wire.data()),wire.size());
            }
        });
    });
    SignalClient client; client.setServerPassword(QStringLiteral("cancel-during-background-kdf"));
    client.connectToServer(QStringLiteral("127.0.0.1"),server.serverPort());
    QElapsedTimer timer; timer.start();
    while (!client.authInProgress() && timer.elapsed() < 3000) { QCoreApplication::processEvents(); QThread::msleep(1); }
    assert(client.authInProgress()); client.disconnect();
    while (timer.elapsed() < 500) { QCoreApplication::processEvents(); QThread::msleep(1); }
    assert(!client.authInProgress() && !client.secureEnabled() && authFrames == 0);
}

} // namespace

void runGuiPolicyDecodeTests() {
    testGuiPreAuthBusinessRejected(VLan::MSG_LOGIN_RESP);
    testGuiPreAuthBusinessRejected(VLan::MSG_ROOM_CREATED);
    testGuiPreAuthBusinessRejected(VLan::MSG_JOIN_RESP);
    testGuiPreAuthBusinessRejected(VLan::MSG_ERROR, true);
    testGuiCancelActiveKdf();
    testGuiPolicyCallbacks(VLan::MODE_RELAY_RAW_UDP);
    testGuiPolicyCallbacks(VLan::MODE_RELAY_KCP);
    testGuiPolicyCallbacks(VLan::MODE_RELAY_TCP);
}

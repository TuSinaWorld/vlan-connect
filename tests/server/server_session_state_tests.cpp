#include "../../server/src/room.h"
#include "../../server/src/client_message_validator.h"

#include <cassert>
#include <cstdint>
#include <map>

using namespace VLan;

static void testStatePolicy() {
    ClientSession session;
    assert(session.state == SessionState::AwaitHello);
    assert(isClientSignalMessageAllowed(session.state, MSG_CLIENT_HELLO));
    assert(!isClientSignalMessageAllowed(session.state, MSG_LOGIN));

    session.state = SessionState::AwaitServerAuth;
    assert(isClientSignalMessageAllowed(session.state, MSG_SERVER_AUTH));
    assert(!isClientSignalMessageAllowed(session.state, MSG_CLIENT_HELLO));

    session.state = SessionState::AwaitLogin;
    assert(isClientSignalMessageAllowed(session.state, MSG_LOGIN));
    assert(isClientSignalMessageAllowed(session.state, MSG_PING));
    assert(!isClientSignalMessageAllowed(session.state, MSG_SERVER_AUTH));

    session.state = SessionState::LoggedIn;
    assert(sessionHasPeerIdentity(session.state));
    assert(sessionCanBindDataChannel(session.state));
    assert(isClientSignalMessageAllowed(session.state, MSG_CREATE_ROOM));
    assert(!isClientSignalMessageAllowed(session.state, MSG_LOGIN));

    session.state = SessionState::AwaitRoomPassword;
    assert(isClientSignalMessageAllowed(session.state, MSG_AUTH_RESPONSE));
    assert(!isClientSignalMessageAllowed(session.state, MSG_JOIN_ROOM));

    session.state = SessionState::ResumePending;
    assert(isClientSignalMessageAllowed(session.state, MSG_RESUME_ROOM));
    assert(!isClientSignalMessageAllowed(session.state, MSG_CREATE_ROOM));

    session.state = SessionState::InRoom;
    assert(isClientSignalMessageAllowed(session.state, MSG_LEAVE_ROOM));
    assert(isClientSignalMessageAllowed(session.state, MSG_TCP_RELAY_DATA));
    assert(!isClientSignalMessageAllowed(session.state, MSG_LOGIN));

    session.state = SessionState::Closing;
    assert(!sessionHasPeerIdentity(session.state));
    assert(!sessionCanBindDataChannel(session.state));
    assert(!isClientSignalMessageAllowed(session.state, MSG_PING));
    assert(!isClientSignalMessageAllowed(session.state, MSG_LOGOUT));
}

static void testHandshakeClassification() {
    assert(isClientHandshakeMessage(MSG_CLIENT_HELLO));
    assert(isClientHandshakeMessage(MSG_SERVER_AUTH));
    assert(isClientHandshakeMessage(MSG_LOGIN));
    assert(!isClientHandshakeMessage(MSG_CREATE_ROOM));
    assert(isKnownClientSignalMessage(MSG_LOGOUT));
    assert(!isKnownClientSignalMessage(0x7f));

    assert(classifyClientSignalMessage(
               SessionState::AwaitHello, MSG_CLIENT_HELLO) ==
           SessionMessageAction::Dispatch);
    assert(classifyClientSignalMessage(
               SessionState::AwaitHello, MSG_LOGIN) ==
           SessionMessageAction::Close);
    assert(classifyClientSignalMessage(
               SessionState::LoggedIn, MSG_LOGIN) ==
           SessionMessageAction::Close);
    assert(classifyClientSignalMessage(
               SessionState::AwaitHello, MSG_LIST_ROOMS) ==
           SessionMessageAction::SendStateError);
    assert(classifyClientSignalMessage(
               SessionState::AwaitHello, 0x7f) ==
           SessionMessageAction::IgnoreUnknown);
}

static void testConditionalIndexBinding() {
    std::map<uint32_t, int> peerIndex;
    assert(bindUniqueSessionIndex(peerIndex, uint32_t(17), 101));
    assert(bindUniqueSessionIndex(peerIndex, uint32_t(17), 101));
    assert(!bindUniqueSessionIndex(peerIndex, uint32_t(17), 202));

    unbindSessionIndex(peerIndex, uint32_t(17), 202);
    assert(peerIndex.size() == 1);
    assert(peerIndex[17] == 101);

    unbindSessionIndex(peerIndex, uint32_t(17), 101);
    assert(peerIndex.empty());

    std::map<int, int> dataFdIndex;
    assert(bindUniqueSessionIndex(dataFdIndex, 55, 101));
    unbindSessionIndex(dataFdIndex, 55, 101);
    assert(bindUniqueSessionIndex(dataFdIndex, 55, 202));
    unbindSessionIndex(dataFdIndex, 55, 101);
    assert(dataFdIndex[55] == 202);

    std::map<uint32_t, int> secureIndex;
    assert(bindUniqueSessionIndex(
        secureIndex, uint32_t(0x12345678), 101));
    assert(!bindUniqueSessionIndex(
        secureIndex, uint32_t(0x12345678), 202));
    unbindSessionIndex(
        secureIndex, uint32_t(0x12345678), 101);
    assert(bindUniqueSessionIndex(
        secureIndex, uint32_t(0x12345678), 202));
    unbindSessionIndex(
        secureIndex, uint32_t(0x12345678), 101);
    assert(secureIndex[0x12345678] == 202);
}

static void testLeaseLifecycle() {
    Room room;
    room.id = 9;
    room.maxPlayers = 2;
    uint8_t token[RECONNECT_TOKEN_SIZE] = {};
    token[0] = 1;
    RoomLease* lease = room.addLease(7, "Alice", token);
    assert(lease != nullptr);
    const uint32_t virtualIP = lease->virtualIP;
    assert(lease->online);

    const time_t now = time(nullptr);
    assert(room.markLeaseOffline(7, now));
    lease = room.leaseByPeerId(7);
    assert(lease != nullptr);
    assert(!lease->online);
    assert(lease->virtualIP == virtualIP);
    assert(lease->expiresAt ==
           now + RECONNECT_LEASE_TIMEOUT_SEC);

    assert(room.markLeaseOnline(7));
    lease = room.leaseByPeerId(7);
    assert(lease != nullptr && lease->online);
    assert(lease->virtualIP == virtualIP);

    assert(room.removeLease(7));
    assert(room.leaseByPeerId(7) == nullptr);
}

static ByteBuffer createRoomPayload(uint8_t tcpMode, uint8_t tcpFec,
                                    uint16_t mtu) {
    ByteBuffer body;
    body.writeString("Room");
    body.writeU8(4);
    body.writeU8(tcpMode);
    body.writeU8(tcpFec);
    body.writeU8(KCP_PROFILE_REALTIME);
    body.writeU8(MODE_RELAY_KCP);
    body.writeU8(FEC_NONE);
    body.writeU8(KCP_PROFILE_REALTIME);
    body.writeU8(0);
    body.writeU16(mtu);
    return body;
}

static void testStrictClientPayloads() {
    ByteBuffer valid = createRoomPayload(
        MODE_RELAY_KCP, FEC_NONE, ROOM_MTU_DEFAULT);
    assert(validateClientSignalPayload(
        MSG_CREATE_ROOM, valid.data(), valid.size()));

    ByteBuffer badMtu = createRoomPayload(
        MODE_RELAY_KCP, FEC_NONE, 1301);
    assert(!validateClientSignalPayload(
        MSG_CREATE_ROOM, badMtu.data(), badMtu.size()));

    ByteBuffer tcpWithFec = createRoomPayload(
        MODE_RELAY_TCP, FEC_10, ROOM_MTU_DEFAULT);
    assert(!validateClientSignalPayload(
        MSG_CREATE_ROOM, tcpWithFec.data(), tcpWithFec.size()));

    ByteBuffer relay;
    relay.writeU32(10);
    relay.writeU32(20);
    relay.writeU8(9);
    assert(!validateClientSignalPayload(
        MSG_TCP_RELAY_DATA, relay.data(), relay.size()));

    ByteBuffer logoutWithTail;
    logoutWithTail.writeU8(0);
    assert(!validateClientSignalPayload(
        MSG_LOGOUT, logoutWithTail.data(), logoutWithTail.size()));
}

int main() {
    testStatePolicy();
    testHandshakeClassification();
    testConditionalIndexBinding();
    testLeaseLifecycle();
    testStrictClientPayloads();
    return 0;
}

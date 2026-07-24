#ifndef VLAN_CLIENT_MESSAGE_VALIDATOR_H
#define VLAN_CLIENT_MESSAGE_VALIDATOR_H

#include "protocol.h"
#include "byte_buffer.h"
#include "secure_frame.h"
#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>

namespace VLan {

inline bool hasNonzeroReconnectToken(
    const uint8_t token[RECONNECT_TOKEN_SIZE])
{
    uint8_t value = 0;
    for (int i = 0; i < RECONNECT_TOKEN_SIZE; ++i)
        value |= token[i];
    return value != 0;
}

inline bool validateClientSignalPayload(
    uint8_t msgType, const uint8_t* payload, size_t len)
{
    try {
        switch (msgType) {
        case MSG_CLIENT_HELLO:
            return len == 2 + 16 + 32;
        case MSG_SERVER_AUTH:
            return len == 32;
        case MSG_ENCRYPTED:
            return payload != nullptr &&
                   len >= SECURE_FRAME_OVERHEAD + 1;
        case MSG_LOGIN: {
            if (!payload || len == 0) return false;
            ByteBuffer bb(payload, len);
            const std::string name = bb.readString();
            if (!isValidPlayerName(name)) return false;
            bb.readU16();
            const uint8_t hasResume = bb.readU8();
            if (hasResume > 1) return false;
            if (hasResume) {
                const uint32_t roomId = bb.readU32();
                const uint32_t peerId = bb.readU32();
                uint8_t token[RECONNECT_TOKEN_SIZE];
                bb.readBytes(token, sizeof(token));
                if (roomId == 0 || peerId == 0 ||
                    !hasNonzeroReconnectToken(token)) {
                    return false;
                }
            }
            return bb.atEnd();
        }
        case MSG_CREATE_ROOM: {
            if (!payload || len == 0) return false;
            ByteBuffer bb(payload, len);
            const std::string roomName = bb.readString();
            const uint8_t maxPlayers = bb.readU8();
            const uint8_t tcpMode = bb.readU8();
            const uint8_t tcpFec = bb.readU8();
            const uint8_t tcpProfile = bb.readU8();
            const uint8_t udpMode = bb.readU8();
            const uint8_t udpFec = bb.readU8();
            const uint8_t udpProfile = bb.readU8();
            if (!isValidRoomName(roomName) ||
                maxPlayers < 2 || maxPlayers > MAX_PLAYERS ||
                !isValidTransportModeValue(tcpMode) ||
                !isValidTransportModeValue(udpMode) ||
                !isValidFecModeValue(tcpFec) ||
                !isValidFecModeValue(udpFec) ||
                !isValidKcpProfileValue(tcpProfile) ||
                !isValidKcpProfileValue(udpProfile) ||
                (tcpMode == MODE_RELAY_TCP && tcpFec != FEC_NONE) ||
                (udpMode == MODE_RELAY_TCP && udpFec != FEC_NONE)) {
                return false;
            }
            const uint8_t passwordProtected = bb.readU8();
            if (passwordProtected > 1) return false;
            if (passwordProtected) {
                uint8_t passwordHash[32];
                bb.readBytes(passwordHash, sizeof(passwordHash));
            }
            const uint16_t mtu = bb.readU16();
            return isValidRoomMtuValue(mtu) && bb.atEnd();
        }
        case MSG_JOIN_ROOM:
        case MSG_REQUEST_RELAY: {
            if (!payload || len != 4) return false;
            ByteBuffer bb(payload, len);
            return bb.readU32() != 0 && bb.atEnd();
        }
        case MSG_RESUME_ROOM: {
            if (!payload || len != 4 + 4 + RECONNECT_TOKEN_SIZE)
                return false;
            ByteBuffer bb(payload, len);
            const uint32_t roomId = bb.readU32();
            const uint32_t peerId = bb.readU32();
            uint8_t token[RECONNECT_TOKEN_SIZE];
            bb.readBytes(token, sizeof(token));
            return roomId != 0 && peerId != 0 &&
                   hasNonzeroReconnectToken(token) && bb.atEnd();
        }
        case MSG_AUTH_RESPONSE:
            return len == 32;
        case MSG_LEAVE_ROOM:
        case MSG_LOGOUT:
        case MSG_LIST_ROOMS:
        case MSG_PING:
            return len == 0;
        case MSG_TCP_RELAY_DATA: {
            if (!payload || len < 9) return false;
            ByteBuffer bb(payload, len);
            const uint32_t sourcePeerId = bb.readU32();
            const uint32_t destinationPeerId = bb.readU32();
            const uint8_t trafficClass = bb.readU8();
            return sourcePeerId != 0 && destinationPeerId != 0 &&
                   (trafficClass == TRAFFIC_TCP ||
                    trafficClass == TRAFFIC_UDP);
        }
        default:
            return true;
        }
    } catch (const std::exception&) {
        return false;
    }
}

} // namespace VLan

#endif // VLAN_CLIENT_MESSAGE_VALIDATOR_H

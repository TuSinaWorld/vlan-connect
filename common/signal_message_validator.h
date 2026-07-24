#ifndef VLAN_SIGNAL_MESSAGE_VALIDATOR_H
#define VLAN_SIGNAL_MESSAGE_VALIDATOR_H

#include "protocol.h"
#include <cstddef>
#include <cstdint>

namespace VLan {

enum class MessageValidationStatus {
    Valid,
    UnknownType,
    Malformed
};

enum class MessageValidationError {
    None,
    Truncated,
    TrailingData,
    InvalidBoolean,
    InvalidCount,
    InvalidStringLength,
    InvalidPolicy,
    InvalidMtu,
    InvalidVersion,
    InvalidTrafficClass,
    InvalidValue,
    EncryptedFrameTooShort
};

struct MessageValidationResult {
    MessageValidationStatus status;
    MessageValidationError error;
    size_t offset;

    MessageValidationResult(MessageValidationStatus s = MessageValidationStatus::Valid,
                            MessageValidationError e = MessageValidationError::None,
                            size_t o = 0)
        : status(s), error(e), offset(o) {}

    bool isValid() const { return status == MessageValidationStatus::Valid; }
};

namespace MessageValidatorDetail {

class Cursor {
public:
    Cursor(const uint8_t* data, size_t len)
        : m_data(data), m_len(len), m_pos(0),
          m_error(MessageValidationError::None), m_errorOffset(0) {}

    size_t position() const { return m_pos; }
    size_t remaining() const { return m_pos <= m_len ? m_len - m_pos : 0; }
    MessageValidationError error() const { return m_error; }
    size_t errorOffset() const { return m_errorOffset; }

    bool readU8(uint8_t* out) {
        if (!require(1)) return false;
        if (out) *out = m_data[m_pos];
        ++m_pos;
        return true;
    }

    bool readU16(uint16_t* out) {
        if (!require(2)) return false;
        if (out) {
            *out = static_cast<uint16_t>(
                (static_cast<uint16_t>(m_data[m_pos]) << 8) |
                static_cast<uint16_t>(m_data[m_pos + 1]));
        }
        m_pos += 2;
        return true;
    }

    bool readU32(uint32_t* out) {
        if (!require(4)) return false;
        if (out) {
            *out = (static_cast<uint32_t>(m_data[m_pos]) << 24) |
                   (static_cast<uint32_t>(m_data[m_pos + 1]) << 16) |
                   (static_cast<uint32_t>(m_data[m_pos + 2]) << 8) |
                    static_cast<uint32_t>(m_data[m_pos + 3]);
        }
        m_pos += 4;
        return true;
    }

    bool skip(size_t count) {
        if (!require(count)) return false;
        m_pos += count;
        return true;
    }

    bool readString(size_t minLen, size_t maxLen, bool alphaNumeric) {
        const size_t lengthOffset = m_pos;
        uint16_t length = 0;
        if (!readU16(&length)) return false;
        if (length < minLen || length > maxLen) {
            setError(MessageValidationError::InvalidStringLength, lengthOffset);
            return false;
        }
        if (!require(length)) return false;
        if (alphaNumeric) {
            for (size_t i = 0; i < length; ++i) {
                if (!isAsciiAlphaNum(static_cast<char>(m_data[m_pos + i]))) {
                    setError(MessageValidationError::InvalidValue, m_pos + i);
                    return false;
                }
            }
        }
        m_pos += length;
        return true;
    }

    bool readBoolean() {
        const size_t fieldOffset = m_pos;
        uint8_t value = 0;
        if (!readU8(&value)) return false;
        if (value > 1) {
            setError(MessageValidationError::InvalidBoolean, fieldOffset);
            return false;
        }
        return true;
    }

    bool readPolicy() {
        const size_t fieldOffset = m_pos;
        uint8_t transport = 0;
        uint8_t fec = 0;
        uint8_t profile = 0;
        if (!readU8(&transport) || !readU8(&fec) || !readU8(&profile))
            return false;
        if (!isValidTransportModeValue(transport) ||
            !isValidFecModeValue(fec) ||
            !isValidKcpProfileValue(profile) ||
            (transport == MODE_RELAY_TCP && fec != FEC_NONE)) {
            setError(MessageValidationError::InvalidPolicy, fieldOffset);
            return false;
        }
        return true;
    }

    bool readMtu() {
        const size_t fieldOffset = m_pos;
        uint16_t mtu = 0;
        if (!readU16(&mtu)) return false;
        if (!isValidRoomMtuValue(mtu)) {
            setError(MessageValidationError::InvalidMtu, fieldOffset);
            return false;
        }
        return true;
    }

    bool finish() {
        if (m_error != MessageValidationError::None) return false;
        if (m_pos != m_len) {
            setError(MessageValidationError::TrailingData, m_pos);
            return false;
        }
        return true;
    }

    void setError(MessageValidationError error, size_t offset) {
        if (m_error == MessageValidationError::None) {
            m_error = error;
            m_errorOffset = offset;
        }
    }

private:
    bool require(size_t count) {
        if (count > remaining()) {
            setError(MessageValidationError::Truncated, m_pos);
            return false;
        }
        return true;
    }

    const uint8_t* m_data;
    size_t m_len;
    size_t m_pos;
    MessageValidationError m_error;
    size_t m_errorOffset;
};

inline MessageValidationResult malformed(const Cursor& cursor) {
    return MessageValidationResult(MessageValidationStatus::Malformed,
                                   cursor.error(), cursor.errorOffset());
}

inline MessageValidationResult finish(Cursor& cursor) {
    if (!cursor.finish()) return malformed(cursor);
    return MessageValidationResult();
}

inline MessageValidationResult fixedSize(size_t actual, size_t expected) {
    if (actual < expected) {
        return MessageValidationResult(MessageValidationStatus::Malformed,
                                       MessageValidationError::Truncated,
                                       actual);
    }
    if (actual > expected) {
        return MessageValidationResult(MessageValidationStatus::Malformed,
                                       MessageValidationError::TrailingData,
                                       expected);
    }
    return MessageValidationResult();
}

inline MessageValidationResult validateEncryptedEnvelope(size_t len) {
    // counter (8) + at least the encrypted inner type (1) + MAC (16)
    const size_t minimum = 8 + 1 + 16;
    if (len < minimum) {
        return MessageValidationResult(MessageValidationStatus::Malformed,
                                       MessageValidationError::EncryptedFrameTooShort,
                                       len);
    }
    return MessageValidationResult();
}

inline MessageValidationResult validatePeerInfo(const uint8_t* data, size_t len) {
    Cursor cursor(data, len);
    uint32_t peerId = 0;
    uint32_t virtualIP = 0;
    if (!cursor.readU32(&peerId) || !cursor.readU32(&virtualIP) ||
        !cursor.readString(MIN_PLAYER_NAME_LEN, MAX_PLAYER_NAME_LEN, true))
        return malformed(cursor);
    if (peerId == 0 || virtualIP == 0) {
        cursor.setError(MessageValidationError::InvalidValue, 0);
        return malformed(cursor);
    }
    return finish(cursor);
}

inline MessageValidationResult validateRelayData(const uint8_t* data, size_t len) {
    Cursor cursor(data, len);
    uint32_t srcPeerId = 0;
    uint32_t dstPeerId = 0;
    uint8_t trafficClass = 0;
    if (!cursor.readU32(&srcPeerId) || !cursor.readU32(&dstPeerId) ||
        !cursor.readU8(&trafficClass))
        return malformed(cursor);
    if (srcPeerId == 0 || dstPeerId == 0) {
        cursor.setError(MessageValidationError::InvalidValue, 0);
        return malformed(cursor);
    }
    if (trafficClass != TRAFFIC_TCP && trafficClass != TRAFFIC_UDP) {
        cursor.setError(MessageValidationError::InvalidTrafficClass, 8);
        return malformed(cursor);
    }
    if (!cursor.skip(cursor.remaining())) return malformed(cursor);
    return finish(cursor);
}

} // namespace MessageValidatorDetail

inline const char* messageValidationErrorName(MessageValidationError error) {
    switch (error) {
    case MessageValidationError::None:                   return "none";
    case MessageValidationError::Truncated:              return "truncated";
    case MessageValidationError::TrailingData:           return "trailing-data";
    case MessageValidationError::InvalidBoolean:         return "invalid-boolean";
    case MessageValidationError::InvalidCount:           return "invalid-count";
    case MessageValidationError::InvalidStringLength:    return "invalid-string-length";
    case MessageValidationError::InvalidPolicy:          return "invalid-policy";
    case MessageValidationError::InvalidMtu:             return "invalid-mtu";
    case MessageValidationError::InvalidVersion:         return "invalid-version";
    case MessageValidationError::InvalidTrafficClass:    return "invalid-traffic-class";
    case MessageValidationError::InvalidValue:           return "invalid-value";
    case MessageValidationError::EncryptedFrameTooShort: return "encrypted-frame-too-short";
    }
    return "unknown";
}

inline MessageValidationResult validateServerSignalPayload(
    uint8_t msgType, const uint8_t* data, size_t len)
{
    using namespace MessageValidatorDetail;

    if (len > 0 && data == nullptr) {
        return MessageValidationResult(MessageValidationStatus::Malformed,
                                       MessageValidationError::Truncated, 0);
    }

    switch (msgType) {
    case MSG_SERVER_HELLO: {
        Cursor cursor(data, len);
        uint16_t version = 0;
        uint8_t authRequired = 0;
        if (!cursor.readU16(&version) || !cursor.readU8(&authRequired))
            return malformed(cursor);
        if (version != PROTOCOL_VERSION) {
            cursor.setError(MessageValidationError::InvalidVersion, 0);
            return malformed(cursor);
        }
        if (authRequired > 1) {
            cursor.setError(MessageValidationError::InvalidBoolean, 2);
            return malformed(cursor);
        }
        if (authRequired != 0 && !cursor.skip(16 + 32))
            return malformed(cursor);
        return finish(cursor);
    }
    case MSG_SERVER_AUTH_OK: {
        Cursor cursor(data, len);
        uint32_t sessionId = 0;
        if (!cursor.readU32(&sessionId) || !cursor.skip(32))
            return malformed(cursor);
        if (sessionId == 0) {
            cursor.setError(MessageValidationError::InvalidValue, 0);
            return malformed(cursor);
        }
        return finish(cursor);
    }

    case MSG_ENCRYPTED:
        return validateEncryptedEnvelope(len);

    case MSG_LOGIN_RESP: {
        Cursor cursor(data, len);
        uint32_t peerId = 0;
        uint16_t version = 0;
        if (!cursor.readU32(&peerId) || !cursor.readU16(&version))
            return malformed(cursor);
        if (peerId == 0) {
            cursor.setError(MessageValidationError::InvalidValue, 0);
            return malformed(cursor);
        }
        if (version != PROTOCOL_VERSION) {
            cursor.setError(MessageValidationError::InvalidVersion, 4);
            return malformed(cursor);
        }
        if (!cursor.readBoolean()) return malformed(cursor);
        return finish(cursor);
    }

    case MSG_ROOM_CREATED: {
        Cursor cursor(data, len);
        uint32_t roomId = 0;
        uint32_t virtualIP = 0;
        if (!cursor.readU32(&roomId) || !cursor.readU32(&virtualIP) ||
            !cursor.readPolicy() || !cursor.readPolicy() ||
            !cursor.readBoolean() || !cursor.readMtu() ||
            !cursor.skip(RECONNECT_TOKEN_SIZE))
            return malformed(cursor);
        if (roomId == 0 || virtualIP == 0) {
            cursor.setError(MessageValidationError::InvalidValue, 0);
            return malformed(cursor);
        }
        return finish(cursor);
    }

    case MSG_JOIN_RESP: {
        Cursor cursor(data, len);
        uint32_t roomId = 0;
        uint32_t virtualIP = 0;
        uint8_t count = 0;
        if (!cursor.readU32(&roomId) || !cursor.readU32(&virtualIP) ||
            !cursor.readPolicy() || !cursor.readPolicy() ||
            !cursor.readBoolean() || !cursor.readMtu() ||
            !cursor.readU8(&count))
            return malformed(cursor);
        if (roomId == 0 || virtualIP == 0) {
            cursor.setError(MessageValidationError::InvalidValue, 0);
            return malformed(cursor);
        }
        if (count == 0 || count > MAX_PLAYERS) {
            cursor.setError(MessageValidationError::InvalidCount,
                            cursor.position() - 1);
            return malformed(cursor);
        }
        for (uint8_t i = 0; i < count; ++i) {
            uint32_t peerId = 0;
            uint32_t memberIP = 0;
            const size_t memberOffset = cursor.position();
            if (!cursor.readU32(&peerId) || !cursor.readU32(&memberIP) ||
                !cursor.readString(MIN_PLAYER_NAME_LEN,
                                   MAX_PLAYER_NAME_LEN, true))
                return malformed(cursor);
            if (peerId == 0 || memberIP == 0) {
                cursor.setError(MessageValidationError::InvalidValue,
                                memberOffset);
                return malformed(cursor);
            }
        }
        if (!cursor.skip(RECONNECT_TOKEN_SIZE)) return malformed(cursor);
        return finish(cursor);
    }

    case MSG_PEER_JOINED:
    case MSG_PEER_RESUMED:
        return validatePeerInfo(data, len);

    case MSG_PEER_LEFT:
    case MSG_RELAY_READY: {
        Cursor cursor(data, len);
        uint32_t peerId = 0;
        if (!cursor.readU32(&peerId)) return malformed(cursor);
        if (peerId == 0) {
            cursor.setError(MessageValidationError::InvalidValue, 0);
            return malformed(cursor);
        }
        return finish(cursor);
    }

    case MSG_LOGOUT_ACK:
    case MSG_PONG:
        return fixedSize(len, 0);

    case MSG_ROOM_LIST:
    case MSG_ROOM_LIST_PUSH: {
        Cursor cursor(data, len);
        uint16_t count = 0;
        if (!cursor.readU16(&count)) return malformed(cursor);
        static const size_t MIN_ROOM_RECORD_SIZE =
            4 + 2 + 1 + 2 + 3 + 3 + 1 + 2;
        if (count > cursor.remaining() / MIN_ROOM_RECORD_SIZE) {
            cursor.setError(MessageValidationError::InvalidCount, 0);
            return malformed(cursor);
        }
        for (uint16_t i = 0; i < count; ++i) {
            uint32_t roomId = 0;
            uint8_t playerCount = 0;
            uint8_t maxPlayers = 0;
            const size_t roomOffset = cursor.position();
            if (!cursor.readU32(&roomId) ||
                !cursor.readString(1, MAX_ROOM_NAME_LEN, false) ||
                !cursor.readU8(&playerCount) ||
                !cursor.readU8(&maxPlayers) ||
                !cursor.readPolicy() || !cursor.readPolicy() ||
                !cursor.readBoolean() || !cursor.readMtu())
                return malformed(cursor);
            if (roomId == 0 || maxPlayers < 2 ||
                maxPlayers > MAX_PLAYERS || playerCount > maxPlayers) {
                cursor.setError(MessageValidationError::InvalidValue,
                                roomOffset);
                return malformed(cursor);
            }
        }
        return finish(cursor);
    }

    case MSG_AUTH_CHALLENGE:
        return fixedSize(len, 32);

    case MSG_ERROR: {
        Cursor cursor(data, len);
        if (!cursor.readString(1, MAX_TCP_MSG_PAYLOAD - 2, false))
            return malformed(cursor);
        return finish(cursor);
    }

    case MSG_TCP_RELAY_DATA:
        return validateRelayData(data, len);

    default:
        return MessageValidationResult(MessageValidationStatus::UnknownType,
                                       MessageValidationError::None, 0);
    }
}

inline MessageValidationResult validateServerDataPayload(
    uint8_t msgType, const uint8_t* data, size_t len)
{
    using namespace MessageValidatorDetail;

    if (len > 0 && data == nullptr) {
        return MessageValidationResult(MessageValidationStatus::Malformed,
                                       MessageValidationError::Truncated, 0);
    }

    switch (msgType) {
    case MSG_ENCRYPTED:
        return validateEncryptedEnvelope(len);
    case MSG_DATA_CHANNEL_ACK:
    case MSG_PONG:
        return fixedSize(len, 0);
    case MSG_TCP_RELAY_DATA:
        return validateRelayData(data, len);
    default:
        return MessageValidationResult(MessageValidationStatus::UnknownType,
                                       MessageValidationError::None, 0);
    }
}

} // namespace VLan

#endif // VLAN_SIGNAL_MESSAGE_VALIDATOR_H

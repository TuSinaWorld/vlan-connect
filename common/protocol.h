#ifndef VLAN_PROTOCOL_H
#define VLAN_PROTOCOL_H

#include <cstdint>
#include <string>
#ifdef QT_CORE_LIB
#include <QString>
#include <QStringList>
#endif

namespace VLan {

static const uint16_t DEFAULT_PORT            = 11510;  // TCP signal + UDP relay

static const uint32_t VNET_SUBNET    = 0x0A0A0000; // 10.10.0.0
static const uint32_t VNET_MASK      = 0xFFFFFF00; // 255.255.255.0
static const uint32_t VNET_BROADCAST = 0x0A0A00FF; // 10.10.0.255
static const uint32_t VNET_GATEWAY   = 0x0A0A0001; // 10.10.0.1

static const int KEEPALIVE_INTERVAL_MS      = 15000;
static const int RECONNECT_INTERVAL_MS      = 3000;
static const int HEARTBEAT_TIMEOUT_MS       = 45000;
static const int KCP_KEEPALIVE_INTERVAL_MS  = 5000;
static const int KCP_DEAD_TIMEOUT_MS        = 30000;
static const int UDP_KEEPALIVE_INTERVAL_MS  = 10000;

static const int RAW_UDP_KEEPALIVE_MS       = 5000;
static const int RAW_UDP_DEAD_TIMEOUT_MS    = 30000;
static const int RAW_UDP_FRAG_TIMEOUT_MS    = 2000;
static const int RAW_UDP_MAX_FRAG_PAYLOAD   = 1457;
static const size_t RAW_UDP_MAX_REASSEMBLY_BYTES = 128 * 1024;
static const size_t RAW_UDP_MAX_ACTIVE_MESSAGES  = 64;
static const size_t FEC_MAX_BUFFERED_BYTES       = 256 * 1024;
static const size_t FEC_MAX_ACTIVE_GROUPS        = 32;
static const int ROOM_MTU_SAFE              = 1280;
static const int ROOM_MTU_BALANCED          = 1400;
static const int ROOM_MTU_AGGRESSIVE        = 1420;
static const int ROOM_MTU_DEFAULT           = ROOM_MTU_BALANCED;
static const int RAW_UDP_TUN_MTU            = ROOM_MTU_DEFAULT;
static const int MAX_ROOM_NAME_LEN      = 63;
static const int MIN_PLAYER_NAME_LEN    = 5;
static const int MAX_PLAYER_NAME_LEN    = 63;
static const int MIN_ROOM_PASSWORD_LEN  = 6;
static const int MAX_ROOM_PASSWORD_LEN  = 63;
static const int MAX_PLAYERS            = 8;
static const int RECONNECT_TOKEN_SIZE   = 32;
static const int RECONNECT_LEASE_TIMEOUT_SEC = 120;
static const uint8_t VNET_FIRST_HOST_SUFFIX = 2;
static const uint8_t VNET_LAST_HOST_SUFFIX  = 254;

static const size_t MAX_TCP_SEND_BUF    = 2 * 1024 * 1024;   // 2 MB per client
static const size_t MAX_TCP_MSG_PAYLOAD = 65000;              // sanity cap
static const size_t ROOM_LIST_PAGE_PAYLOAD = 60000;
static const int TCP_RELAY_KEEPALIVE_MS = 10000;              // peer-level probe
static const int TCP_RELAY_DEAD_MS      = 30000;              // peer-level timeout
static const int TCP_RECV_TIMEOUT_MS    = 45000;              // client-side no-recv

static const uint32_t PROTOCOL_MAGIC    = 0x564C414E; // "VLAN"
static const uint16_t PROTOCOL_VERSION  = 8;

enum UdpPacketType : uint8_t {
    UDP_KCP_DATA      = 0x01,
    UDP_RELAY_DATA    = 0x02,
    UDP_KEEPALIVE         = 0x07,
    UDP_RAW_RELAY_DATA    = 0x08,
    UDP_ENCRYPTED         = 0x09
};

enum TcpMsgType : uint8_t {
    MSG_LOGIN          = 0x01,
    MSG_LOGIN_RESP     = 0x02,
    MSG_CREATE_ROOM    = 0x03,
    MSG_JOIN_ROOM      = 0x04,
    MSG_ROOM_INFO      = 0x05,
    MSG_PEER_JOINED    = 0x06,
    MSG_PEER_LEFT      = 0x07,
    MSG_LEAVE_ROOM     = 0x08,
    MSG_ROOM_CREATED   = 0x09,
    MSG_JOIN_RESP      = 0x0A,
    MSG_REQUEST_RELAY  = 0x0E,
    MSG_RELAY_READY    = 0x0F,
    MSG_LIST_ROOMS     = 0x10,
    MSG_ROOM_LIST      = 0x11,
    MSG_ROOM_LIST_PUSH = 0x12,
    MSG_RESUME_ROOM    = 0x13,
    MSG_LOGOUT         = 0x14,
    MSG_LOGOUT_ACK     = 0x15,
    MSG_PEER_RESUMED   = 0x16,
    MSG_TCP_RELAY_DATA      = 0x20,
    MSG_DATA_CHANNEL_INIT   = 0x21,
    MSG_DATA_CHANNEL_ACK    = 0x22,
    MSG_CLIENT_HELLO    = 0x30,
    MSG_SERVER_HELLO    = 0x31,
    MSG_SERVER_AUTH     = 0x32,
    MSG_SERVER_AUTH_OK  = 0x33,
    MSG_ENCRYPTED       = 0x34,
    MSG_AUTH_CHALLENGE  = 0xE0,
    MSG_AUTH_RESPONSE   = 0xE1,
    MSG_PING           = 0xF0,
    MSG_PONG           = 0xF1,
    MSG_ERROR          = 0xF2
};

enum TransportType : uint8_t {
    TRANSPORT_NONE          = 0,
    TRANSPORT_RELAY_KCP     = 2,
    TRANSPORT_RELAY_TCP     = 3,
    TRANSPORT_RELAY_RAW_UDP = 4
};

enum TrafficClass : uint8_t {
    TRAFFIC_TCP = 1,
    TRAFFIC_UDP = 2
};

enum RoomListOperation : uint8_t {
    ROOM_LIST_UPSERT = 1,
    ROOM_LIST_REMOVE = 2
};

enum class DataPlaneState : uint8_t {
    Stopped = 0,
    Running = 1
};

enum class DataPlaneSecurityMode : uint8_t {
    Unconfigured = 0,
    Secure       = 1
};

inline bool dataPlaneAllowsTraffic(DataPlaneState state,
                                   DataPlaneSecurityMode mode) {
    return state == DataPlaneState::Running &&
           mode == DataPlaneSecurityMode::Secure;
}

inline bool dataPlaneCanReconfigure(DataPlaneState state) {
    return state == DataPlaneState::Stopped;
}

enum KcpProfile : uint8_t {
    KCP_PROFILE_REALTIME = 0,
    KCP_PROFILE_BULK     = 1
};

#pragma pack(push, 1)

struct UdpHeader {
    uint8_t type;
};

struct UdpRelayHeader {
    uint8_t  type;       // UDP_RELAY_DATA
    uint8_t  trafficClass;
    uint32_t srcPeerId;
    uint32_t dstPeerId;
};

struct TcpMsgHeader {
    uint8_t  msgType;
    uint16_t length;     // payload length (big-endian)
};

struct FragHeader {
    uint16_t msgId;
    uint8_t  fragIndex;
    uint8_t  fragTotal;
    uint16_t totalLen;
};

struct FecHeader {
    uint8_t  groupId;    // wraps 0-255
    uint8_t  index;      // 0..N-1 = data, N..N+M-1 = parity
    uint8_t  dataCount;  // N (number of data packets in group)
    uint8_t  totalCount; // N+M (data + parity)
};

#pragma pack(pop)

enum TransportMode : uint8_t {
    MODE_RELAY_KCP      = 2,
    MODE_RELAY_TCP      = 3,
    MODE_RELAY_RAW_UDP  = 4
};

enum FecMode : uint8_t {
    FEC_NONE   = 0,
    FEC_10     = 1,   // ~10% redundancy  (10 data + 1 parity)
    FEC_30     = 2,   // ~30% redundancy  (10 data + 3 parity)
    FEC_50     = 3,   // ~50% redundancy  (10 data + 5 parity)
    FEC_70     = 4,   // ~70% redundancy  (10 data + 7 parity)
    FEC_100    = 5,   // ~100% redundancy (10 data + 10 parity)
    FEC_200    = 6    // ~200% redundancy (10 data + 20 parity)
};

static const int FEC_GROUP_SIZE       = 10;
static const int FEC_FLUSH_INTERVAL_MS = 10;

inline bool isAsciiAlphaNum(char c) {
    return (c >= '0' && c <= '9') ||
           (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z');
}

inline bool isAsciiAlphaNumString(const std::string& s) {
    for (size_t i = 0; i < s.size(); ++i) {
        if (!isAsciiAlphaNum(s[i])) return false;
    }
    return true;
}

inline bool isValidPlayerName(const std::string& s) {
    return s.size() >= MIN_PLAYER_NAME_LEN &&
           s.size() <= MAX_PLAYER_NAME_LEN &&
           isAsciiAlphaNumString(s);
}

inline bool isValidRoomName(const std::string& s) {
    return !s.empty() && s.size() <= MAX_ROOM_NAME_LEN;
}

inline bool isValidRoomPassword(const std::string& s) {
    return s.size() >= MIN_ROOM_PASSWORD_LEN &&
           s.size() <= MAX_ROOM_PASSWORD_LEN &&
           isAsciiAlphaNumString(s);
}

inline uint8_t normalizeMaxPlayers(int value) {
    if (value < 2) return 2;
    if (value > MAX_PLAYERS) return MAX_PLAYERS;
    return static_cast<uint8_t>(value);
}

inline bool isValidTransportModeValue(uint8_t raw) {
    return raw == MODE_RELAY_KCP ||
           raw == MODE_RELAY_TCP ||
           raw == MODE_RELAY_RAW_UDP;
}

inline TransportMode normalizeTransportMode(uint8_t raw) {
    return isValidTransportModeValue(raw)
        ? static_cast<TransportMode>(raw)
        : MODE_RELAY_KCP;
}

inline bool isValidFecModeValue(uint8_t raw) {
    return raw == FEC_NONE ||
           raw == FEC_10 ||
           raw == FEC_30 ||
           raw == FEC_50 ||
           raw == FEC_70 ||
           raw == FEC_100 ||
           raw == FEC_200;
}

inline FecMode normalizeFecMode(uint8_t raw, TransportMode mode) {
    FecMode fec = isValidFecModeValue(raw)
        ? static_cast<FecMode>(raw)
        : FEC_NONE;
    if (mode == MODE_RELAY_TCP)
        return FEC_NONE;
    return fec;
}

inline bool isValidKcpProfileValue(uint8_t raw) {
    return raw == KCP_PROFILE_REALTIME ||
           raw == KCP_PROFILE_BULK;
}

inline KcpProfile normalizeKcpProfile(uint8_t raw) {
    return isValidKcpProfileValue(raw)
        ? static_cast<KcpProfile>(raw)
        : KCP_PROFILE_REALTIME;
}

inline bool isValidRoomMtuValue(int mtu) {
    return mtu == ROOM_MTU_SAFE ||
           mtu == ROOM_MTU_BALANCED ||
           mtu == ROOM_MTU_AGGRESSIVE;
}

inline uint16_t normalizeRoomMtu(int mtu) {
    return isValidRoomMtuValue(mtu)
        ? static_cast<uint16_t>(mtu)
        : static_cast<uint16_t>(ROOM_MTU_DEFAULT);
}

inline int transportPayloadBudget(int roomMtu,
                                  TransportMode mode,
                                  bool fecEnabled,
                                  bool encrypted)
{
    int budget = static_cast<int>(normalizeRoomMtu(roomMtu));
    budget -= static_cast<int>(sizeof(UdpRelayHeader));
    if (fecEnabled) budget -= static_cast<int>(sizeof(FecHeader));
    if (encrypted) budget -= 1 + 4 + 8 + 16; // UDP_ENCRYPTED + session + counter + MAC
    if (mode == MODE_RELAY_RAW_UDP)
        budget -= static_cast<int>(sizeof(FragHeader));
    if (budget < 256) budget = 256;
    if (budget > RAW_UDP_MAX_FRAG_PAYLOAD) budget = RAW_UDP_MAX_FRAG_PAYLOAD;
    return budget;
}

inline int kcpMtuFromRoomMtu(int roomMtu,
                             bool fecEnabled = false,
                             bool encrypted = false) {
    return transportPayloadBudget(roomMtu, MODE_RELAY_KCP, fecEnabled, encrypted);
}

struct RoomTrafficPolicy {
    TransportMode transportMode;
    FecMode       fecMode;
    KcpProfile    kcpProfile;

    RoomTrafficPolicy()
        : transportMode(MODE_RELAY_KCP),
          fecMode(FEC_NONE),
          kcpProfile(KCP_PROFILE_REALTIME) {}
};

inline RoomTrafficPolicy makeDefaultTcpPolicy() {
    RoomTrafficPolicy p;
    p.transportMode = MODE_RELAY_RAW_UDP;
    p.fecMode = FEC_NONE;
    p.kcpProfile = KCP_PROFILE_REALTIME;
    return p;
}

inline RoomTrafficPolicy makeDefaultUdpPolicy() {
    RoomTrafficPolicy p;
    p.transportMode = MODE_RELAY_KCP;
    p.fecMode = FEC_NONE;
    p.kcpProfile = KCP_PROFILE_REALTIME;
    return p;
}

inline RoomTrafficPolicy normalizeTrafficPolicy(uint8_t transport,
                                                uint8_t fec,
                                                uint8_t profile,
                                                const RoomTrafficPolicy& fallback)
{
    RoomTrafficPolicy p = fallback;
    if (isValidTransportModeValue(transport))
        p.transportMode = static_cast<TransportMode>(transport);
    p.fecMode = normalizeFecMode(fec, p.transportMode);
    p.kcpProfile = normalizeKcpProfile(profile);
    if (p.transportMode == MODE_RELAY_TCP)
        p.fecMode = FEC_NONE;
    return p;
}

struct PeerInfo {
    uint32_t      peerId;
    uint32_t      virtualIP;
    TransportType transport;
    std::string   name;
};

struct RoomListItem {
    uint32_t      roomId;
    char          roomName[MAX_ROOM_NAME_LEN + 1];
    uint8_t       playerCount;
    uint8_t       maxPlayers;
    RoomTrafficPolicy tcpPolicy;
    RoomTrafficPolicy udpPolicy;
    uint8_t       passwordProtected;
    uint16_t      mtu;
};

inline const RoomTrafficPolicy& policyForTraffic(const RoomTrafficPolicy& tcpPolicy,
                                                 const RoomTrafficPolicy& udpPolicy,
                                                 TrafficClass cls)
{
    return cls == TRAFFIC_TCP ? tcpPolicy : udpPolicy;
}

inline TrafficClass trafficClassFromIpPacket(const uint8_t* ipPacket, size_t len)
{
    if (len < 20) return TRAFFIC_UDP;
    uint8_t version = (ipPacket[0] >> 4) & 0x0F;
    if (version != 4) return TRAFFIC_UDP;
    uint8_t ihl = (ipPacket[0] & 0x0F) * 4;
    if (ihl < 20 || len < ihl) return TRAFFIC_UDP;
    uint8_t proto = ipPacket[9];
    return proto == 6 ? TRAFFIC_TCP : TRAFFIC_UDP;
}

inline int fecParityCount(FecMode mode, int dataCount) {
    switch (mode) {
    case FEC_10:  return (dataCount * 1 + 9) / 10;  // ceil(dataCount * 0.1)
    case FEC_30:  return (dataCount * 3 + 9) / 10;  // ceil(dataCount * 0.3)
    case FEC_50:  return (dataCount * 5 + 9) / 10;  // ceil(dataCount * 0.5)
    case FEC_70:  return (dataCount * 7 + 9) / 10;  // ceil(dataCount * 0.7)
    case FEC_100: return dataCount;                  // 1:1 redundancy
    case FEC_200: return dataCount * 2;              // 2:1 redundancy
    default:      return 0;
    }
}

inline const char* fecModeName(FecMode m) {
    switch (m) {
    case FEC_10:  return "FEC 10%";
    case FEC_30:  return "FEC 30%";
    case FEC_50:  return "FEC 50%";
    case FEC_70:  return "FEC 70%";
    case FEC_100: return "FEC 100%";
    case FEC_200: return "FEC 200%";
    default:      return "None";
    }
}

inline const char* transportName(TransportType t) {
    switch (t) {
    case TRANSPORT_RELAY_KCP:     return "Relay (KCP)";
    case TRANSPORT_RELAY_TCP:     return "Relay (TCP)";
    case TRANSPORT_RELAY_RAW_UDP: return "Relay (Raw UDP)";
    default:                      return "None";
    }
}

#ifdef QT_CORE_LIB
inline QString virtualIPToString(uint32_t ip) {
    return QString("%1.%2.%3.%4")
        .arg((ip >> 24) & 0xFF)
        .arg((ip >> 16) & 0xFF)
        .arg((ip >> 8) & 0xFF)
        .arg(ip & 0xFF);
}

inline uint32_t stringToVirtualIP(const QString& s) {
    QStringList parts = s.split('.');
    if (parts.size() != 4) return 0;
    return (parts[0].toUInt() << 24) |
           (parts[1].toUInt() << 16) |
           (parts[2].toUInt() << 8) |
           parts[3].toUInt();
}
#endif

extern bool g_verboseLog;

} // namespace VLan

#endif // VLAN_PROTOCOL_H

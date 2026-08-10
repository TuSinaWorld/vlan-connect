#ifndef VLAN_SIGNAL_SERVER_H
#define VLAN_SIGNAL_SERVER_H

#include "protocol.h"
#include "byte_buffer.h"
#include "net_common.h"
#include "room.h"
#include "relay_server.h"

#include <map>
#include <vector>
#include <cstdint>
#include <string>
#include <set>
#include <csignal>

namespace VLan {

struct ServerLimits {
    size_t maxClients;
    size_t maxPending;
    size_t maxRooms;
    size_t maxClientsPerIp;
    size_t maxPendingPerIp;
    size_t maxSendBufferBytes;

    ServerLimits()
        : maxClients(256), maxPending(64), maxRooms(128),
          maxClientsPerIp(32), maxPendingPerIp(8),
          maxSendBufferBytes(64 * 1024 * 1024) {}
};

class SignalServer {
public:
    SignalServer();
    ~SignalServer();

    bool setAuthPassword(const std::string& password);
    bool authEnabled() const { return m_authReady; }
    void setLimits(const ServerLimits& limits) { m_limits = limits; }

    bool init(uint16_t port = DEFAULT_PORT);
    void run(const volatile sig_atomic_t* stopRequested = nullptr);
    void stop();

private:
    enum class DisconnectReason {
        Network,
        Takeover,
        LogoutComplete
    };

    int  createTcpListener(uint16_t port);
    int  createUdpSocket(uint16_t port);
    void epollAdd(int fd, uint32_t events);
    void epollDel(int fd);

    void handleTcpAccept();
    void handlePendingData(int fd);
    void handleClientData(int fd);
    void handleDataChannelData(int fd);
    void processDataRecvBuffer(ClientSession& client);
    void handleClientDisconnect(int fd);
    void handleDataChannelDisconnect(int fd);
    void handleUdpPacket(int fd);
    void checkTimeouts();
    void destroyClient(int fd, DisconnectReason reason);
    void closeDataChannel(ClientSession& c);
    void unbindClientIndexes(ClientSession& c);
    ClientSession* findClientOwnerByPeerId(uint32_t peerId);
    ClientSession* findClientOwnerBySecureSessionId(uint32_t sessionId);
    ClientSession* findClientOwnerByDataFd(int dataFd);
    ClientSession* findClientByPeerId(uint32_t peerId);
    ClientSession* findClientBySecureSessionId(uint32_t sessionId);
    ClientSession* findClientByDataFd(int dataFd);
    bool bindPeerIndex(ClientSession& c, uint32_t peerId);
    bool bindSecureSessionIndex(ClientSession& c, uint32_t sessionId);
    bool bindDataFdIndex(ClientSession& c, int dataFd);

    void processMessage(ClientSession& client, uint8_t msgType,
                        const uint8_t* payload, size_t len);
    void processDataMessage(ClientSession& client, uint8_t msgType,
                            const uint8_t* payload, size_t len);
    void onClientHello(ClientSession& c, const uint8_t* p, size_t len);
    void onServerAuth(ClientSession& c, const uint8_t* p, size_t len);
    void onLogin(ClientSession& c, const uint8_t* p, size_t len);
    void onCreateRoom(ClientSession& c, const uint8_t* p, size_t len);
    void onJoinRoom(ClientSession& c, const uint8_t* p, size_t len);
    void onResumeRoom(ClientSession& c, const uint8_t* p, size_t len);
    void onAuthResponse(ClientSession& c, const uint8_t* p, size_t len);
    void completeJoin(ClientSession& c, Room* room);
    void completeResume(ClientSession& c, Room* room, RoomLease* lease);
    void onLeaveRoom(ClientSession& c);
    void onLogout(ClientSession& c);
    void onListRooms(ClientSession& c);
    void onRequestRelay(ClientSession& c, const uint8_t* p, size_t len);
    void onPing(ClientSession& c);
    void onTcpRelayData(ClientSession& c, const uint8_t* p, size_t len);
    void onSecureDataChannelInit(int fd, const uint8_t* p, size_t len);
    struct RoomListState {
        uint32_t roomId;
        std::string roomName;
        uint8_t playerCount;
        uint8_t maxPlayers;
        RoomTrafficPolicy tcpPolicy;
        RoomTrafficPolicy udpPolicy;
        uint8_t passwordProtected;
        uint16_t mtu;

        bool operator==(const RoomListState& other) const;
        bool operator!=(const RoomListState& other) const {
            return !(*this == other);
        }
    };
    std::map<uint32_t, RoomListState> collectRoomListState();
    void writeRoomListItem(ByteBuffer& body, const RoomListState& room) const;
    std::vector<ByteBuffer> buildRoomListPages(
        const std::map<uint32_t, RoomListState>& rooms) const;
    void broadcastRoomListPush();

    bool sendMsg(int fd, uint8_t msgType, const ByteBuffer& body);
    void sendMsg(int fd, uint8_t msgType);
    void sendClientMsg(ClientSession& c, uint8_t msgType, const ByteBuffer& body);
    void sendClientMsg(ClientSession& c, uint8_t msgType);
    bool sendDataMsg(ClientSession& c, uint8_t msgType, const ByteBuffer& body);
    void sendError(int fd, const std::string& text);
    void broadcastToRoom(uint32_t roomId, uint8_t msgType,
                         const ByteBuffer& body, uint32_t excludePeerId = 0);
    void notifyRelayPeers(ClientSession& c);
    void sendRelayReady(ClientSession& c, uint32_t peerId);
    uint32_t allocatePeerId();
    void releasePeerId(uint32_t peerId);
    bool generateLeaseToken(uint8_t token[RECONNECT_TOKEN_SIZE]);
    bool validateResumeLease(uint32_t roomId, uint32_t peerId,
                             const uint8_t token[RECONNECT_TOKEN_SIZE],
                             Room** roomOut, RoomLease** leaseOut);
    void sendJoinResponse(ClientSession& c, Room* room,
                          const uint8_t token[RECONNECT_TOKEN_SIZE]);
    void markSessionOffline(ClientSession& c);
    void closeClientForTakeover(ClientSession& c);
    void cleanupExpiredLeases(time_t now);
    bool decryptClientFrame(ClientSession& c, const uint8_t* payload, size_t len,
                            uint8_t* innerType, std::vector<uint8_t>* innerPayload);
    ByteBuffer encryptForClient(ClientSession& c, uint8_t msgType, const ByteBuffer& body);
    bool decryptUdpPacket(const uint8_t* data, size_t len, ClientSession** src,
                          std::vector<uint8_t>* plain);
    void sendEncryptedUdp(int udpFd, ClientSession& dst,
                          const std::vector<uint8_t>& plain,
                          const struct sockaddr_in& dstAddr);

    void flushSendBuf(ClientSession& c);
    bool flushDataSendBuf(ClientSession& c);
    void epollMod(int fd, uint32_t events);
    void handleWritable(int fd);
    bool appendSendBuffer(ClientSession& c, bool dataChannel,
                          const uint8_t* first, size_t firstLen,
                          const uint8_t* second, size_t secondLen);
    void consumeSendBuffer(ClientSession& c, bool dataChannel, size_t count);
    void clearSendBuffer(ClientSession& c, bool dataChannel);
    void logDataDropSampled(const char* reason);
    void logSendRejectSampled(const char* reason, uint32_t peerId,
                              size_t queuedBytes);

    struct TokenBucket {
        double tokens;
        time_t updated;
        TokenBucket() : tokens(0.0), updated(0) {}
    };
    struct IpState {
        size_t clients;
        size_t pending;
        TokenBucket accepts;
        TokenBucket authFailures;
        TokenBucket creates;
        TokenBucket roomPasswordFailures;
        time_t lastActivity;
        IpState() : clients(0), pending(0), lastActivity(0) {}
    };
    struct PendingConn;
    bool takeIpToken(uint32_t ip, TokenBucket IpState::* bucket,
                     double burst, double refillSeconds);
    void removePending(std::map<int, PendingConn>::iterator it, bool closeFd);
    void releaseClientIp(ClientSession& c);
    void cleanupIpStates(time_t now);

    int  m_epfd;
    int  m_tcpListenFd;
    int  m_udpFd;
    bool m_running;

    struct PendingConn {
        int fd;
        uint32_t remoteIpv4;
        std::vector<uint8_t> recvBuf;
        time_t created;
        PendingConn() : fd(-1), remoteIpv4(0), created(0) {}
    };

    std::map<int, ClientSession>       m_clients;
    std::map<int, PendingConn>         m_pending;
    std::map<int, int>                 m_dataFdMap;
    std::map<uint32_t, int>            m_peerMap;
    RoomManager                        m_rooms;
    uint32_t                           m_nextPeerId;
    std::set<uint32_t>                 m_freePeerIds;
    bool                               m_authReady;
    uint8_t                            m_serverAuthHash[32];
    std::map<uint32_t, int>            m_secureSessionMap;
    ServerLimits                       m_limits;
    size_t                             m_globalSendBytes;
    time_t                             m_lastDataDropLog;
    size_t                             m_suppressedDataDrops;
    time_t                             m_lastSendRejectLog;
    size_t                             m_suppressedSendRejects;
    std::map<uint32_t, IpState>        m_ipStates;
    uint64_t                           m_roomListRevision;
    std::map<uint32_t, RoomListState>  m_publishedRooms;
};

} // namespace VLan
#endif // VLAN_SIGNAL_SERVER_H

#ifndef VLAN_CLI_NET_H
#define VLAN_CLI_NET_H

#include "cli_common.h"
#include "auth_kdf_task.h"
#include <functional>
#include <map>
#include <vector>
#include <string>

namespace VLan {

// ======================== CliSignalClient ========================

struct CliRoomListItem {
    uint32_t      roomId;
    std::string   roomName;
    uint8_t       playerCount;
    uint8_t       maxPlayers;
    RoomTrafficPolicy tcpPolicy;
    RoomTrafficPolicy udpPolicy;
    uint8_t       passwordProtected;
    uint16_t      mtu;
};

class CliSignalClient {
public:
    CliSignalClient();
    ~CliSignalClient();

    bool connectTo(const std::string& ip, uint16_t port);
    void disconnect();
    bool isConnected() const { return m_conn.connected; }
    socket_t fd() const { return m_conn.fd; }
    bool wantsWrite() const { return m_conn.connecting || !m_conn.sendBuf.empty(); }
    bool secureEnabled() const { return m_secureReady; }
    bool authInProgress() const { return m_authOperation != 0; }
    bool roomOperationPending() const { return m_roomOperationPending; }
    uint32_t secureSessionId() const { return m_secureSessionId; }
    const Buffer& secureMaster() const { return m_secureMaster; }
    void setServerPassword(const std::string& password);
    void continueServerAuth();

    void login(const std::string& name,
               bool hasResume = false,
               uint32_t resumeRoomId = 0,
               uint32_t resumePeerId = 0,
               const uint8_t* resumeToken = nullptr);
    void createRoom(const std::string& roomName, uint8_t maxPlayers,
                    RoomTrafficPolicy tcpPolicy,
                    RoomTrafficPolicy udpPolicy,
                    uint16_t mtu,
                    bool passwordProtected, const std::string& password);
    void joinRoom(uint32_t roomId, const std::string& password);
    void resumeRoom(uint32_t roomId, uint32_t peerId, const uint8_t* resumeToken);
    void leaveRoom();
    void logout();
    void abandonRoomOperation();
    void listRooms();
    void requestRelay(uint32_t targetPeerId);
    void sendRelayData(uint32_t srcPeerId, uint32_t dstPeerId,
                       TrafficClass cls, const Buffer& data);
    void sendPing();

    void onReadable();
    void onWritable();
    void checkTimeouts();

    uint32_t myPeerId() const { return m_myPeerId; }

    /* Callbacks */
    std::function<void()>  onConnected;
    std::function<void()>  onDisconnected;
    std::function<void(const std::string&)> onConnectFailed;
    std::function<void(uint32_t peerId, bool resumeAccepted)> onLoginResponse;
    std::function<void(uint32_t roomId, uint32_t virtualIP,
                       RoomTrafficPolicy, RoomTrafficPolicy, uint16_t mtu,
                       bool passwordProtected, const Buffer& leaseToken)> onRoomCreated;
    std::function<void(uint32_t roomId, uint32_t virtualIP,
                       RoomTrafficPolicy, RoomTrafficPolicy, uint16_t mtu,
                       bool passwordProtected,
                       const std::vector<PeerInfo>& members,
                       const Buffer& leaseToken)> onJoinResponse;
    std::function<void(PeerInfo)> onPeerJoined;
    std::function<void(PeerInfo)> onPeerResumed;
    std::function<void(uint32_t peerId)> onPeerLeft;
    std::function<void(const std::vector<CliRoomListItem>&, bool)> onRoomList;
    std::function<void(uint32_t peerId)> onRelayReady;
    std::function<void(uint32_t srcPeerId,
                       TrafficClass cls,
                       Buffer data)> onRelayData;
    std::function<void()> onLogoutAck;
    std::function<void()> onServerPasswordRequired;
    std::function<void(uint32_t sessionId, const Buffer& master)> onSecureSessionEstablished;
    std::function<void(const std::string&)> onServerError;
    std::function<void(const std::string&)> onRoomOperationFailure;
    std::function<void(uint16_t, const std::string&)> onFatalAuthError;
    std::function<void(int rttMs)> onServerRtt;
    std::function<void(const uint8_t* challenge)> onAuthChallenge;

    void sendAuthResponse(const uint8_t* response);

private:
    friend class CliRoomReadinessTests;
    void sendMsg(uint8_t msgType, const ByteBuffer& body);
    void sendMsg(uint8_t msgType);
    bool processMessage(uint8_t msgType, const uint8_t* payload, size_t len);
    void sendClientHello();
    void sendServerAuth();
    void pollAuthTask();
    void cancelRoomOperation();
    void failAuthentication(AuthErrorCode code, const std::string& message);
    void resetSecureState();
    void failSignalFrame(uint8_t msgType, size_t len,
                         const char* error, size_t offset);
    void notifyDisconnectedOnce();
    CliRoomListItem readRoomListItem(ByteBuffer& bb);
    void resetRoomListState();
    void emitRoomList(bool pushed);

    TcpConnection m_conn;
    uint32_t      m_myPeerId;
    uint32_t      m_pingSentTime;
    uint32_t      m_connectStartTime;
    uint32_t      m_authStartedMs;
    std::string   m_pendingRoomPassword;
    AuthKdfTask   m_authTask;
    int           m_authOperation;
    ByteBuffer    m_pendingCreate;
    uint16_t      m_pendingCreateMtu;
    uint8_t       m_authSalt[AUTH_SALT_SIZE];
    uint8_t       m_expectedServerProof[32];
    uint8_t       m_roomChallenge[32];
    bool          m_hasPendingAuth;
    bool          m_roomOperationPending;
    bool          m_discardRoomResponses;
    uint64_t      m_roomRequestStarted;
    bool          m_sendFailurePending;
    std::string   m_serverPassword;
    bool          m_serverAuthRequired;
    bool          m_secureReady;
    bool          m_disconnectNotified;
    uint32_t      m_secureSessionId;
    uint8_t       m_clientNonce[16];
    uint8_t       m_serverNonce[16];
    uint8_t       m_clientPrivKey[32];
    uint8_t       m_clientPubKey[32];
    uint8_t       m_serverPubKey[32];
    Buffer        m_secureMaster;
    SecureFrameCipher m_secureCipher;
    uint64_t      m_roomListRevision;
    uint64_t      m_snapshotRevision;
    uint16_t      m_snapshotPageCount;
    uint16_t      m_snapshotNextPage;
    std::map<uint32_t, CliRoomListItem> m_roomListCache;
    std::map<uint32_t, CliRoomListItem> m_snapshotRooms;
};

// ======================== CliDataChannel ========================

class CliDataChannel {
public:
    CliDataChannel();
    ~CliDataChannel();

    bool connectTo(const std::string& ip, uint16_t port, uint32_t peerId);
    void disconnect();
    bool isConnected() const { return m_established; }
    socket_t fd() const { return m_conn.fd; }
    bool wantsWrite() const { return m_conn.connecting || !m_conn.sendBuf.empty(); }
    bool installSecureSession(uint32_t sessionId, const Buffer& master);
    void clearSecurityContext();
    DataPlaneSecurityMode securityMode() const { return m_securityMode; }

    void sendRelayData(uint32_t srcPeerId, uint32_t dstPeerId,
                       TrafficClass cls, const Buffer& data);
    void sendPing();

    void onReadable();
    void onWritable();
    void checkTimeouts();

    std::function<void()> onConnectedCb;
    std::function<void()> onDisconnectedCb;
    std::function<void(const std::string&)> onSecurityFailure;
    std::function<void(uint32_t srcPeerId, TrafficClass cls, Buffer data)> onRelayData;

private:
    void sendMsg(uint8_t msgType, const ByteBuffer& body);
    void sendMsg(uint8_t msgType);
    bool processMessage(uint8_t msgType, const uint8_t* payload, size_t len);
    void scheduleReconnect();
    void failDataChannelFrame(uint8_t msgType, size_t len,
                              const char* error, size_t offset);

    TcpConnection m_conn;
    std::string   m_host;
    uint16_t      m_port;
    uint32_t      m_peerId;
    bool          m_established;
    bool          m_needReconnect;
    bool          m_sendFailurePending;
    uint32_t      m_reconnectTime;
    DataPlaneSecurityMode m_securityMode;
    uint32_t      m_secureSessionId;
    Buffer        m_secureMaster;
    SecureFrameCipher m_cipher;
};

} // namespace VLan
#endif // VLAN_CLI_NET_H

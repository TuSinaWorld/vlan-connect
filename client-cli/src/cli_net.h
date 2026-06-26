#ifndef VLAN_CLI_NET_H
#define VLAN_CLI_NET_H

#include "cli_common.h"
#include <functional>
#include <map>
#include <vector>
#include <string>
#include <random>

namespace VLan {

// ======================== CliSignalClient ========================

struct CliRoomListItem {
    uint32_t      roomId;
    std::string   roomName;
    uint8_t       playerCount;
    uint8_t       maxPlayers;
    TransportMode transportMode;
    FecMode       fecMode;
    uint8_t       encrypted;
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

    void login(const std::string& name);
    void createRoom(const std::string& roomName, uint8_t maxPlayers,
                    TransportMode mode, FecMode fecMode,
                    uint16_t mtu,
                    bool encrypted, const uint8_t* passwordHash);
    void joinRoom(uint32_t roomId, const uint8_t* authHash);
    void leaveRoom();
    void listRooms();
    void reportNatType(NatType type);
    void reportPunchResult(uint32_t targetPeerId, bool success);
    void requestRelay(uint32_t targetPeerId);
    void sendPing();

    void onReadable();
    void onWritable();
    void checkTimeouts();

    uint32_t myPeerId() const { return m_myPeerId; }

    /* Callbacks */
    std::function<void()>  onConnected;
    std::function<void()>  onDisconnected;
    std::function<void(const std::string&)> onConnectFailed;
    std::function<void(uint32_t peerId)> onLoginResponse;
    std::function<void(uint32_t roomId, uint32_t virtualIP,
                       TransportMode, FecMode, uint16_t mtu, bool encrypted,
                       const uint8_t* salt, const uint8_t* sessionSeed)> onRoomCreated;
    std::function<void(uint32_t roomId, uint32_t virtualIP,
                       TransportMode, FecMode, uint16_t mtu, bool encrypted,
                       const uint8_t* salt, const uint8_t* sessionSeed,
                       const std::vector<PeerInfo>& members)> onJoinResponse;
    std::function<void(PeerInfo)> onPeerJoined;
    std::function<void(uint32_t peerId)> onPeerLeft;
    std::function<void(const std::vector<CliRoomListItem>&)> onRoomList;
    std::function<void(uint32_t peerId, uint32_t virtualIP,
                       NatType natType, uint32_t publicIP, uint16_t publicPort)> onPunchNotify;
    std::function<void(uint32_t peerId)> onRelayReady;
    std::function<void(const std::string&)> onServerError;
    std::function<void(int rttMs)> onServerRtt;
    std::function<void(const uint8_t* challenge)> onAuthChallenge;

    void sendAuthResponse(const uint8_t* response);

private:
    void processMessage(uint8_t msgType, const uint8_t* payload, size_t len);

    TcpConnection m_conn;
    uint32_t      m_myPeerId;
    uint32_t      m_pingSentTime;
    uint32_t      m_connectStartTime;
    uint8_t       m_pendingAuthHash[CIPHER_KEY_SIZE];
    bool          m_hasPendingAuth;
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

    void sendRelayData(uint32_t srcPeerId, uint32_t dstPeerId, const Buffer& data);
    void sendPing();

    void onReadable();
    void onWritable();
    void checkTimeouts();

    std::function<void()> onConnectedCb;
    std::function<void()> onDisconnectedCb;
    std::function<void(uint32_t srcPeerId, Buffer data)> onRelayData;

private:
    void processMessage(uint8_t msgType, const uint8_t* payload, size_t len);
    void scheduleReconnect();

    TcpConnection m_conn;
    std::string   m_host;
    uint16_t      m_port;
    uint32_t      m_peerId;
    bool          m_established;
    bool          m_needReconnect;
    uint32_t      m_reconnectTime;
};

// ======================== CliNatDetector ========================

class CliNatDetector {
public:
    CliNatDetector();

    void detect(socket_t udpFd, uint16_t localPort,
                uint32_t serverIP, uint16_t serverPort, uint32_t myPeerId);
    void handleStunResponse(const uint8_t* data, size_t len);
    void checkTimeout(socket_t udpFd);

    bool done() const { return m_done; }
    NatType result() const { return m_result; }
    uint32_t observedIP() const { return m_observedIP; }
    uint16_t observedPort() const { return m_observedPort; }

    std::function<void(NatType, uint32_t publicIP, uint16_t publicPort)> onDetected;

private:
    void sendProbe(socket_t udpFd);

    uint32_t m_serverIP;
    uint16_t m_serverPort;
    uint16_t m_localPort;
    uint32_t m_myPeerId;
    uint32_t m_token;
    uint32_t m_observedIP;
    uint16_t m_observedPort;
    bool     m_done;
    int      m_retryCount;
    uint32_t m_lastProbeTime;
    NatType  m_result;
};

// ======================== CliHolePuncher ========================

class CliHolePuncher {
public:
    CliHolePuncher(uint32_t myPeerId);

    void startPunch(uint32_t targetPeerId, uint32_t targetIP, uint16_t targetPort);
    void cancelPunch(uint32_t targetPeerId);
    void handleIncomingPacket(const uint8_t* data, size_t len,
                              uint32_t fromIP, uint16_t fromPort);
    void update(socket_t udpFd);

    std::function<void(uint32_t peerId, uint32_t ip, uint16_t port)> onPunchSucceeded;
    std::function<void(uint32_t peerId)> onPunchFailed;

private:
    struct PunchAttempt {
        uint32_t targetPeerId;
        uint32_t targetIP;
        uint16_t targetPort;
        uint32_t token;
        int      attempts;
        bool     ackReceived;
        uint32_t lastSendTime;
    };

    void sendPunchPacket(socket_t udpFd, const PunchAttempt& a);
    void sendAck(socket_t udpFd, uint32_t token, uint32_t ip, uint16_t port);

    uint32_t m_myPeerId;
    std::map<uint32_t, PunchAttempt> m_attempts;
    std::mt19937 m_rng;
};

} // namespace VLan
#endif // VLAN_CLI_NET_H

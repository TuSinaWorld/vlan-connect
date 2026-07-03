#ifndef VLAN_CLI_APP_H
#define VLAN_CLI_APP_H

#include "cli_common.h"
#include "cli_net.h"
#include "cli_tunnel.h"
#include "cli_tun.h"
#include <atomic>
#include <thread>
#include <string>
#include <set>

namespace VLan {

/*
 * Main CLI application.
 *
 * Orchestrates the full lifecycle just like RoomManager in the GUI client,
 * but driven by a select()-based event loop and stdin commands.
 *
 * Thread model:
 *   - Main thread:  select() on TCP + UDP sockets, timer management
 *   - TUN thread:   reads packets from virtual NIC (CliTunAdapter)
 *   - Stdin thread:  reads user commands
 */
class CliApp {
public:
    CliApp();
    ~CliApp();

    void setServer(const std::string& host, uint16_t port);
    void setServerPassword(const std::string& password);
    void setPlayerName(const std::string& name);
    void setVerbose(bool v);

    int run();
    void requestStop();

private:
    void setupCallbacks();
    void eventLoop();
    void processCommand(const std::string& line);
    void printHelp();
    void printStatus();
    void printPeers();

    void doConnect();
    void doDisconnect();
    void doCreateRoom(const std::string& args);
    void doJoinRoom(const std::string& args);
    void doLeaveRoom();

    /* Signal callbacks */
    void onSignalConnected();
    void onSignalDisconnected();
    void onLoginResponse(uint32_t peerId, bool resumeAccepted);
    void onRoomCreated(uint32_t roomId, uint32_t virtualIP,
                       RoomTrafficPolicy tcpPolicy,
                       RoomTrafficPolicy udpPolicy,
                       uint16_t mtu,
                       bool passwordProtected,
                       const Buffer& leaseToken);
    void onJoinResponse(uint32_t roomId, uint32_t virtualIP,
                        RoomTrafficPolicy tcpPolicy,
                        RoomTrafficPolicy udpPolicy,
                        uint16_t mtu,
                        bool passwordProtected,
                        const std::vector<PeerInfo>& members,
                        const Buffer& leaseToken);
    void onPeerJoined(PeerInfo info);
    void onPeerLeft(uint32_t peerId);
    void onRelayReady(uint32_t peerId);
    void onLogoutAck();
    void onTransportDead(uint32_t peerId, TrafficClass cls);

    void setupTun();
    void teardownTun();
    void setupRelayTunnel(uint32_t peerId, TrafficClass cls);
    void setupRawUdpRelayTunnel(uint32_t peerId, TrafficClass cls);
    void setupTcpRelayTunnel(uint32_t peerId, TrafficClass cls);
    void setupPolicyTunnel(uint32_t peerId, TrafficClass cls);
    void handleTcpRelayReceived(uint32_t srcPeerId, TrafficClass cls, Buffer data);
    void handleReconnectRoomList(const std::vector<CliRoomListItem>& rooms);
    void beginGracefulDisconnect(bool exitAfterDisconnect);
    void finishGracefulDisconnect();
    void clearPendingRebuild(uint32_t peerId);
    bool takePendingRebuild(uint32_t peerId, TrafficClass cls);
    static uint64_t transportKey(uint32_t peerId, TrafficClass cls);
    void startResumeLeaseDeadline();
    void expireResumeLeaseIfNeeded();
    bool hasUsableResumeLease();
    void rememberResumeLease(uint32_t roomId, uint32_t peerId,
                             uint32_t virtualIP, const Buffer& token);
    void clearResumeLease();

    void stdinReadLoop();

    std::string   m_serverHost;
    std::string   m_resolvedIP;
    uint16_t      m_port;
    std::string   m_playerName;
    std::string   m_serverPassword;

    CliSignalClient   m_signal;
    CliDataChannel    m_dataChannel;
    CliTunnelManager  m_tunnel;
    CliTunAdapter*    m_tun;

    uint32_t      m_currentRoomId;
    uint32_t      m_myVirtualIP;
    RoomTrafficPolicy m_tcpPolicy;
    RoomTrafficPolicy m_udpPolicy;
    uint16_t      m_roomMtu;
    bool          m_roomPasswordProtected;
    std::string   m_roomPassword;

    std::atomic<bool> m_running;
    ThreadSafeQueue<std::string> m_cmdQueue;
    std::thread   m_stdinThread;

    uint32_t m_lastPingTime;
    uint32_t m_lastKcpUpdateTime;
    uint32_t m_lastUdpKeepaliveTime;
    uint32_t m_lastTcpRelayCheckTime;
    uint32_t m_lastLatencyCheckTime;
    uint32_t m_lastDataChannelPingTime;

    static const int MAX_RECONNECT_ATTEMPTS = 3;

    bool          m_wantReconnect;
    int           m_reconnectAttempts;
    uint32_t      m_nextReconnectTime;
    bool          m_wasInRoom;
    bool          m_pendingResumeRoom;
    bool          m_manualDisconnecting;
    bool          m_logoutPending;
    bool          m_exitAfterDisconnect;
    uint32_t      m_logoutDeadline;
    bool          m_hasResumeLease;
    uint32_t      m_resumeRoomId;
    uint32_t      m_resumePeerId;
    uint32_t      m_resumeVirtualIP;
    uint32_t      m_resumeLeaseDeadlineMs;
    Buffer        m_resumeToken;

    uint32_t      m_savedRoomId;
    std::string   m_savedRoomName;
    uint8_t       m_savedMaxPlayers;
    RoomTrafficPolicy m_savedTcpPolicy;
    RoomTrafficPolicy m_savedUdpPolicy;
    uint16_t      m_savedRoomMtu;
    bool          m_savedRoomPasswordProtected;

    std::vector<CliRoomListItem> m_cachedRoomList;
    std::set<uint64_t> m_pendingRebuild;
};

} // namespace VLan
#endif // VLAN_CLI_APP_H

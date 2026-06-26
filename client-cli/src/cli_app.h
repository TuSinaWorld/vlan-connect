#ifndef VLAN_CLI_APP_H
#define VLAN_CLI_APP_H

#include "cli_common.h"
#include "cli_net.h"
#include "cli_tunnel.h"
#include "cli_tun.h"
#include <atomic>
#include <thread>
#include <string>

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
    void doCreateRoom(const std::string& args);
    void doJoinRoom(const std::string& args);
    void doLeaveRoom();

    /* Signal callbacks */
    void onSignalConnected();
    void onSignalDisconnected();
    void onLoginResponse(uint32_t peerId);
    void onRoomCreated(uint32_t roomId, uint32_t virtualIP,
                       TransportMode tmode, FecMode fmode,
                       uint16_t mtu,
                       bool encrypted, const uint8_t* salt,
                       const uint8_t* sessionSeed);
    void onJoinResponse(uint32_t roomId, uint32_t virtualIP,
                        TransportMode tmode, FecMode fmode,
                        uint16_t mtu,
                        bool encrypted, const uint8_t* salt,
                        const uint8_t* sessionSeed,
                        const std::vector<PeerInfo>& members);
    void onPeerJoined(PeerInfo info);
    void onPeerLeft(uint32_t peerId);
    void onPunchNotify(uint32_t peerId, uint32_t virtualIP,
                       NatType natType, uint32_t publicIP, uint16_t publicPort);
    void onRelayReady(uint32_t peerId);
    void onNatDetected(NatType type, uint32_t publicIP, uint16_t publicPort);
    void onTunnelDead(uint32_t peerId);

    void setupTun();
    void teardownTun();
    void setupRelayTunnel(uint32_t peerId);
    void setupRawUdpRelayTunnel(uint32_t peerId);
    void setupTcpRelayTunnel(uint32_t peerId);
    void handleTcpRelayReceived(uint32_t srcPeerId, Buffer data);
    void handleReconnectRoomList(const std::vector<CliRoomListItem>& rooms);

    void stdinReadLoop();

    std::string   m_serverHost;
    std::string   m_resolvedIP;
    uint16_t      m_port;
    std::string   m_playerName;

    CliSignalClient   m_signal;
    CliDataChannel    m_dataChannel;
    CliTunnelManager  m_tunnel;
    CliTunAdapter*    m_tun;
    CliNatDetector    m_natDetector;
    CliHolePuncher*   m_puncher;

    uint32_t      m_currentRoomId;
    uint32_t      m_myVirtualIP;
    NatType       m_myNatType;
    TransportMode m_transportMode;
    FecMode       m_fecMode;
    uint16_t      m_roomMtu;
    bool          m_encrypted;
    std::string   m_roomPassword;

    uint8_t       m_intermediate[CIPHER_KEY_SIZE];
    bool          m_hasIntermediate;
    uint8_t       m_encryptKey[CIPHER_KEY_SIZE];
    uint8_t       m_sessionSeed[CIPHER_SESSION_SEED_SIZE];
    bool          m_hasCipherParams;

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

    std::string   m_savedRoomName;
    uint8_t       m_savedMaxPlayers;
    TransportMode m_savedTransportMode;
    FecMode       m_savedFecMode;
    uint16_t      m_savedRoomMtu;
    bool          m_savedEncrypted;

    std::vector<CliRoomListItem> m_cachedRoomList;
};

} // namespace VLan
#endif // VLAN_CLI_APP_H

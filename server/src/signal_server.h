#ifndef VLAN_SIGNAL_SERVER_H
#define VLAN_SIGNAL_SERVER_H

#include "protocol.h"
#include "byte_buffer.h"
#include "net_common.h"
#include "room.h"
#include "stun_server.h"
#include "relay_server.h"

#include <map>
#include <vector>
#include <cstdint>
#include <string>

namespace VLan {

class SignalServer {
public:
    SignalServer();
    ~SignalServer();

    bool init(uint16_t port = DEFAULT_PORT);
    void run();
    void stop();

private:
    int  createTcpListener(uint16_t port);
    int  createUdpSocket(uint16_t port);
    void epollAdd(int fd, uint32_t events);
    void epollDel(int fd);

    void handleTcpAccept();
    void handlePendingData(int fd);
    void handleClientData(int fd);
    void handleDataChannelData(int fd);
    void handleClientDisconnect(int fd);
    void handleDataChannelDisconnect(int fd);
    void handleUdpPacket(int fd);
    void checkTimeouts();

    void processMessage(ClientSession& client, uint8_t msgType,
                        const uint8_t* payload, size_t len);
    void processDataMessage(ClientSession& client, uint8_t msgType,
                            const uint8_t* payload, size_t len);
    void onLogin(ClientSession& c, const uint8_t* p, size_t len);
    void onCreateRoom(ClientSession& c, const uint8_t* p, size_t len);
    void onJoinRoom(ClientSession& c, const uint8_t* p, size_t len);
    void onAuthResponse(ClientSession& c, const uint8_t* p, size_t len);
    void completeJoin(ClientSession& c, Room* room);
    void onLeaveRoom(ClientSession& c);
    void onListRooms(ClientSession& c);
    void onNatReport(ClientSession& c, const uint8_t* p, size_t len);
    void onPunchResult(ClientSession& c, const uint8_t* p, size_t len);
    void onRequestRelay(ClientSession& c, const uint8_t* p, size_t len);
    void onPing(ClientSession& c);
    void onTcpRelayData(ClientSession& c, const uint8_t* p, size_t len);
    void onDataChannelInit(int fd, const uint8_t* p, size_t len);

    void sendMsg(int fd, uint8_t msgType, const ByteBuffer& body);
    void sendMsg(int fd, uint8_t msgType);
    void sendDataMsg(ClientSession& c, uint8_t msgType, const ByteBuffer& body);
    void sendError(int fd, const std::string& text);
    void broadcastToRoom(uint32_t roomId, uint8_t msgType,
                         const ByteBuffer& body, uint32_t excludePeerId = 0);
    void notifyPunchPeers(ClientSession& c);

    void flushSendBuf(ClientSession& c);
    void flushDataSendBuf(ClientSession& c);
    void epollMod(int fd, uint32_t events);
    void handleWritable(int fd);

    int  m_epfd;
    int  m_tcpListenFd;
    int  m_stunFd;
    bool m_running;

    struct PendingConn {
        int fd;
        std::vector<uint8_t> recvBuf;
        time_t created;
    };

    std::map<int, ClientSession>       m_clients;
    std::map<int, PendingConn>         m_pending;
    std::map<int, ClientSession*>      m_dataFdMap;
    std::map<uint32_t, ClientSession*> m_peerMap;
    RoomManager                        m_rooms;
    uint32_t                           m_nextPeerId;
};

} // namespace VLan
#endif // VLAN_SIGNAL_SERVER_H

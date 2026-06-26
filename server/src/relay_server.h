#ifndef VLAN_RELAY_SERVER_H
#define VLAN_RELAY_SERVER_H

#include "protocol.h"
#include "net_common.h"
#include "room.h"
#include <map>

namespace VLan {

/*
 * Relay handler for both UDP and TCP fallback relay.
 *
 * UDP relay: receives UdpRelayHeader packets, looks up the destination peer's
 *            UDP endpoint, and forwards the entire packet.
 *
 * TCP relay: pure data forwarding over a paired TCP connection.
 */
class RelayHandler {
public:
    /*
     * Forward a UDP relay packet to the destination peer.
     * peerMap: peerId -> ClientSession* (used to look up udpAddr).
     */
    static void processUdpRelay(int udpFd,
                                const uint8_t* data, size_t len,
                                const struct sockaddr_in& from,
                                std::map<uint32_t, ClientSession*>& peerMap);
};

} // namespace VLan
#endif // VLAN_RELAY_SERVER_H

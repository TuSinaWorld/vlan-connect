#ifndef VLAN_STUN_SERVER_H
#define VLAN_STUN_SERVER_H

#include "protocol.h"
#include "net_common.h"
#include "room.h"
#include <map>

namespace VLan {

/*
 * Simplified STUN service.
 *
 * Listens on a single UDP port (shared with relay).  Client sends a
 * StunRequest; the server replies with the observed public IP:Port
 * and records the peer's UDP address for relay routing.
 */
class StunHandler {
public:
    /* Process an incoming StunRequest and send back a StunResponse.
     * Also records the peer's UDP address in the peerMap if peerId is valid. */
    static void processRequest(int udpFd,
                               const uint8_t* data, size_t len,
                               const struct sockaddr_in& from,
                               std::map<uint32_t, ClientSession*>& peerMap);
};

} // namespace VLan
#endif // VLAN_STUN_SERVER_H

#include "stun_server.h"
#include "server_logger.h"
#include <cstring>
#include <cstdio>

namespace VLan {

void StunHandler::processRequest(int udpFd,
                                 const uint8_t* data, size_t len,
                                 const struct sockaddr_in& from,
                                 std::map<uint32_t, ClientSession*>& peerMap)
{
    if (len < sizeof(StunRequest)) return;

    const StunRequest* req = reinterpret_cast<const StunRequest*>(data);
    if (req->type != UDP_STUN_REQUEST) return;

    uint32_t peerId    = ntohl(req->peerId);
    uint32_t token     = ntohl(req->token);
    uint16_t localPort = ntohs(req->localPort);

    if (peerId != 0) {
        auto it = peerMap.find(peerId);
        if (it != peerMap.end()) {
            it->second->udpAddr      = from;
            it->second->udpAddrKnown = true;
        }
    }

    uint32_t observedIP   = ntohl(from.sin_addr.s_addr);
    uint16_t observedPort = ntohs(from.sin_port);

    LOG_DETAIL("[stun] Request from %s peer=%u token=%u localPort=%u -> observed=%s:%u",
               addrToString(from).c_str(), peerId, token, localPort,
               ipToString(observedIP).c_str(), observedPort);

    StunResponse resp;
    memset(&resp, 0, sizeof(resp));
    resp.type         = UDP_STUN_RESPONSE;
    resp.token        = req->token;
    resp.observedIP   = htonl(observedIP);
    resp.observedPort = htons(observedPort);

    ssize_t n = sendto(udpFd, reinterpret_cast<const char*>(&resp), sizeof(resp), 0,
                       reinterpret_cast<const struct sockaddr*>(&from), sizeof(from));
    if (n < 0) {
        LOG_ERROR("[stun] sendto failed: %s", strerror(errno));
    }
}

} // namespace VLan

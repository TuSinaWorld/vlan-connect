#include "relay_server.h"
#include "server_logger.h"
#include <cstring>

namespace VLan {

void RelayHandler::processUdpRelay(int udpFd,
                                   const uint8_t* data, size_t len,
                                   const struct sockaddr_in& from,
                                   ClientSession& src,
                                   ClientSession& dst)
{
    if (len < sizeof(UdpRelayHeader)) return;

    const UdpRelayHeader* hdr = reinterpret_cast<const UdpRelayHeader*>(data);
    if (hdr->type != UDP_RELAY_DATA && hdr->type != UDP_RAW_RELAY_DATA) return;

    uint32_t srcId = ntohl(hdr->srcPeerId);
    uint32_t dstId = ntohl(hdr->dstPeerId);
    const char* relayType = (hdr->type == UDP_RELAY_DATA) ? "KCP" : "RawUDP";

    if (src.peerId != srcId || src.roomId == 0) {
        LOG_DETAIL("[relay] %s relay from unknown/out-of-room peer %u, dropping %zu bytes",
                   relayType, srcId, len);
        return;
    }
    src.udpAddr      = from;
    src.udpAddrKnown = true;

    if (dst.peerId != dstId || !dst.udpAddrKnown) {
        LOG_DETAIL("[relay] %s relay peer %u -> peer %u: dst not found or no UDP addr, dropping %zu bytes",
                   relayType, srcId, dstId, len);
        return;
    }
    if (dst.roomId == 0 || src.roomId != dst.roomId) {
        LOG_ERROR("[relay] %s relay rejected: peer %u room=%u -> peer %u room=%u",
                  relayType, srcId, src.roomId, dstId, dst.roomId);
        return;
    }

    LOG_DETAIL("[relay] %s relay peer %u -> peer %u size=%zu", relayType, srcId, dstId, len);

    const struct sockaddr_in& dstAddr = dst.udpAddr;
    ssize_t n = sendto(udpFd, reinterpret_cast<const char*>(data), len, 0,
                       reinterpret_cast<const struct sockaddr*>(&dstAddr), sizeof(dstAddr));
    if (n < 0) {
        LOG_ERROR("[relay] sendto peer %u failed: %s", dstId, strerror(errno));
    }
}

} // namespace VLan

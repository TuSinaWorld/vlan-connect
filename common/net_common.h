#ifndef VLAN_NET_COMMON_H
#define VLAN_NET_COMMON_H

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <iphlpapi.h>
  #define SOCK_INVALID INVALID_SOCKET
#else
  #include <sys/socket.h>
  #include <sys/types.h>
  #include <sys/epoll.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <signal.h>
  #include <errno.h>
  #define SOCK_INVALID (-1)
#endif

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <chrono>

namespace VLan {

#ifdef _WIN32
  typedef SOCKET socket_t;
  inline int  sock_close(socket_t s) { return closesocket(s); }
  inline int  sock_error()           { return WSAGetLastError(); }
  inline void sock_init() {
      WSADATA w; WSAStartup(MAKEWORD(2,2), &w);
  }
  inline void sock_cleanup() { WSACleanup(); }
#else
  typedef int socket_t;
  inline int  sock_close(socket_t s) { return close(s); }
  inline int  sock_error()           { return errno; }
  inline void sock_init()            { signal(SIGPIPE, SIG_IGN); }
  inline void sock_cleanup()         {}
#endif

inline std::string ipToString(uint32_t ip) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
        (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
        (ip >> 8)  & 0xFF,  ip        & 0xFF);
    return buf;
}

inline uint32_t stringToIP(const std::string& s) {
    unsigned a, b, c, d;
    if (sscanf(s.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return 0;
    return (a << 24) | (b << 16) | (c << 8) | d;
}

inline std::string addrToString(const struct sockaddr_in& addr) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%s:%u",
        inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
    return buf;
}

inline struct sockaddr_in makeAddr(uint32_t ip, uint16_t port) {
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family      = AF_INET;
    a.sin_port        = htons(port);
    a.sin_addr.s_addr = htonl(ip);
    return a;
}

inline struct sockaddr_in makeAddr(const std::string& ip, uint16_t port) {
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port   = htons(port);
    inet_pton(AF_INET, ip.c_str(), &a.sin_addr);
    return a;
}

inline bool setNonBlocking(socket_t fd) {
#ifdef _WIN32
    u_long mode = 1;
    return ioctlsocket(fd, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

inline bool setReuseAddr(socket_t fd) {
    int opt = 1;
    return setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
        reinterpret_cast<const char*>(&opt), sizeof(opt)) == 0;
}

inline bool setTcpNoDelay(socket_t fd) {
    int opt = 1;
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
        reinterpret_cast<const char*>(&opt), sizeof(opt)) == 0;
}

inline uint32_t currentTimeMs() {
    using namespace std::chrono;
    return static_cast<uint32_t>(
        duration_cast<milliseconds>(
            steady_clock::now().time_since_epoch()
        ).count());
}

inline uint32_t extractDstIP(const uint8_t* ipPacket, size_t len) {
    if (len < 20) return 0;
    uint8_t version = (ipPacket[0] >> 4) & 0x0F;
    if (version != 4) return 0;
    return (static_cast<uint32_t>(ipPacket[16]) << 24) |
           (static_cast<uint32_t>(ipPacket[17]) << 16) |
           (static_cast<uint32_t>(ipPacket[18]) << 8)  |
            ipPacket[19];
}

inline uint32_t extractSrcIP(const uint8_t* ipPacket, size_t len) {
    if (len < 20) return 0;
    uint8_t version = (ipPacket[0] >> 4) & 0x0F;
    if (version != 4) return 0;
    return (static_cast<uint32_t>(ipPacket[12]) << 24) |
           (static_cast<uint32_t>(ipPacket[13]) << 16) |
           (static_cast<uint32_t>(ipPacket[14]) << 8)  |
            ipPacket[15];
}

inline bool isBroadcast(uint32_t ip) {
    return ip == 0xFFFFFFFF || (ip & 0x000000FF) == 0xFF;
}

} // namespace VLan
#endif // VLAN_NET_COMMON_H

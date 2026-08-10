#ifndef _WIN32

#include "cli_tun.h"
#include "cli_log.h"
#include <cstring>
#include <cstdio>
#include <cerrno>
#include <algorithm>

#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>
#include <linux/if_tun.h>
#include <linux/rtnetlink.h>
#include <linux/netlink.h>
#include <fcntl.h>
#include <unistd.h>

namespace VLan {

namespace {

class SystemNetlinkTransport : public NetlinkTransport {
public:
    bool transact(const Buffer& request, int* errorCode) override {
        if (errorCode) *errorCode = 0;
        int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
        if (fd < 0) {
            if (errorCode) *errorCode = errno;
            return false;
        }
        struct sockaddr_nl local;
        memset(&local, 0, sizeof(local));
        local.nl_family = AF_NETLINK;
        if (bind(fd, reinterpret_cast<struct sockaddr*>(&local),
                 sizeof(local)) < 0) {
            if (errorCode) *errorCode = errno;
            close(fd);
            return false;
        }
        struct sockaddr_nl kernel;
        memset(&kernel, 0, sizeof(kernel));
        kernel.nl_family = AF_NETLINK;
        ssize_t sent = sendto(fd, request.data(), request.size(), 0,
                              reinterpret_cast<struct sockaddr*>(&kernel),
                              sizeof(kernel));
        if (sent != static_cast<ssize_t>(request.size())) {
            if (errorCode) *errorCode = errno ? errno : EIO;
            close(fd);
            return false;
        }
        uint8_t reply[8192];
        for (;;) {
            ssize_t received = recv(fd, reply, sizeof(reply), 0);
            if (received < 0) {
                if (errno == EINTR) continue;
                if (errorCode) *errorCode = errno;
                close(fd);
                return false;
            }
            for (struct nlmsghdr* header =
                     reinterpret_cast<struct nlmsghdr*>(reply);
                 NLMSG_OK(header, static_cast<unsigned>(received));
                 header = NLMSG_NEXT(header, received)) {
                if (header->nlmsg_type != NLMSG_ERROR) continue;
                const struct nlmsgerr* error =
                    reinterpret_cast<const struct nlmsgerr*>(NLMSG_DATA(header));
                if (error->error == 0) {
                    close(fd);
                    return true;
                }
                if (errorCode) *errorCode = -error->error;
                close(fd);
                return false;
            }
        }
    }
};

SystemNetlinkTransport g_systemNetlink;
uint32_t g_netlinkSequence = 1;

void appendAttribute(Buffer& request, uint16_t type,
                     const void* data, size_t length) {
    const size_t offset = NLMSG_ALIGN(request.size());
    request.resize(offset + RTA_ALIGN(RTA_LENGTH(length)), 0);
    struct rtattr* attribute =
        reinterpret_cast<struct rtattr*>(request.data() + offset);
    attribute->rta_type = type;
    attribute->rta_len = RTA_LENGTH(length);
    memcpy(RTA_DATA(attribute), data, length);
    reinterpret_cast<struct nlmsghdr*>(request.data())->nlmsg_len =
        static_cast<uint32_t>(request.size());
}

bool prefixLength(uint32_t mask, uint8_t* prefix) {
    bool zeroSeen = false;
    uint8_t count = 0;
    for (int bit = 31; bit >= 0; --bit) {
        const bool set = (mask & (static_cast<uint32_t>(1) << bit)) != 0;
        if (set && zeroSeen) return false;
        if (set) ++count;
        else zeroSeen = true;
    }
    *prefix = count;
    return true;
}

Buffer addressRequest(int type, int ifindex, uint32_t ip,
                      uint8_t prefix, bool create) {
    Buffer request(NLMSG_SPACE(sizeof(struct ifaddrmsg)), 0);
    struct nlmsghdr* header = reinterpret_cast<struct nlmsghdr*>(request.data());
    header->nlmsg_len = NLMSG_LENGTH(sizeof(struct ifaddrmsg));
    header->nlmsg_type = static_cast<uint16_t>(type);
    header->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK |
        (create ? (NLM_F_CREATE | NLM_F_REPLACE) : 0);
    header->nlmsg_seq = g_netlinkSequence++;
    struct ifaddrmsg* message =
        reinterpret_cast<struct ifaddrmsg*>(NLMSG_DATA(header));
    message->ifa_family = AF_INET;
    message->ifa_prefixlen = prefix;
    message->ifa_scope = RT_SCOPE_UNIVERSE;
    message->ifa_index = ifindex;
    const uint32_t networkIp = htonl(ip);
    appendAttribute(request, IFA_LOCAL, &networkIp, sizeof(networkIp));
    appendAttribute(request, IFA_ADDRESS, &networkIp, sizeof(networkIp));
    return request;
}

Buffer linkRequest(int ifindex, int mtu) {
    Buffer request(NLMSG_SPACE(sizeof(struct ifinfomsg)), 0);
    struct nlmsghdr* header = reinterpret_cast<struct nlmsghdr*>(request.data());
    header->nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
    header->nlmsg_type = RTM_NEWLINK;
    header->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    header->nlmsg_seq = g_netlinkSequence++;
    struct ifinfomsg* message =
        reinterpret_cast<struct ifinfomsg*>(NLMSG_DATA(header));
    message->ifi_family = AF_UNSPEC;
    message->ifi_index = ifindex;
    message->ifi_flags = IFF_UP;
    message->ifi_change = IFF_UP;
    if (mtu > 0)
        appendAttribute(request, IFLA_MTU, &mtu, sizeof(mtu));
    return request;
}

Buffer routeRequest(int type, int ifindex, bool create) {
    Buffer request(NLMSG_SPACE(sizeof(struct rtmsg)), 0);
    struct nlmsghdr* header = reinterpret_cast<struct nlmsghdr*>(request.data());
    header->nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
    header->nlmsg_type = static_cast<uint16_t>(type);
    header->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK |
        (create ? (NLM_F_CREATE | NLM_F_REPLACE) : 0);
    header->nlmsg_seq = g_netlinkSequence++;
    struct rtmsg* message = reinterpret_cast<struct rtmsg*>(NLMSG_DATA(header));
    message->rtm_family = AF_INET;
    message->rtm_dst_len = 32;
    message->rtm_table = RT_TABLE_MAIN;
    message->rtm_protocol = RTPROT_STATIC;
    message->rtm_scope = RT_SCOPE_LINK;
    message->rtm_type = RTN_UNICAST;
    const uint32_t destination = htonl(0xffffffffu);
    const uint32_t output = static_cast<uint32_t>(ifindex);
    const uint32_t priority = 1;
    appendAttribute(request, RTA_DST, &destination, sizeof(destination));
    appendAttribute(request, RTA_OIF, &output, sizeof(output));
    appendAttribute(request, RTA_PRIORITY, &priority, sizeof(priority));
    return request;
}

} // namespace

void rollbackLinuxTun(NetlinkTransport* transport,
                      LinuxTunConfiguration* state) {
    if (!transport || !state) return;
    if (state->routeConfigured) {
        Buffer request = routeRequest(RTM_DELROUTE, state->ifindex, false);
        transport->transact(request, nullptr);
        state->routeConfigured = false;
    }
    if (state->addressConfigured) {
        Buffer request = addressRequest(RTM_DELADDR, state->ifindex,
                                        state->ip, state->prefix, false);
        transport->transact(request, nullptr);
        state->addressConfigured = false;
    }
}

bool configureLinuxTun(NetlinkTransport* transport, int ifindex,
                       uint32_t ip, uint32_t mask, int mtu,
                       LinuxTunConfiguration* state, int* errorCode) {
    if (errorCode) *errorCode = 0;
    if (!transport || !state || ifindex <= 0) {
        if (errorCode) *errorCode = EINVAL;
        return false;
    }
    *state = LinuxTunConfiguration();
    state->ifindex = ifindex;
    state->ip = ip;
    if (!prefixLength(mask, &state->prefix)) {
        if (errorCode) *errorCode = EINVAL;
        return false;
    }

    Buffer request = addressRequest(RTM_NEWADDR, ifindex, ip,
                                    state->prefix, true);
    if (!transport->transact(request, errorCode)) return false;
    state->addressConfigured = true;

    request = linkRequest(ifindex, mtu);
    if (!transport->transact(request, errorCode)) {
        rollbackLinuxTun(transport, state);
        return false;
    }

    request = routeRequest(RTM_NEWROUTE, ifindex, true);
    if (!transport->transact(request, errorCode)) {
        rollbackLinuxTun(transport, state);
        return false;
    }
    state->routeConfigured = true;
    return true;
}

CliTunAdapter::CliTunAdapter()
    : m_running(false), m_ip(0), m_mask(0), m_tunFd(-1),
      m_interfaceIndex(0), m_addressConfigured(false),
      m_routeConfigured(false), m_netlinkTransport(&g_systemNetlink)
{}

CliTunAdapter::~CliTunAdapter() {
    shutdown();
}

bool CliTunAdapter::initialize(const std::string& adapterName) {
    m_tunFd = open("/dev/net/tun", O_RDWR);
    if (m_tunFd < 0) {
        LOG_ERR("Failed to open /dev/net/tun (need root?)");
        return false;
    }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
    strncpy(ifr.ifr_name, adapterName.c_str(), IFNAMSIZ - 1);

    if (ioctl(m_tunFd, TUNSETIFF, &ifr) < 0) {
        LOG_ERR("ioctl TUNSETIFF failed: %s", strerror(errno));
        close(m_tunFd);
        m_tunFd = -1;
        return false;
    }

    m_interfaceName = ifr.ifr_name;
    m_interfaceIndex = static_cast<int>(if_nametoindex(ifr.ifr_name));
    if (m_interfaceIndex <= 0) {
        LOG_ERR("if_nametoindex failed for %s: %s", ifr.ifr_name, strerror(errno));
        close(m_tunFd);
        m_tunFd = -1;
        return false;
    }

    LOG_INFO("TUN interface created: %s", ifr.ifr_name);
    return true;
}

bool CliTunAdapter::configureIP(uint32_t ip, uint32_t mask, int mtu) {
    m_ip   = ip;
    m_mask = mask;

    if (m_tunFd < 0 || m_interfaceIndex <= 0 || !m_netlinkTransport)
        return false;
    int errorCode = 0;
    LinuxTunConfiguration state;
    if (!configureLinuxTun(m_netlinkTransport, m_interfaceIndex,
                           ip, mask, mtu, &state, &errorCode)) {
        LOG_ERR("TUN rtnetlink configuration failed: %s",
                strerror(errorCode ? errorCode : EIO));
        return false;
    }
    m_addressConfigured = state.addressConfigured;
    m_routeConfigured = state.routeConfigured;
    LOG_INFO("Configured TUN %s address=%s/%u mtu=%d",
             m_interfaceName.c_str(), ipToString(ip).c_str(),
             state.prefix, mtu);
    return true;
}

bool CliTunAdapter::startSession() {
    if (m_tunFd < 0) return false;
    m_running = true;
    m_readThread = std::thread(&CliTunAdapter::readLoop, this);
    return true;
}

void CliTunAdapter::shutdown() {
    m_running = false;
    if (m_readThread.joinable()) m_readThread.join();
    LinuxTunConfiguration state;
    state.ifindex = m_interfaceIndex;
    state.ip = m_ip;
    prefixLength(m_mask, &state.prefix);
    state.addressConfigured = m_addressConfigured;
    state.routeConfigured = m_routeConfigured;
    rollbackLinuxTun(m_netlinkTransport, &state);
    m_addressConfigured = false;
    m_routeConfigured = false;
    if (m_tunFd >= 0) {
        close(m_tunFd);
        m_tunFd = -1;
    }

    m_interfaceIndex = 0;
    m_interfaceName.clear();
}

void CliTunAdapter::readLoop() {
    uint8_t buf[65536];
    while (m_running) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(m_tunFd, &fds);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 500000;

        int ready = select(m_tunFd + 1, &fds, nullptr, nullptr, &tv);
        if (!m_running) break;
        if (ready < 0) {
            if (errno == EINTR) continue;
            m_errorQueue.push(std::string("TUN select failed: ") + strerror(errno));
            break;
        }
        if (ready == 0) continue;

        ssize_t n = read(m_tunFd, buf, sizeof(buf));
        if (n > 0) {
            m_recvQueue.push(Buffer(buf, buf + n));
        } else if (n == 0) {
            m_errorQueue.push("TUN read returned EOF");
            break;
        } else if (errno != EINTR && errno != EAGAIN) {
            m_errorQueue.push(std::string("TUN read failed: ") + strerror(errno));
            break;
        }
    }
    m_running = false;
}

bool CliTunAdapter::writePacket(const uint8_t* data, size_t len) {
    if (m_tunFd < 0) return false;
    std::lock_guard<std::mutex> lock(m_writeMutex);
    ssize_t n = write(m_tunFd, data, len);
    return n == static_cast<ssize_t>(len);
}

} // namespace VLan

#endif // !_WIN32

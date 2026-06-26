#ifndef _WIN32

#include "cli_tun.h"
#include "cli_log.h"
#include <cstring>
#include <cstdio>

#include <sys/ioctl.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <fcntl.h>
#include <unistd.h>

namespace VLan {

CliTunAdapter::CliTunAdapter()
    : m_running(false), m_ip(0), m_mask(0), m_tunFd(-1)
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

    LOG_INFO("TUN interface created: %s", ifr.ifr_name);
    return true;
}

bool CliTunAdapter::configureIP(uint32_t ip, uint32_t mask, int mtu) {
    m_ip   = ip;
    m_mask = mask;

    std::string sIP   = ipToString(ip);
    std::string sMask = ipToString(mask);

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "ip addr add %s/%s dev VLan", sIP.c_str(), sMask.c_str());
    LOG_INFO("Configuring IP: %s", cmd);
    if (system(cmd) != 0) {
        snprintf(cmd, sizeof(cmd), "ip addr flush dev VLan && ip addr add %s/%s dev VLan",
                 sIP.c_str(), sMask.c_str());
        system(cmd);
    }

    system("ip link set VLan up");

    if (mtu > 0) {
        snprintf(cmd, sizeof(cmd), "ip link set VLan mtu %d", mtu);
        system(cmd);
    }

    snprintf(cmd, sizeof(cmd), "ip route add 255.255.255.255/32 dev VLan metric 1 2>/dev/null");
    system(cmd);

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
    if (m_tunFd >= 0) {
        close(m_tunFd);
        m_tunFd = -1;
    }

    system("ip route del 255.255.255.255/32 dev VLan 2>/dev/null");
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
        if (ready <= 0) continue;

        ssize_t n = read(m_tunFd, buf, sizeof(buf));
        if (n > 0) {
            m_recvQueue.push(Buffer(buf, buf + n));
        } else if (n == 0) {
            break;
        }
    }
}

bool CliTunAdapter::writePacket(const uint8_t* data, size_t len) {
    if (m_tunFd < 0) return false;
    std::lock_guard<std::mutex> lock(m_writeMutex);
    ssize_t n = write(m_tunFd, data, len);
    return n == static_cast<ssize_t>(len);
}

} // namespace VLan

#endif // !_WIN32

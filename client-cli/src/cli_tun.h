#ifndef VLAN_CLI_TUN_H
#define VLAN_CLI_TUN_H

#include "cli_common.h"
#include <atomic>
#include <thread>
#include <mutex>
#include <functional>

namespace VLan {

#ifndef _WIN32
class NetlinkTransport {
public:
    virtual ~NetlinkTransport() {}
    virtual bool transact(const Buffer& request, int* errorCode) = 0;
};

struct LinuxTunConfiguration {
    int ifindex;
    uint32_t ip;
    uint8_t prefix;
    bool addressConfigured;
    bool routeConfigured;
    LinuxTunConfiguration()
        : ifindex(0), ip(0), prefix(0),
          addressConfigured(false), routeConfigured(false) {}
};

bool configureLinuxTun(NetlinkTransport* transport, int ifindex,
                       uint32_t ip, uint32_t mask, int mtu,
                       LinuxTunConfiguration* state, int* errorCode);
void rollbackLinuxTun(NetlinkTransport* transport,
                      LinuxTunConfiguration* state);
#endif

/*
 * Cross-platform TUN adapter interface.
 * - Windows: WinTun (wintun.dll)
 * - Linux:   /dev/net/tun (ioctl)
 *
 * The read loop runs in a dedicated thread and pushes received IP
 * packets into a ThreadSafeQueue that the main event loop drains.
 */
class CliTunAdapter {
public:
    CliTunAdapter();
    ~CliTunAdapter();

    bool initialize(const std::string& adapterName = "VLan");
    bool configureIP(uint32_t ip, uint32_t mask, int mtu = 0);
    bool startSession();
    void shutdown();

    bool writePacket(const uint8_t* data, size_t len);
    bool writePacket(const Buffer& pkt) { return writePacket(pkt.data(), pkt.size()); }

    ThreadSafeQueue<Buffer>& recvQueue() { return m_recvQueue; }
    ThreadSafeQueue<std::string>& errorQueue() { return m_errorQueue; }

    bool isRunning() const { return m_running.load(); }
#ifndef _WIN32
    void setNetlinkTransportForTesting(NetlinkTransport* transport) {
        m_netlinkTransport = transport;
    }
#endif

private:
    void readLoop();

    ThreadSafeQueue<Buffer> m_recvQueue;
    ThreadSafeQueue<std::string> m_errorQueue;
    std::atomic<bool>       m_running;
    std::thread             m_readThread;
    std::mutex              m_writeMutex;

    uint32_t m_ip;
    uint32_t m_mask;

#ifdef _WIN32
    void* m_dll;
    void* m_adapter;
    void* m_session;
    void* m_readEvent;

    typedef void* (*FnCreateAdapter)(const wchar_t*, const wchar_t*, const void*);
    typedef void  (*FnCloseAdapter)(void*);
    typedef void* (*FnStartSession)(void*, unsigned long);
    typedef void  (*FnEndSession)(void*);
    typedef void* (*FnGetReadWaitEvent)(void*);
    typedef unsigned char* (*FnReceivePacket)(void*, unsigned long*);
    typedef void  (*FnReleaseReceivePacket)(void*, const unsigned char*);
    typedef unsigned char* (*FnAllocateSendPacket)(void*, unsigned long);
    typedef void  (*FnSendPacket)(void*, const unsigned char*);

    FnCreateAdapter        m_fnCreate;
    FnCloseAdapter         m_fnClose;
    FnStartSession         m_fnStartSession;
    FnEndSession           m_fnEndSession;
    FnGetReadWaitEvent     m_fnGetReadWaitEvent;
    FnReceivePacket        m_fnReceivePacket;
    FnReleaseReceivePacket m_fnReleaseReceivePacket;
    FnAllocateSendPacket   m_fnAllocateSendPacket;
    FnSendPacket           m_fnSendPacket;

    bool m_firewallRuleActive;
    bool m_broadcastRouteActive;
#else
    int m_tunFd;
    std::string m_interfaceName;
    int m_interfaceIndex;
    bool m_addressConfigured;
    bool m_routeConfigured;
    NetlinkTransport* m_netlinkTransport;
#endif
};

} // namespace VLan
#endif // VLAN_CLI_TUN_H

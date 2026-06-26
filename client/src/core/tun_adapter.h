#ifndef VLAN_TUN_ADAPTER_H
#define VLAN_TUN_ADAPTER_H

#include <QThread>
#include <QByteArray>
#include <QString>
#include <QMutex>
#include <atomic>
#include <windows.h>

/* Forward-declare WinTun opaque types */
typedef void* WINTUN_ADAPTER_HANDLE;
typedef void* WINTUN_SESSION_HANDLE;

namespace VLan {

/*
 * WinTun virtual network adapter.
 *
 * Runs a read loop in a dedicated thread; emits packetReceived() for each
 * IP packet captured from the virtual NIC.  Packets written via writePacket()
 * are injected into the virtual NIC so that applications (games) see them.
 *
 * Requires administrator privileges and wintun.dll next to the executable.
 */
class TunAdapter : public QThread {
    Q_OBJECT
public:
    explicit TunAdapter(QObject* parent = nullptr);
    ~TunAdapter();

    bool initialize(const QString& adapterName = QStringLiteral("VLan"));
    bool configureIP(uint32_t ip, uint32_t mask, int mtu = 0);
    bool startSession();
    void shutdown();

    bool writePacket(const QByteArray& packet);

signals:
    void packetReceived(QByteArray packet);
    void errorOccurred(QString message);
    void firewallRuleChanged(bool added, bool success);

protected:
    void run() override;

private:
    bool loadWinTun();
    void unloadWinTun();

    HMODULE              m_dll;
    WINTUN_ADAPTER_HANDLE m_adapter;
    WINTUN_SESSION_HANDLE m_session;
    HANDLE               m_readEvent;
    std::atomic<bool>    m_running;
    bool                 m_firewallRuleActive;
    bool                 m_broadcastRouteActive;
    QMutex               m_writeMutex;

    uint32_t m_ip;
    uint32_t m_mask;

    /* Function pointers loaded from wintun.dll */
    typedef WINTUN_ADAPTER_HANDLE (WINAPI *FnCreateAdapter)(LPCWSTR, LPCWSTR, const GUID*);
    typedef void    (WINAPI *FnCloseAdapter)(WINTUN_ADAPTER_HANDLE);
    typedef WINTUN_SESSION_HANDLE (WINAPI *FnStartSession)(WINTUN_ADAPTER_HANDLE, DWORD);
    typedef void    (WINAPI *FnEndSession)(WINTUN_SESSION_HANDLE);
    typedef HANDLE  (WINAPI *FnGetReadWaitEvent)(WINTUN_SESSION_HANDLE);
    typedef BYTE*   (WINAPI *FnReceivePacket)(WINTUN_SESSION_HANDLE, DWORD*);
    typedef void    (WINAPI *FnReleaseReceivePacket)(WINTUN_SESSION_HANDLE, const BYTE*);
    typedef BYTE*   (WINAPI *FnAllocateSendPacket)(WINTUN_SESSION_HANDLE, DWORD);
    typedef void    (WINAPI *FnSendPacket)(WINTUN_SESSION_HANDLE, const BYTE*);

    FnCreateAdapter        m_fnCreate;
    FnCloseAdapter         m_fnClose;
    FnStartSession         m_fnStartSession;
    FnEndSession           m_fnEndSession;
    FnGetReadWaitEvent     m_fnGetReadWaitEvent;
    FnReceivePacket        m_fnReceivePacket;
    FnReleaseReceivePacket m_fnReleaseReceivePacket;
    FnAllocateSendPacket   m_fnAllocateSendPacket;
    FnSendPacket           m_fnSendPacket;
};

} // namespace VLan
#endif // VLAN_TUN_ADAPTER_H

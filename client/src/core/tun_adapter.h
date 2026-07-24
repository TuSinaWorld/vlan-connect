#ifndef VLAN_TUN_ADAPTER_H
#define VLAN_TUN_ADAPTER_H

#include <QThread>
#include <QByteArray>
#include <QString>
#include <QMutex>
#include <atomic>
#include "wintun_api.h"

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
    TunAdapter(WintunApi* api, QObject* parent);
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

    WintunApi*            m_api;
    bool                  m_ownsApi;
    bool                  m_apiLoaded;
    WINTUN_ADAPTER_HANDLE m_adapter;
    WINTUN_SESSION_HANDLE m_session;
    HANDLE               m_readEvent;
    std::atomic<bool>    m_running;
    std::atomic<bool>    m_acceptIo;
    bool                 m_firewallRuleActive;
    bool                 m_broadcastRouteActive;
    QMutex               m_writeMutex;

    uint32_t m_ip;
    uint32_t m_mask;

};

} // namespace VLan
#endif // VLAN_TUN_ADAPTER_H

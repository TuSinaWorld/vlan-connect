#ifndef VLAN_TUN_ADAPTER_H
#define VLAN_TUN_ADAPTER_H

#include <QThread>
#include <QByteArray>
#include <QString>
#include <QMutex>
#include <atomic>
#include <memory>
#include "room_operation.h"
#include "wintun_api.h"
#include "windows_network_config.h"
#include "windows_network_readiness.h"

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
    bool checkRoute(uint32_t destination, QString* error) const;
    void beginReadiness();
    WindowsReadinessResult pollReadiness();
    void cancelReadiness();
    QString configurationError() const { return m_configurationError; }
    bool startSession();
    void shutdown();
    void setSetupControl(const std::shared_ptr<RoomTaskControl>& control) { m_setupControl = control; }

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
    WindowsNetworkState  m_networkState;
    uint64_t             m_adapterLuid;
    NativeWindowsReadinessBackend m_readinessBackend;
    WindowsNetworkReadiness m_readiness;
    QMutex               m_writeMutex;

    uint32_t m_ip;
    uint32_t m_mask;
    QString m_adapterName;
    QString m_configurationError;
    std::shared_ptr<RoomTaskControl> m_setupControl;

};

} // namespace VLan
#endif // VLAN_TUN_ADAPTER_H

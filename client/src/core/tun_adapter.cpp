#include "tun_adapter.h"
#include "protocol.h"
#include "net_common.h"
#include <QProcess>
#include "../ui/log_manager.h"
#include <cstring>

namespace VLan {

TunAdapter::TunAdapter(QObject* parent)
    : QThread(parent),
      m_dll(nullptr), m_adapter(nullptr), m_session(nullptr),
      m_readEvent(nullptr), m_running(false),
      m_firewallRuleActive(false), m_broadcastRouteActive(false),
      m_ip(0), m_mask(0),
      m_fnCreate(nullptr), m_fnClose(nullptr),
      m_fnStartSession(nullptr), m_fnEndSession(nullptr),
      m_fnGetReadWaitEvent(nullptr), m_fnReceivePacket(nullptr),
      m_fnReleaseReceivePacket(nullptr), m_fnAllocateSendPacket(nullptr),
      m_fnSendPacket(nullptr)
{}

TunAdapter::~TunAdapter() {
    shutdown();
}

bool TunAdapter::loadWinTun() {
    m_dll = LoadLibraryW(L"wintun.dll");
    if (!m_dll) {
        emit errorOccurred("Failed to load wintun.dll");
        return false;
    }

    m_fnCreate             = (FnCreateAdapter)       GetProcAddress(m_dll, "WintunCreateAdapter");
    m_fnClose              = (FnCloseAdapter)         GetProcAddress(m_dll, "WintunCloseAdapter");
    m_fnStartSession       = (FnStartSession)        GetProcAddress(m_dll, "WintunStartSession");
    m_fnEndSession         = (FnEndSession)           GetProcAddress(m_dll, "WintunEndSession");
    m_fnGetReadWaitEvent   = (FnGetReadWaitEvent)    GetProcAddress(m_dll, "WintunGetReadWaitEvent");
    m_fnReceivePacket      = (FnReceivePacket)       GetProcAddress(m_dll, "WintunReceivePacket");
    m_fnReleaseReceivePacket = (FnReleaseReceivePacket) GetProcAddress(m_dll, "WintunReleaseReceivePacket");
    m_fnAllocateSendPacket = (FnAllocateSendPacket)  GetProcAddress(m_dll, "WintunAllocateSendPacket");
    m_fnSendPacket         = (FnSendPacket)           GetProcAddress(m_dll, "WintunSendPacket");

    if (!m_fnCreate || !m_fnClose || !m_fnStartSession || !m_fnEndSession ||
        !m_fnGetReadWaitEvent || !m_fnReceivePacket || !m_fnReleaseReceivePacket ||
        !m_fnAllocateSendPacket || !m_fnSendPacket) {
        emit errorOccurred("wintun.dll: missing exports (wrong version?)");
        FreeLibrary(m_dll);
        m_dll = nullptr;
        return false;
    }
    return true;
}

void TunAdapter::unloadWinTun() {
    if (m_dll) { FreeLibrary(m_dll); m_dll = nullptr; }
}

bool TunAdapter::initialize(const QString& adapterName) {
    if (!loadWinTun()) return false;

    std::wstring wname = adapterName.toStdWString();
    m_adapter = m_fnCreate(wname.c_str(), L"VLan", nullptr);
    if (!m_adapter) {
        emit errorOccurred("WintunCreateAdapter failed (need admin?)");
        return false;
    }
    return true;
}

bool TunAdapter::configureIP(uint32_t ip, uint32_t mask, int mtu) {
    m_ip   = ip;
    m_mask = mask;

    QString sIP   = virtualIPToString(ip);
    QString sMask = virtualIPToString(mask);

    // Use netsh to configure the adapter IP
    int prefixLen = 0;
    uint32_t m = mask;
    while (m & 0x80000000) { ++prefixLen; m <<= 1; }

    QString cmd = QString("netsh interface ip set address name=\"VLan\" "
                          "static %1 %2").arg(sIP).arg(sMask);
    LogManager::instance().logDetail(QString("[TUN] Configuring IP: %1").arg(cmd));

    int ret = QProcess::execute("cmd", QStringList() << "/C" << cmd);
    if (ret != 0) {
        emit errorOccurred("Failed to configure IP on virtual adapter");
        return false;
    }

    // Set metric high so default route stays on physical NIC
    QString metricCmd = QString("netsh interface ip set interface \"VLan\" metric=9999");
    QProcess::execute("cmd", QStringList() << "/C" << metricCmd);

    if (mtu > 0) {
        QString mtuCmd = QString("netsh interface ipv4 set subinterface \"VLan\" mtu=%1 store=active").arg(mtu);
        int mtuRet = QProcess::execute("cmd", QStringList() << "/C" << mtuCmd);
        if (mtuRet == 0)
            LogManager::instance().logDetail(QString("[TUN] MTU set to %1").arg(mtu));
        else
            LogManager::instance().logError(QString("[TUN] Failed to set MTU to %1 (exit code %2)").arg(mtu).arg(mtuRet));
    }

    // Allow all inbound traffic from the virtual subnet through Windows Firewall
    QProcess::execute("netsh", QStringList()
        << "advfirewall" << "firewall" << "delete" << "rule"
        << "name=VLan Virtual LAN");

    int fwRet = -1;
    for (int attempt = 0; attempt < 3; ++attempt) {
        fwRet = QProcess::execute("netsh", QStringList()
            << "advfirewall" << "firewall" << "add" << "rule"
            << "name=VLan Virtual LAN"
            << "dir=in" << "action=allow"
            << "remoteip=10.10.0.0/24"
            << "enable=yes");
        if (fwRet == 0) break;
        LogManager::instance().logError(QString("[TUN] Firewall rule add attempt %1 failed (exit code %2), retrying...").arg(attempt + 1).arg(fwRet));
        Sleep(500);
    }

    if (fwRet == 0) {
        m_firewallRuleActive = true;
        LogManager::instance().logDetail(QString("[TUN] Firewall rule added: allow inbound from 10.10.0.0/24"));
    } else {
        LogManager::instance().logError(QString("[TUN] Failed to add firewall rule after 3 attempts"));
        emit errorOccurred(QString::fromUtf8(
            "防火墙规则添加失败，游戏端口可能无法联通。请检查 Windows 防火墙服务是否正常运行。"));
    }
    emit firewallRuleChanged(true, fwRet == 0);

    // Route limited broadcasts (255.255.255.255) through VLan so that
    // game room discovery via UDP broadcast works across the virtual LAN.
    QProcess::execute("netsh", QStringList()
        << "interface" << "ipv4" << "delete" << "route"
        << "255.255.255.255/32" << "VLan"
        << "store=active");

    int bcastRet = QProcess::execute("netsh", QStringList()
        << "interface" << "ipv4" << "add" << "route"
        << "255.255.255.255/32" << "VLan"
        << sIP << "metric=1" << "store=active");

    if (bcastRet == 0) {
        m_broadcastRouteActive = true;
        LogManager::instance().logDetail(QString("[TUN] Broadcast route added: 255.255.255.255/32 via VLan"));
    } else {
        LogManager::instance().logError(QString("[TUN] Failed to add broadcast route (exit code %1)").arg(bcastRet));
    }

    return true;
}

bool TunAdapter::startSession() {
    if (!m_adapter) return false;

    // Ring buffer capacity: 0x400000 = 4 MB
    m_session = m_fnStartSession(m_adapter, 0x400000);
    if (!m_session) {
        emit errorOccurred("WintunStartSession failed");
        return false;
    }

    m_readEvent = m_fnGetReadWaitEvent(m_session);
    m_running   = true;
    QThread::start();
    return true;
}

void TunAdapter::shutdown() {
    m_running = false;
    if (isRunning()) {
        if (m_readEvent) SetEvent(m_readEvent);
        wait(3000);
    }
    if (m_session) { m_fnEndSession(m_session); m_session = nullptr; }
    if (m_adapter) { m_fnClose(m_adapter);      m_adapter = nullptr; }
    unloadWinTun();

    if (m_broadcastRouteActive) {
        m_broadcastRouteActive = false;
        int brDel = QProcess::execute("netsh", QStringList()
            << "interface" << "ipv4" << "delete" << "route"
            << "255.255.255.255/32" << "VLan"
            << "store=active");
        if (brDel == 0)
            LogManager::instance().logDetail(QString("[TUN] Broadcast route removed"));
        else
            LogManager::instance().logError(QString("[TUN] Failed to remove broadcast route (exit code %1)").arg(brDel));
    }

    if (m_firewallRuleActive) {
        m_firewallRuleActive = false;
        int fwDel = QProcess::execute("netsh", QStringList()
            << "advfirewall" << "firewall" << "delete" << "rule"
            << "name=VLan Virtual LAN");
        if (fwDel == 0)
            LogManager::instance().logDetail(QString("[TUN] Firewall rule removed"));
        else
            LogManager::instance().logError(QString("[TUN] Failed to remove firewall rule (exit code %1)").arg(fwDel));
        emit firewallRuleChanged(false, fwDel == 0);
    }
}

void TunAdapter::run() {
    while (m_running) {
        DWORD waitResult = WaitForSingleObject(m_readEvent, 500);
        if (!m_running) break;
        if (waitResult != WAIT_OBJECT_0) continue;

        DWORD packetSize = 0;
        BYTE* packet = m_fnReceivePacket(m_session, &packetSize);
        while (packet) {
            emit packetReceived(QByteArray(reinterpret_cast<char*>(packet),
                                           static_cast<int>(packetSize)));
            m_fnReleaseReceivePacket(m_session, packet);
            packet = m_fnReceivePacket(m_session, &packetSize);
        }
    }
}

bool TunAdapter::writePacket(const QByteArray& data) {
    if (!m_session) return false;
    QMutexLocker lock(&m_writeMutex);

    BYTE* buf = m_fnAllocateSendPacket(m_session, static_cast<DWORD>(data.size()));
    if (!buf) return false;
    memcpy(buf, data.constData(), data.size());
    m_fnSendPacket(m_session, buf);
    return true;
}

} // namespace VLan

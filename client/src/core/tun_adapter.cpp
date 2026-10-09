#include "tun_adapter.h"
#include "protocol.h"
#include "net_common.h"
#include <netioapi.h>
#include <QProcess>
#include "../ui/log_manager.h"
#include <cstring>

namespace VLan {

TunAdapter::TunAdapter(QObject* parent)
    : TunAdapter(static_cast<WintunApi*>(nullptr), parent)
{}

TunAdapter::TunAdapter(WintunApi* api, QObject* parent)
    : QThread(parent),
      m_api(api ? api : new RealWintunApi()),
      m_ownsApi(api == nullptr),
      m_apiLoaded(false),
      m_adapter(nullptr), m_session(nullptr),
      m_readEvent(nullptr), m_running(false), m_acceptIo(false),
      m_firewallRuleActive(false), m_broadcastRouteActive(false),
      m_ip(0), m_mask(0)
{}

TunAdapter::~TunAdapter() {
    shutdown();
    if (m_ownsApi) {
        delete m_api;
        m_api = nullptr;
    }
}

bool TunAdapter::loadWinTun() {
    if (!m_api) {
        emit errorOccurred("Wintun API is unavailable");
        return false;
    }
    QString error;
    if (!m_api->load(&error)) {
        emit errorOccurred(error.isEmpty()
            ? QStringLiteral("Failed to initialize Wintun API") : error);
        return false;
    }
    m_apiLoaded = true;
    return true;
}

void TunAdapter::unloadWinTun() {
    if (m_api && m_apiLoaded) {
        m_api->unload();
        m_apiLoaded = false;
    }
}

bool TunAdapter::initialize(const QString& adapterName) {
    if (m_adapter || m_session || isRunning())
        return false;
    if (!loadWinTun()) return false;
    m_adapterName = adapterName;

    std::wstring wname = adapterName.toStdWString();
    m_adapter = m_api->createAdapter(wname.c_str(), L"VLan", nullptr);
    if (!m_adapter) {
        emit errorOccurred("WintunCreateAdapter failed (need admin?)");
        unloadWinTun();
        return false;
    }
    return true;
}

bool TunAdapter::configureIP(uint32_t ip, uint32_t mask, int mtu) {
    m_ip   = ip;
    m_mask = mask;
    m_configurationError.clear();
    const auto fail = [this](const QString& message) {
        m_configurationError = message;
        emit errorOccurred(message);
        return false;
    };

    QString sIP   = virtualIPToString(ip);
    QString sMask = virtualIPToString(mask);

    LogManager::instance().logNormal(QString("[TUN] Configuring adapter=%1 IP=%2 mask=%3 MTU=%4")
                                    .arg(m_adapterName).arg(sIP).arg(sMask).arg(mtu));
    int ret = QProcess::execute("netsh", QStringList()
        << "interface" << "ipv4" << "set" << "address"
        << ("name=" + m_adapterName) << "static" << sIP << sMask
        << "gateway=none" << "store=active");
    if (ret != 0) {
        return fail(QString("Failed to configure virtual adapter IP (netsh exit %1)").arg(ret));
    }

    // Windows adds the interface metric to the route metric. There is no
    // default gateway on this adapter; metric=9999 only made overlay and
    // limited broadcast routes lose to physical/VPN interfaces.
    ret = QProcess::execute("netsh", QStringList()
        << "interface" << "ipv4" << "set" << "interface"
        << ("interface=" + m_adapterName) << "metric=5" << "store=active");
    if (ret != 0)
        return fail(QString("Failed to configure virtual adapter metric (netsh exit %1)").arg(ret));

    if (mtu > 0) {
        int mtuRet = QProcess::execute("netsh", QStringList()
            << "interface" << "ipv4" << "set" << "subinterface"
            << m_adapterName << QString("mtu=%1").arg(mtu) << "store=active");
        if (mtuRet == 0)
            LogManager::instance().logDetail(QString("[TUN] MTU set to %1").arg(mtu));
        else
            return fail(QString("Failed to set virtual adapter MTU=%1 (netsh exit %2)").arg(mtu).arg(mtuRet));
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
    }
    emit firewallRuleChanged(true, fwRet == 0);
    if (fwRet != 0)
        return fail(QString::fromUtf8(
            "防火墙规则添加失败，已取消入房。请检查 Windows 防火墙服务及管理员权限。"));

    // Route limited broadcasts (255.255.255.255) through VLan so that
    // game room discovery via UDP broadcast works across the virtual LAN.
    QProcess::execute("netsh", QStringList()
        << "interface" << "ipv4" << "delete" << "route"
        << "255.255.255.255/32" << m_adapterName
        << "store=active");

    int bcastRet = QProcess::execute("netsh", QStringList()
        << "interface" << "ipv4" << "add" << "route"
        << "255.255.255.255/32" << m_adapterName
        << "0.0.0.0" << "metric=1" << "store=active");

    if (bcastRet == 0) {
        m_broadcastRouteActive = true;
        LogManager::instance().logDetail(QString("[TUN] Broadcast route added: 255.255.255.255/32 via VLan"));
    } else {
        return fail(QString("Failed to add virtual LAN broadcast route (netsh exit %1)").arg(bcastRet));
    }

    return true;
}

bool TunAdapter::checkRoute(uint32_t destination, QString* error) const {
    NET_LUID luid = {};
    const std::wstring name = m_adapterName.toStdWString();
    DWORD result = ConvertInterfaceAliasToLuid(name.c_str(), &luid);
    SOCKADDR_INET target = {};
    target.Ipv4.sin_family = AF_INET;
    target.Ipv4.sin_addr.s_addr = htonl(destination);
    MIB_IPFORWARD_ROW2 route = {};
    SOCKADDR_INET source = {};
    if (result == NO_ERROR) {
        // Wintun must have an active session. Address DAD and media-up can
        // complete after netsh returns; allow a bounded readiness interval.
        for (int attempt = 0; attempt < 60; ++attempt) {
            result = GetBestRoute2(nullptr, 0, nullptr, &target, 0, &route, &source);
            if (result == NO_ERROR && route.InterfaceLuid.Value == luid.Value &&
                source.Ipv4.sin_addr.s_addr == htonl(m_ip))
                return true;
            Sleep(50);
        }
    }
    if (error) {
        *error = QString::fromUtf8(
            "虚拟局域网路由冲突或不可用：目标 %1，所选接口 %2，源地址 %3，Windows 错误 %4。请检查 VPN/其它网卡的 10.10.0.0/24 路由。")
            .arg(virtualIPToString(destination)).arg(route.InterfaceIndex)
            .arg(virtualIPToString(ntohl(source.Ipv4.sin_addr.s_addr))).arg(result);
    }
    return false;
}

bool TunAdapter::startSession() {
    QMutexLocker lock(&m_writeMutex);
    if (!m_api || !m_adapter || m_session || isRunning()) {
        return false;
    }

    // Ring buffer capacity: 0x400000 = 4 MB
    m_session = m_api->startSession(m_adapter, 0x400000);
    if (!m_session) {
        emit errorOccurred("WintunStartSession failed");
        return false;
    }

    m_readEvent = m_api->getReadWaitEvent(m_session);
    if (!m_readEvent) {
        emit errorOccurred("WintunGetReadWaitEvent failed");
        m_api->endSession(m_session);
        m_session = nullptr;
        return false;
    }
    m_acceptIo = true;
    m_running   = true;
    QThread::start();
    return true;
}

void TunAdapter::shutdown() {
    m_acceptIo = false;
    m_running = false;
    HANDLE readEvent = m_readEvent;
    if (readEvent)
        SetEvent(readEvent);
    if (isRunning()) {
        if (!wait(3000)) {
            LogManager::instance().logError(
                "[TUN] Read thread did not stop within 3 seconds; waiting for a safe shutdown");
            if (readEvent)
                SetEvent(readEvent);
            wait();
        }
    }

    {
        QMutexLocker lock(&m_writeMutex);
        if (m_session && m_api) {
            m_api->endSession(m_session);
            m_session = nullptr;
        }
        m_readEvent = nullptr;
        if (m_adapter && m_api) {
            m_api->closeAdapter(m_adapter);
            m_adapter = nullptr;
        }
        unloadWinTun();
    }

    if (m_broadcastRouteActive) {
        m_broadcastRouteActive = false;
        int brDel = QProcess::execute("netsh", QStringList()
            << "interface" << "ipv4" << "delete" << "route"
            << "255.255.255.255/32" << m_adapterName
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
    while (m_running.load()) {
        DWORD waitResult = WaitForSingleObject(m_readEvent, 500);
        if (!m_running.load()) break;
        if (waitResult == WAIT_TIMEOUT) continue;
        if (waitResult == WAIT_FAILED) {
            emit errorOccurred(QString("Wintun read wait failed (error %1)")
                               .arg(GetLastError()));
            break;
        }
        if (waitResult != WAIT_OBJECT_0) {
            emit errorOccurred(QStringLiteral("Wintun read session became invalid"));
            break;
        }

        while (m_running.load() && m_acceptIo.load()) {
            DWORD packetSize = 0;
            BYTE* packet = m_api
                ? m_api->receivePacket(m_session, &packetSize) : nullptr;
            if (!packet) {
                const DWORD error = GetLastError();
                if (error != ERROR_NO_MORE_ITEMS) {
                    emit errorOccurred(QString("WintunReceivePacket failed (error %1)")
                                       .arg(error));
                    m_running = false;
                }
                break;
            }
            QByteArray received(reinterpret_cast<char*>(packet),
                                static_cast<int>(packetSize));
            m_api->releaseReceivePacket(m_session, packet);
            if (!m_running.load() || !m_acceptIo.load())
                break;
            emit packetReceived(received);
        }
    }
    m_acceptIo = false;
    m_running = false;
}

bool TunAdapter::writePacket(const QByteArray& data) {
    QMutexLocker lock(&m_writeMutex);
    if (!m_acceptIo.load() || !m_running.load() ||
        !m_api || !m_session) {
        return false;
    }

    BYTE* buf = m_api->allocateSendPacket(
        m_session, static_cast<DWORD>(data.size()));
    if (!buf) return false;
    memcpy(buf, data.constData(), data.size());
    m_api->sendPacket(m_session, buf);
    return true;
}

} // namespace VLan

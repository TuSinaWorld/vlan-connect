#include "tun_adapter.h"
#include "protocol.h"
#include "net_common.h"
#include <netioapi.h>

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
    m_ip = ip;
    m_mask = mask;
    m_configurationError.clear();
    m_networkState.alias = m_adapterName.toStdWString();
    NET_LUID luid = {};
    if (m_api && m_adapter && m_api->getAdapterLuid(m_adapter, &luid)) {
        wchar_t alias[IF_MAX_STRING_SIZE + 1] = {};
        const DWORD result = ConvertInterfaceLuidToAlias(&luid, alias, IF_MAX_STRING_SIZE + 1);
        if (result != NO_ERROR) {
            m_configurationError = QString("Cannot resolve virtual adapter alias (Windows %1)").arg(result);
            emit errorOccurred(m_configurationError);
            return false;
        }
        m_networkState.alias = alias;
        m_adapterName = QString::fromStdWString(m_networkState.alias);
    }
    SystemWindowsNetworkCommands commands;
    std::string error;
    const bool configured = configureWindowsNetwork(commands, m_networkState, ip, mask, mtu,
        [](const std::string& message) {
            LogManager::instance().logNormal(QString::fromUtf8(message.data(), static_cast<int>(message.size())));
        }, &error);
    emit firewallRuleChanged(true, m_networkState.firewallActive);
    if (!configured) {
        m_configurationError = QString::fromUtf8(error.data(), static_cast<int>(error.size()));
        emit errorOccurred(m_configurationError);
    }
    return configured;
}

bool TunAdapter::checkRoute(uint32_t destination, QString* error) const {
    std::string detail;
    const bool ok = checkWindowsOverlayRoute(m_networkState.alias, m_ip, destination, &detail);
    if (error) *error = QString::fromUtf8(detail.data(), static_cast<int>(detail.size()));
    LogManager::instance().logNormal(ok
        ? QString("[network] route-check destination=%1 alias=%2 result=success")
            .arg(virtualIPToString(destination)).arg(m_adapterName)
        : QString::fromUtf8(detail.data(), static_cast<int>(detail.size())));
    return ok;
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

    SystemWindowsNetworkCommands commands;
    const bool hadFirewall = m_networkState.firewallActive;
    rollbackWindowsNetwork(commands, m_networkState, [](const std::string& message) {
        LogManager::instance().logNormal(QString::fromUtf8(message.data(), static_cast<int>(message.size())));
    });
    if (hadFirewall)
        emit firewallRuleChanged(false, !m_networkState.firewallActive);

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

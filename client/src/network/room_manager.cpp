#include "room_manager.h"
#include "signal_client.h"
#include "data_channel.h"
#include "net_common.h"
#include "payload_cipher.h"
#include "../core/tunnel_manager.h"
#include "../core/tun_adapter.h"
#include "../core/kcp_tunnel.h"
#include "../core/raw_udp_tunnel.h"
#include "../core/peer_connection.h"
#include "../ui/log_manager.h"
#include "../ui/ui_strings.h"
#include <QTimer>

namespace VLan {

namespace {

QByteArray roomPasswordHash(const QString& password) {
    if (password.isEmpty())
        return QByteArray();
    QByteArray intermediate = PayloadCipher::computeIntermediate(password);
    QByteArray hash = PayloadCipher::hashFromIntermediate(intermediate);
    crypto_wipe(reinterpret_cast<uint8_t*>(intermediate.data()),
                static_cast<size_t>(intermediate.size()));
    return hash;
}

const char* trafficClassName(TrafficClass cls) {
    return cls == TRAFFIC_TCP ? "TCP" : "UDP";
}

bool samePolicy(const RoomTrafficPolicy& a, const RoomTrafficPolicy& b) {
    return a.transportMode == b.transportMode &&
           a.fecMode == b.fecMode &&
           a.kcpProfile == b.kcpProfile;
}

bool deadlineReached(uint32_t deadlineMs) {
    return deadlineMs != 0 &&
           static_cast<int32_t>(currentTimeMs() - deadlineMs) >= 0;
}

} // namespace

RoomManager::RoomManager(QObject* parent)
    : QObject(parent),
      m_signal(nullptr),
      m_dataChannel(nullptr),
      m_tunnel(nullptr),
      m_tun(nullptr),
      m_port(DEFAULT_PORT),
      m_tcpRelayTimer(nullptr), m_latencyTimer(nullptr), m_trafficTimer(nullptr),
      m_roomListTimer(nullptr),
      m_lastUploadBytes(0), m_lastDownloadBytes(0),
      m_currentRoomId(0), m_myVirtualIP(0),
      m_tcpPolicy(makeDefaultTcpPolicy()),
      m_udpPolicy(makeDefaultUdpPolicy()),
      m_roomMtu(ROOM_MTU_DEFAULT),
      m_roomPasswordProtected(false),
      m_wantReconnect(false), m_reconnectAttempts(0), m_wasInRoom(false),
      m_pendingResumeRoom(false), m_manualDisconnecting(false),
      m_logoutPending(false), m_hasResumeLease(false),
      m_resumeRoomId(0), m_resumePeerId(0), m_resumeVirtualIP(0),
      m_resumeLeaseDeadlineMs(0),
      m_savedRoomId(0),
      m_savedMaxPlayers(8), m_savedRoomMtu(ROOM_MTU_DEFAULT),
      m_savedRoomPasswordProtected(false),
      m_savedTcpPolicy(makeDefaultTcpPolicy()),
      m_savedUdpPolicy(makeDefaultUdpPolicy())
{
    m_signal = new SignalClient(this);
    m_tunnel = new TunnelManager(this);

    connect(m_signal, &SignalClient::connected,     this, &RoomManager::onSignalConnected);
    connect(m_signal, &SignalClient::disconnected,  this, &RoomManager::onSignalDisconnected);
    connect(m_signal, &SignalClient::loginResponse, this, &RoomManager::onLoginResponse);
    connect(m_signal, &SignalClient::roomCreated,   this, &RoomManager::onRoomCreated);
    connect(m_signal, &SignalClient::joinResponse,  this, &RoomManager::onJoinResponse);
    connect(m_signal, &SignalClient::peerJoined,    this, &RoomManager::onPeerJoined);
    connect(m_signal, &SignalClient::peerResumed,   this, &RoomManager::onPeerResumed);
    connect(m_signal, &SignalClient::peerLeft,      this, &RoomManager::onPeerLeft);
    connect(m_signal, &SignalClient::relayReady,    this, &RoomManager::onRelayReady);
    connect(m_signal, &SignalClient::logoutAck,     this, &RoomManager::onLogoutAck);
    connect(m_signal, &SignalClient::serverPasswordRequired,
            this, &RoomManager::serverPasswordRequired);
    connect(m_signal, &SignalClient::secureSessionEstablished,
            this, &RoomManager::onSecureSessionEstablished);
    connect(m_signal, &SignalClient::serverError, this, [this](const QString& msg) {
        emit errorOccurred(msg);
        if (m_pendingResumeRoom) {
            m_pendingResumeRoom = false;
            clearResumeLease();
            if (m_wasInRoom && !m_savedRoomName.isEmpty()) {
                m_wantReconnect = true;
                m_signal->listRooms();
                return;
            }
        }
        if (m_signal->myPeerId() == 0) {
            if (m_wantReconnect) {
                m_signal->disconnect();
            } else {
                QTimer::singleShot(0, this, [this]() { disconnectFromServer(); });
            }
        }
    });
    connect(m_signal, &SignalClient::roomList, this,
            [this](QList<VLan::RoomListItem> rooms) {
        m_cachedRoomList = rooms;
        if (!m_pendingResumeRoom &&
            m_wantReconnect && m_wasInRoom && !m_savedRoomName.isEmpty()) {
            handleReconnectRoomList(rooms);
            return;
        }
        emit roomListUpdated(rooms);
    });
    connect(m_signal, &SignalClient::serverRttUpdated,
            this, &RoomManager::serverRttUpdated);

    connect(m_tunnel, &TunnelManager::transportDead,
            this, &RoomManager::onTransportDead);

    m_latencyTimer = new QTimer(this);
    connect(m_latencyTimer, &QTimer::timeout, this, &RoomManager::onLatencyUpdate);

    m_trafficTimer = new QTimer(this);
    connect(m_trafficTimer, &QTimer::timeout, this, &RoomManager::onTrafficUpdate);

    m_roomListTimer = new QTimer(this);
    m_roomListTimer->setInterval(15000);
    connect(m_roomListTimer, &QTimer::timeout, this, &RoomManager::onRoomListRefreshTimer);
}

RoomManager::~RoomManager() {
    teardownTun();
}

void RoomManager::setServerAddress(const QString& host, quint16 port) {
    quint16 normalizedPort = port == 0 ? DEFAULT_PORT : port;
    bool changed = (m_serverHost != host || m_port != normalizedPort);
    m_serverHost = host;
    m_port = normalizedPort;
    m_resolvedAddr = QHostAddress();
    if (changed) {
        m_serverPassword.clear();
        clearResumeLease();
        if (m_signal)
            m_signal->setServerPassword(QString());
    }
}

void RoomManager::setServerPassword(const QString& password) {
    m_serverPassword = password;
    if (m_signal)
        m_signal->setServerPassword(password);
}

void RoomManager::continueServerAuth() {
    if (m_signal)
        m_signal->continueServerAuth();
}

void RoomManager::connectAndLogin(const QString& playerName) {
    QString previousName = m_playerName;
    m_playerName = playerName;
    if (!m_wantReconnect) {
        expireResumeLeaseIfNeeded();
        if (!(m_wasInRoom && hasUsableResumeLease() && previousName == playerName))
            clearResumeLease();
    }
    if (m_tunnel)
        m_tunnel->setSecureSession(0, QByteArray());
    if (m_dataChannel) {
        m_dataChannel->disconnect();
        m_dataChannel->setSecureSession(0, QByteArray());
    }
    if (m_signal)
        m_signal->setServerPassword(m_serverPassword);
    emit statusMessage(UiStrings::text("status.connectingServer").arg(m_serverHost).arg(m_port));
    resolveAndConnect();
}

void RoomManager::disconnectFromServer() {
    m_wantReconnect = false;
    m_pendingResumeRoom = false;
    clearResumeLease();
    m_playerName.clear();
    m_serverPassword.clear();
    if (m_dataChannel) {
        m_dataChannel->disconnect();
        m_dataChannel->setSecureSession(0, QByteArray());
    }
    if (m_tunnel)
        m_tunnel->setSecureSession(0, QByteArray());
    if (!m_signal)
        return;

    m_signal->setServerPassword(QString());
    if (m_signal->isConnected() && m_signal->myPeerId() != 0) {
        if (!m_logoutPending) {
            m_manualDisconnecting = true;
            m_logoutPending = true;
            m_signal->logout();
            QTimer::singleShot(1000, this, [this]() {
                if (m_logoutPending)
                    finishManualDisconnect();
            });
        }
        return;
    }

    m_manualDisconnecting = true;
    finishManualDisconnect();
}

void RoomManager::resolveAndConnect() {
    QHostAddress directAddr(m_serverHost);
    if (!directAddr.isNull()) {
        m_resolvedAddr = directAddr;
        proceedWithConnection();
        return;
    }
    emit statusMessage(UiStrings::text("status.resolvingHost").arg(m_serverHost));
    QHostInfo::lookupHost(m_serverHost, this, SLOT(onHostResolved(QHostInfo)));
}

void RoomManager::onHostResolved(const QHostInfo& hostInfo) {
    if (hostInfo.error() != QHostInfo::NoError || hostInfo.addresses().isEmpty()) {
        emit errorOccurred(UiStrings::text("status.resolveFailed").arg(hostInfo.errorString()));
        emit connectionStatusChanged(false);
        if (m_wantReconnect)
            scheduleReconnectAttempt();
        return;
    }
    for (const QHostAddress& addr : hostInfo.addresses()) {
        if (addr.protocol() == QAbstractSocket::IPv4Protocol) {
            m_resolvedAddr = addr;
            break;
        }
    }
    if (m_resolvedAddr.isNull())
        m_resolvedAddr = hostInfo.addresses().first();

    emit statusMessage(UiStrings::text("status.resolvedHost")
                       .arg(m_serverHost).arg(m_resolvedAddr.toString()));
    proceedWithConnection();
}

void RoomManager::proceedWithConnection() {
    m_tunnel->setServerEndpoint(m_resolvedAddr, m_port);
    m_signal->setServerPassword(m_serverPassword);
    m_signal->connectToServer(m_serverHost, m_port);
}

uint32_t RoomManager::myPeerId() const {
    return m_signal->myPeerId();
}

void RoomManager::rememberResumeLease(uint32_t roomId, uint32_t peerId,
                                      uint32_t virtualIP, const QByteArray& token) {
    if (token.size() != RECONNECT_TOKEN_SIZE) {
        clearResumeLease();
        return;
    }
    m_hasResumeLease = true;
    m_resumeRoomId = roomId;
    m_resumePeerId = peerId;
    m_resumeVirtualIP = virtualIP;
    m_resumeLeaseDeadlineMs = 0;
    m_resumeToken = token;
}

void RoomManager::clearResumeLease() {
    m_hasResumeLease = false;
    m_resumeRoomId = 0;
    m_resumePeerId = 0;
    m_resumeVirtualIP = 0;
    m_resumeLeaseDeadlineMs = 0;
    if (!m_resumeToken.isEmpty()) {
        crypto_wipe(reinterpret_cast<uint8_t*>(m_resumeToken.data()),
                    static_cast<size_t>(m_resumeToken.size()));
        m_resumeToken.clear();
    }
}

void RoomManager::startResumeLeaseDeadline() {
    if (!m_hasResumeLease || m_resumeToken.size() != RECONNECT_TOKEN_SIZE)
        return;
    m_resumeLeaseDeadlineMs = currentTimeMs() +
        static_cast<uint32_t>(RECONNECT_LEASE_TIMEOUT_SEC * 1000);
    QTimer::singleShot(RECONNECT_LEASE_TIMEOUT_SEC * 1000, this, [this]() {
        expireResumeLeaseIfNeeded();
    });
}

void RoomManager::expireResumeLeaseIfNeeded() {
    if (deadlineReached(m_resumeLeaseDeadlineMs))
        clearResumeLease();
}

bool RoomManager::hasUsableResumeLease() {
    expireResumeLeaseIfNeeded();
    return m_hasResumeLease &&
           m_resumeToken.size() == RECONNECT_TOKEN_SIZE &&
           !deadlineReached(m_resumeLeaseDeadlineMs);
}

// Room operations

void RoomManager::createRoom(const QString& roomName, uint8_t maxPlayers,
                             RoomTrafficPolicy tcpPolicy,
                             RoomTrafficPolicy udpPolicy,
                             uint16_t mtu,
                             bool passwordProtected,
                             const QString& password) {
    uint16_t normalizedMtu = normalizeRoomMtu(mtu);
    m_savedRoomName = roomName;
    m_savedMaxPlayers = maxPlayers;
    m_savedRoomMtu = normalizedMtu;
    m_savedTcpPolicy = normalizeTrafficPolicy(
        tcpPolicy.transportMode, tcpPolicy.fecMode, tcpPolicy.kcpProfile,
        makeDefaultTcpPolicy());
    m_savedUdpPolicy = normalizeTrafficPolicy(
        udpPolicy.transportMode, udpPolicy.fecMode, udpPolicy.kcpProfile,
        makeDefaultUdpPolicy());
    m_savedRoomPasswordProtected = passwordProtected;
    m_savedRoomPassword = passwordProtected ? password : QString();

    m_roomMtu = normalizedMtu;
    m_tcpPolicy = m_savedTcpPolicy;
    m_udpPolicy = m_savedUdpPolicy;
    m_roomPasswordProtected = passwordProtected;
    m_roomPassword = m_savedRoomPassword;

    QByteArray pwdHash = passwordProtected ? roomPasswordHash(password) : QByteArray();
    m_signal->createRoom(roomName, maxPlayers, m_tcpPolicy, m_udpPolicy,
                         normalizedMtu, passwordProtected, pwdHash);
}

void RoomManager::joinRoom(uint32_t roomId, const QString& password) {
    for (int i = 0; i < m_cachedRoomList.size(); ++i) {
        if (m_cachedRoomList[i].roomId == roomId) {
            const RoomListItem& item = m_cachedRoomList[i];
            m_savedRoomId = item.roomId;
            m_savedRoomName = QString::fromUtf8(item.roomName);
            m_savedMaxPlayers = item.maxPlayers;
            m_savedRoomMtu = normalizeRoomMtu(item.mtu);
            m_savedTcpPolicy = item.tcpPolicy;
            m_savedUdpPolicy = item.udpPolicy;
            m_savedRoomPasswordProtected = item.passwordProtected != 0;
            m_roomMtu = m_savedRoomMtu;
            m_tcpPolicy = item.tcpPolicy;
            m_udpPolicy = item.udpPolicy;
            break;
        }
    }
    m_roomPassword = password;
    m_savedRoomPassword = password;
    QByteArray authHash = roomPasswordHash(password);
    m_signal->joinRoom(roomId, authHash);
}

void RoomManager::leaveRoom() {
    m_signal->leaveRoom();
    teardownTun();
    m_pendingResumeRoom = false;
    clearResumeLease();
    m_currentRoomId = 0;
    m_myVirtualIP = 0;
    m_roomMtu = ROOM_MTU_DEFAULT;
    m_tcpPolicy = makeDefaultTcpPolicy();
    m_udpPolicy = makeDefaultUdpPolicy();
    m_roomPasswordProtected = false;
    m_roomPassword.clear();
    m_savedRoomId = 0;
    m_savedRoomName.clear();
    m_savedRoomPassword.clear();
    m_savedRoomPasswordProtected = false;
    m_wasInRoom = false;
    emit roomLeft();
}

void RoomManager::refreshRoomList() {
    m_signal->listRooms();
}

void RoomManager::onRoomListRefreshTimer() {
    if (m_signal && m_signal->isConnected() && m_signal->myPeerId() != 0)
        refreshRoomList();
}

// Signal slots

void RoomManager::onSignalConnected() {
    emit statusMessage(UiStrings::text("status.connectedLoggingIn"));
    emit connectionStatusChanged(true);
    bool canResume = m_wasInRoom && hasUsableResumeLease();
    m_signal->login(m_playerName, canResume,
                    canResume ? m_resumeRoomId : 0,
                    canResume ? m_resumePeerId : 0,
                    canResume ? m_resumeToken : QByteArray());
}

void RoomManager::onSignalDisconnected() {
    bool wasInRoom = (m_currentRoomId != 0);
    bool manualDisconnect = m_manualDisconnecting;

    if (manualDisconnect) {
        m_wasInRoom = false;
        m_savedRoomId = 0;
        m_savedRoomName.clear();
        m_savedRoomPassword.clear();
        m_savedRoomPasswordProtected = false;
    } else if (!m_wantReconnect) {
        if (wasInRoom) {
            m_wasInRoom = true;
            m_savedRoomId = m_currentRoomId;
            m_savedTcpPolicy = m_tcpPolicy;
            m_savedUdpPolicy = m_udpPolicy;
            m_savedRoomMtu = m_roomMtu;
            m_savedRoomPasswordProtected = m_roomPasswordProtected;
            m_savedRoomPassword = m_roomPassword;
        } else {
            m_wasInRoom = false;
        }
    }
    if (!manualDisconnect && wasInRoom && m_hasResumeLease)
        startResumeLeaseDeadline();

    teardownTun();
    if (m_tunnel)
        m_tunnel->setSecureSession(0, QByteArray());
    if (m_roomListTimer)
        m_roomListTimer->stop();
    m_currentRoomId = 0;
    m_myVirtualIP = 0;
    m_roomMtu = ROOM_MTU_DEFAULT;
    m_logoutPending = false;
    if (wasInRoom) emit roomLeft();
    emit connectionStatusChanged(false);

    if (manualDisconnect) {
        m_manualDisconnecting = false;
        emit statusMessage(UiStrings::text("status.disconnected"));
    } else if (!m_wantReconnect && !m_playerName.isEmpty()) {
        m_wantReconnect = true;
        m_reconnectAttempts = 0;
        emit statusMessage(UiStrings::text("status.disconnectedReconnect")
                           .arg(RECONNECT_INTERVAL_MS / 1000)
                           .arg(MAX_RECONNECT_ATTEMPTS)
                           .arg(m_wasInRoom ? UiStrings::text("status.rejoinSuffix") : QString()));
        scheduleReconnectAttempt();
    } else if (m_wantReconnect) {
        scheduleReconnectAttempt();
    } else {
        emit statusMessage(UiStrings::text("status.disconnected"));
    }
}

void RoomManager::scheduleReconnectAttempt() {
    QTimer::singleShot(RECONNECT_INTERVAL_MS, this, [this]() {
        if (m_signal->isConnected() || m_signal->isConnecting()
            || m_playerName.isEmpty() || !m_wantReconnect) {
            return;
        }
        if (m_reconnectAttempts >= MAX_RECONNECT_ATTEMPTS) {
            m_wantReconnect = false;
            m_pendingResumeRoom = false;
            expireResumeLeaseIfNeeded();
            if (m_wasInRoom && hasUsableResumeLease()) {
                emit statusMessage(UiStrings::text("status.reconnectStoppedResumeWindow")
                                   .arg(MAX_RECONNECT_ATTEMPTS)
                                   .arg(RECONNECT_LEASE_TIMEOUT_SEC));
            } else {
                emit statusMessage(UiStrings::text("status.reconnectFailed")
                                   .arg(MAX_RECONNECT_ATTEMPTS));
            }
            return;
        }
        m_reconnectAttempts++;
        emit statusMessage(UiStrings::text("status.reconnectAttempt")
                           .arg(m_reconnectAttempts).arg(MAX_RECONNECT_ATTEMPTS));
        resolveAndConnect();
    });
}

void RoomManager::onLogoutAck() {
    if (m_logoutPending)
        finishManualDisconnect();
}

void RoomManager::finishManualDisconnect() {
    m_logoutPending = false;
    if (m_signal && (m_signal->isConnected() || m_signal->isConnecting()))
        m_signal->disconnect();
}

void RoomManager::rebuildPeerTransports(uint32_t peerId) {
    PeerConnection* peer = m_tunnel->peerById(peerId);
    if (!peer) return;

    clearPendingRebuild(peerId);
    for (int cls = TRAFFIC_TCP; cls <= TRAFFIC_UDP; ++cls) {
        TrafficClass trafficClass = static_cast<TrafficClass>(cls);
        if (peer->transport(trafficClass) == TRANSPORT_NONE)
            continue;
        m_tunnel->removeTransport(peerId, trafficClass);
        emit peerTransportChanged(peerId, trafficClass, TRANSPORT_NONE);
    }

    if (m_signal && m_signal->isConnected())
        m_signal->requestRelay(peerId);
}

void RoomManager::onSecureSessionEstablished(uint32_t sessionId, QByteArray master) {
    if (m_tunnel)
        m_tunnel->setSecureSession(sessionId, master);
    if (m_dataChannel)
        m_dataChannel->setSecureSession(sessionId, master);
}

void RoomManager::onLoginResponse(uint32_t peerId, bool resumeAccepted) {
    emit statusMessage(UiStrings::text("status.loginSuccess").arg(peerId));
    emit loggedIn(peerId);
    m_tunnel->setMyPeerId(peerId);
    if (m_roomListTimer)
        m_roomListTimer->start();
    if (m_signal->secureEnabled())
        m_tunnel->setSecureSession(m_signal->secureSessionId(), m_signal->secureMaster());

    bool canResume = m_wasInRoom && hasUsableResumeLease();
    if (m_wantReconnect || canResume) {
        if (resumeAccepted && canResume) {
            m_pendingResumeRoom = true;
            m_signal->resumeRoom(m_resumeRoomId, m_resumePeerId, m_resumeToken);
        } else if (m_wasInRoom && !m_savedRoomName.isEmpty()) {
            if (canResume && !resumeAccepted)
                clearResumeLease();
            m_wantReconnect = true;
            emit statusMessage(UiStrings::text("status.reconnectFindingRoom")
                               .arg(m_savedRoomName));
            m_signal->listRooms();
        } else {
            m_wantReconnect = false;
            emit statusMessage(UiStrings::text("status.reconnectSuccess"));
        }
    }

    if (!m_dataChannel) {
        m_dataChannel = new DataChannel(this);
        connect(m_dataChannel, &DataChannel::connected,
                this, &RoomManager::onDataChannelConnected);
        connect(m_dataChannel, &DataChannel::disconnected,
                this, &RoomManager::onDataChannelDisconnected);
        connect(m_dataChannel, &DataChannel::relayDataReceived,
                this, &RoomManager::onDataChannelRelayReceived);
    }
    if (m_signal->secureEnabled())
        m_dataChannel->setSecureSession(m_signal->secureSessionId(), m_signal->secureMaster());
    m_dataChannel->connectToServer(m_serverHost, m_port, peerId);
}

void RoomManager::onRoomCreated(uint32_t roomId, uint32_t virtualIP,
                                RoomTrafficPolicy tcpPolicy,
                                RoomTrafficPolicy udpPolicy,
                                uint16_t mtu,
                                bool passwordProtected,
                                QByteArray leaseToken) {
    m_currentRoomId = roomId;
    m_savedRoomId = roomId;
    m_myVirtualIP = virtualIP;
    m_tcpPolicy = tcpPolicy;
    m_udpPolicy = udpPolicy;
    m_roomMtu = normalizeRoomMtu(mtu);
    m_savedRoomMtu = m_roomMtu;
    m_roomPasswordProtected = passwordProtected;
    rememberResumeLease(roomId, myPeerId(), virtualIP, leaseToken);
    m_tunnel->setMyVirtualIP(virtualIP);

    setupTun();
    m_latencyTimer->start(3000);
    emit roomCreated(roomId);
    emit statusMessage(UiStrings::text("status.roomCreated")
                       .arg(roomId).arg(virtualIPToString(virtualIP)).arg(m_roomMtu));
}

void RoomManager::onJoinResponse(uint32_t roomId, uint32_t virtualIP,
                                 RoomTrafficPolicy tcpPolicy,
                                 RoomTrafficPolicy udpPolicy,
                                 uint16_t mtu,
                                 bool passwordProtected,
                                 QList<PeerInfo> members,
                                 QByteArray leaseToken)
{
    m_pendingResumeRoom = false;
    m_wantReconnect = false;
    m_currentRoomId = roomId;
    m_savedRoomId = roomId;
    m_myVirtualIP = virtualIP;
    m_tcpPolicy = tcpPolicy;
    m_udpPolicy = udpPolicy;
    m_roomMtu = normalizeRoomMtu(mtu);
    m_savedRoomMtu = m_roomMtu;
    m_roomPasswordProtected = passwordProtected;
    rememberResumeLease(roomId, myPeerId(), virtualIP, leaseToken);
    m_tunnel->setMyVirtualIP(virtualIP);

    setupTun();
    m_latencyTimer->start(3000);

    emit roomJoined(roomId);
    emit statusMessage(UiStrings::text("status.roomJoined")
                       .arg(roomId).arg(virtualIPToString(virtualIP)).arg(m_roomMtu));

    for (const PeerInfo& pi : members) {
        if (pi.peerId == myPeerId()) continue;
        QString name = QString::fromStdString(pi.name);
        if (name.isEmpty()) name = QString("Peer%1").arg(pi.peerId);
        m_tunnel->addPeer(pi.peerId, pi.virtualIP, name);
        emit peerConnected(pi.peerId, pi.virtualIP, name);
        m_signal->requestRelay(pi.peerId);
    }
}

void RoomManager::onPeerJoined(PeerInfo info) {
    QString name = QString::fromStdString(info.name);
    if (name.isEmpty()) name = QString("Peer%1").arg(info.peerId);
    m_tunnel->addPeer(info.peerId, info.virtualIP, name);
    emit peerConnected(info.peerId, info.virtualIP, name);
    emit statusMessage(UiStrings::text("status.playerJoined")
                       .arg(name).arg(virtualIPToString(info.virtualIP)));
    m_signal->requestRelay(info.peerId);
}

void RoomManager::onPeerResumed(PeerInfo info) {
    QString name = QString::fromStdString(info.name);
    if (name.isEmpty()) name = QString("Peer%1").arg(info.peerId);

    PeerConnection* peer = m_tunnel->peerById(info.peerId);
    if (!peer) {
        m_tunnel->addPeer(info.peerId, info.virtualIP, name);
        emit peerConnected(info.peerId, info.virtualIP, name);
        emit statusMessage(UiStrings::text("status.playerJoined")
                           .arg(name).arg(virtualIPToString(info.virtualIP)));
        m_signal->requestRelay(info.peerId);
        return;
    }

    LogManager::instance().logDetail(QString("[room] Peer resumed peer=%1 vip=%2 oldTcp=%3 oldUdp=%4, rebuilding transports")
        .arg(info.peerId)
        .arg(virtualIPToString(info.virtualIP))
        .arg(transportName(peer->transport(TRAFFIC_TCP)))
        .arg(transportName(peer->transport(TRAFFIC_UDP))));
    emit statusMessage(UiStrings::text("status.peerResumedRebuild").arg(peer->name()));
    rebuildPeerTransports(info.peerId);
}

void RoomManager::onPeerLeft(uint32_t peerId) {
    PeerConnection* peer = m_tunnel->peerById(peerId);
    QString name = peer ? peer->name() : QString("Peer%1").arg(peerId);
    clearPendingRebuild(peerId);
    m_tunnel->removePeer(peerId);
    emit peerDisconnected(peerId);
    emit statusMessage(UiStrings::text("status.playerLeft").arg(name));
}

void RoomManager::onRelayReady(uint32_t peerId) {
    PeerConnection* peer = m_tunnel->peerById(peerId);
    if (!peer) return;

    bool rebuildTcp = takePendingRebuild(peerId, TRAFFIC_TCP);
    bool rebuildUdp = takePendingRebuild(peerId, TRAFFIC_UDP);
    bool initialSetup = !rebuildTcp && !rebuildUdp;
    bool changed = false;

    if ((initialSetup || rebuildTcp) && peer->transport(TRAFFIC_TCP) == TRANSPORT_NONE) {
        setupPolicyTunnel(peerId, TRAFFIC_TCP);
        changed = true;
    }
    if ((initialSetup || rebuildUdp) && peer->transport(TRAFFIC_UDP) == TRANSPORT_NONE) {
        setupPolicyTunnel(peerId, TRAFFIC_UDP);
        changed = true;
    }

    if (!changed)
        return;

    emit statusMessage(UiStrings::text("status.relayReady")
                       .arg(peer->name())
                       .arg(transportName(peer->transport(TRAFFIC_TCP)))
                       .arg(transportName(peer->transport(TRAFFIC_UDP))));
}

// Internal

void RoomManager::setupPolicyTunnel(uint32_t peerId, TrafficClass cls) {
    PeerConnection* peer = m_tunnel->peerById(peerId);
    if (!peer || peer->transport(cls) != TRANSPORT_NONE)
        return;

    RoomTrafficPolicy policy = cls == TRAFFIC_TCP ? m_tcpPolicy : m_udpPolicy;
    policy = normalizeTrafficPolicy(policy.transportMode, policy.fecMode,
                                    policy.kcpProfile,
                                    cls == TRAFFIC_TCP ? makeDefaultTcpPolicy()
                                                       : makeDefaultUdpPolicy());

    if (policy.transportMode == MODE_RELAY_TCP) {
        setupTcpRelayTunnel(peerId, cls);
        return;
    }
    if (policy.transportMode == MODE_RELAY_RAW_UDP) {
        setupRawUdpRelayTunnel(peerId, cls);
        return;
    }
    setupRelayTunnel(peerId, cls);
}

void RoomManager::setupRelayTunnel(uint32_t peerId, TrafficClass cls) {
    PeerConnection* peer = m_tunnel->peerById(peerId);
    if (!peer || peer->transport(cls) != TRANSPORT_NONE)
        return;

    RoomTrafficPolicy policy = cls == TRAFFIC_TCP ? m_tcpPolicy : m_udpPolicy;
    KcpTunnel* kcp = m_tunnel->createKcpTunnel(
        peer, m_resolvedAddr, m_port, TRANSPORT_RELAY_KCP,
        policy.fecMode, m_roomMtu, policy.kcpProfile, cls);
    kcp->setRelayMode(myPeerId(), peerId);
    emit peerTransportChanged(peerId, cls, TRANSPORT_RELAY_KCP);
    LogManager::instance().logDetail(QString("[room] KCP relay ready peer=%1 class=%2 fec=%3 profile=%4")
        .arg(peerId).arg(trafficClassName(cls)).arg(fecModeName(policy.fecMode))
        .arg(policy.kcpProfile == KCP_PROFILE_BULK ? "bulk" : "realtime"));
}

void RoomManager::setupRawUdpRelayTunnel(uint32_t peerId, TrafficClass cls) {
    PeerConnection* peer = m_tunnel->peerById(peerId);
    if (!peer || peer->transport(cls) != TRANSPORT_NONE)
        return;

    RoomTrafficPolicy policy = cls == TRAFFIC_TCP ? m_tcpPolicy : m_udpPolicy;
    RawUdpTunnel* tunnel = m_tunnel->createRawUdpTunnel(
        peer, m_resolvedAddr, m_port, policy.fecMode, m_roomMtu, cls);
    tunnel->setRelayMode(myPeerId(), peerId);
    emit peerTransportChanged(peerId, cls, TRANSPORT_RELAY_RAW_UDP);
    LogManager::instance().logDetail(QString("[room] Raw UDP relay ready peer=%1 class=%2 fec=%3")
        .arg(peerId).arg(trafficClassName(cls)).arg(fecModeName(policy.fecMode)));
}

void RoomManager::setupTcpRelayTunnel(uint32_t peerId, TrafficClass cls) {
    PeerConnection* peer = m_tunnel->peerById(peerId);
    if (!peer || peer->transport(cls) != TRANSPORT_NONE)
        return;

    uint32_t myId = myPeerId();
    peer->setTcpRelaySender([this, myId](uint32_t dstPeerId, TrafficClass trafficClass,
                                         const QByteArray& data) {
        if (m_dataChannel && m_dataChannel->isConnected()) {
            m_dataChannel->sendRelayData(myId, dstPeerId, trafficClass, data);
        }
    });
    peer->setTransport(cls, TRANSPORT_RELAY_TCP);
    emit peerTransportChanged(peerId, cls, TRANSPORT_RELAY_TCP);

    if (!m_tcpRelayTimer) {
        m_tcpRelayTimer = new QTimer(this);
        connect(m_tcpRelayTimer, &QTimer::timeout,
                this, &RoomManager::onTcpRelayHealthCheck);
        m_tcpRelayTimer->start(TCP_RELAY_KEEPALIVE_MS / 2);
    }
    LogManager::instance().logDetail(QString("[room] TCP relay ready peer=%1 class=%2")
        .arg(peerId).arg(trafficClassName(cls)));
}

void RoomManager::handleTcpRelayReceived(uint32_t srcPeerId, TrafficClass cls, QByteArray data) {
    PeerConnection* peer = m_tunnel->peerById(srcPeerId);
    if (!peer) return;
    peer->onTcpRelayDataReceived(cls);
    if (data.isEmpty()) return;
    if (data.size() >= 2 && static_cast<uint8_t>(data[0]) == LATENCY_PROBE_MARKER) {
        peer->handleLatencyProbe(cls, data);
        return;
    }
    if (m_tun && m_tun->writePacket(data))
        m_tunnel->addTunDownloadBytes(static_cast<quint64>(data.size()));
}

void RoomManager::onDataChannelConnected() {
    emit statusMessage(UiStrings::text("status.dataChannelConnected"));
}

void RoomManager::onDataChannelDisconnected() {
    emit statusMessage(UiStrings::text("status.dataChannelDisconnected"));
}

void RoomManager::onDataChannelRelayReceived(uint32_t srcPeerId, TrafficClass cls, QByteArray data) {
    handleTcpRelayReceived(srcPeerId, cls, data);
}

void RoomManager::setupTun() {
    if (m_tun) return;

    m_tun = new TunAdapter(this);
    connect(m_tun, &TunAdapter::firewallRuleChanged,
            this, [this](bool added, bool success) {
        if (added) {
            emit statusMessage(success
                ? UiStrings::text("status.firewallAdded")
                : UiStrings::text("status.firewallAddFailed"));
        } else {
            emit statusMessage(success
                ? UiStrings::text("status.firewallRemoved")
                : UiStrings::text("status.firewallRemoveFailed"));
        }
    });
    if (!m_tun->initialize()) {
        emit errorOccurred(UiStrings::text("status.tunInitFailed"));
        delete m_tun; m_tun = nullptr;
        return;
    }
    int mtu = static_cast<int>(normalizeRoomMtu(m_roomMtu));
    if (!m_tun->configureIP(m_myVirtualIP, VNET_MASK, mtu)) {
        emit errorOccurred(UiStrings::text("status.tunIpFailed"));
        delete m_tun; m_tun = nullptr;
        return;
    }
    if (!m_tun->startSession()) {
        emit errorOccurred(UiStrings::text("status.tunSessionFailed"));
        delete m_tun; m_tun = nullptr;
        return;
    }
    m_tunnel->setTunAdapter(m_tun);
    m_tunnel->resetTrafficCounters();
    m_lastUploadBytes = 0;
    m_lastDownloadBytes = 0;
    if (m_trafficTimer)
        m_trafficTimer->start(1000);
    emit tunSpeedUpdated(0, 0);
    emit statusMessage(UiStrings::text("status.tunStarted")
                       .arg(virtualIPToString(m_myVirtualIP)).arg(mtu));
}

void RoomManager::teardownTun() {
    m_pendingRebuild.clear();
    if (m_latencyTimer)
        m_latencyTimer->stop();
    if (m_trafficTimer)
        m_trafficTimer->stop();
    m_lastUploadBytes = 0;
    m_lastDownloadBytes = 0;
    if (m_tunnel)
        m_tunnel->resetTrafficCounters();
    emit tunSpeedUpdated(0, 0);
    if (m_tcpRelayTimer) {
        m_tcpRelayTimer->stop();
        delete m_tcpRelayTimer;
        m_tcpRelayTimer = nullptr;
    }
    if (m_dataChannel) {
        m_dataChannel->disconnect();
        delete m_dataChannel;
        m_dataChannel = nullptr;
    }
    m_roomPasswordProtected = false;
    m_roomPassword.clear();
    m_tunnel->removeAllPeers();
    if (m_tun) {
        m_tunnel->setTunAdapter(nullptr);
        m_tun->shutdown();
        delete m_tun;
        m_tun = nullptr;
    }
}

void RoomManager::onTcpRelayHealthCheck() {
    QList<TransportKey> deadTransports;

    for (PeerConnection* peer : m_tunnel->allPeers()) {
        peer->sendTcpRelayKeepalive();
        for (int cls = TRAFFIC_TCP; cls <= TRAFFIC_UDP; ++cls) {
            TrafficClass trafficClass = static_cast<TrafficClass>(cls);
            if (peer->isTcpRelayDead(trafficClass)) {
                TransportKey key;
                key.peerId = peer->peerId();
                key.cls = trafficClass;
                deadTransports.append(key);
            }
        }
    }

    for (const TransportKey& key : deadTransports) {
        PeerConnection* peer = m_tunnel->peerById(key.peerId);
        if (!peer || peer->transport(key.cls) != TRANSPORT_RELAY_TCP)
            continue;
        onTransportDead(key.peerId, key.cls);
    }
}

void RoomManager::onLatencyUpdate() {
    for (PeerConnection* peer : m_tunnel->allPeers()) {
        peer->sendLatencyPing(TRAFFIC_TCP);
        peer->sendLatencyPing(TRAFFIC_UDP);
        emit peerLatencyUpdated(peer->peerId(), TRAFFIC_TCP,
                                peer->latencyMs(TRAFFIC_TCP));
        emit peerLatencyUpdated(peer->peerId(), TRAFFIC_UDP,
                                peer->latencyMs(TRAFFIC_UDP));
    }
}

void RoomManager::onTrafficUpdate() {
    if (!m_tun) {
        emit tunSpeedUpdated(0, 0);
        return;
    }

    quint64 uploadBytes = 0;
    quint64 downloadBytes = 0;
    m_tunnel->trafficCounters(&uploadBytes, &downloadBytes);

    quint64 uploadRate = uploadBytes >= m_lastUploadBytes
        ? uploadBytes - m_lastUploadBytes : 0;
    quint64 downloadRate = downloadBytes >= m_lastDownloadBytes
        ? downloadBytes - m_lastDownloadBytes : 0;

    m_lastUploadBytes = uploadBytes;
    m_lastDownloadBytes = downloadBytes;
    emit tunSpeedUpdated(uploadRate, downloadRate);
}

void RoomManager::onTransportDead(uint32_t peerId, TrafficClass cls) {
    PeerConnection* peer = m_tunnel->peerById(peerId);
    if (!peer || peer->transport(cls) == TRANSPORT_NONE)
        return;

    TransportKey key;
    key.peerId = peerId;
    key.cls = cls;
    if (m_pendingRebuild.contains(key))
        return;

    QString name = peer->name();
    TransportType deadTransport = peer->transport(cls);
    m_pendingRebuild[key] = true;

    emit statusMessage(UiStrings::text(deadTransport == TRANSPORT_RELAY_TCP
                       ? "status.tcpRelayTimeout"
                       : "status.tunnelTimeout").arg(name));

    m_tunnel->removeTransport(peerId, cls);
    emit peerTransportChanged(peerId, cls, TRANSPORT_NONE);
    if (m_signal && m_signal->isConnected())
        m_signal->requestRelay(peerId);
}

void RoomManager::clearPendingRebuild(uint32_t peerId) {
    for (auto it = m_pendingRebuild.begin(); it != m_pendingRebuild.end(); ) {
        if (it.key().peerId == peerId)
            it = m_pendingRebuild.erase(it);
        else
            ++it;
    }
}

bool RoomManager::takePendingRebuild(uint32_t peerId, TrafficClass cls) {
    TransportKey key;
    key.peerId = peerId;
    key.cls = cls;
    if (!m_pendingRebuild.contains(key))
        return false;
    m_pendingRebuild.remove(key);
    return true;
}

void RoomManager::handleReconnectRoomList(QList<RoomListItem> rooms) {
    m_wantReconnect = false;

    QList<uint32_t> matches;
    for (int i = 0; i < rooms.size(); ++i) {
        const RoomListItem& item = rooms[i];
        if (QString::fromUtf8(item.roomName) == m_savedRoomName &&
            item.maxPlayers == m_savedMaxPlayers &&
            normalizeRoomMtu(item.mtu) == m_savedRoomMtu &&
            (item.passwordProtected != 0) == m_savedRoomPasswordProtected &&
            samePolicy(item.tcpPolicy, m_savedTcpPolicy) &&
            samePolicy(item.udpPolicy, m_savedUdpPolicy)) {
            matches.append(item.roomId);
        }
    }

    if (matches.size() == 1) {
        uint32_t foundRoomId = matches.first();
        emit statusMessage(UiStrings::text("status.reconnectFoundRoom")
                           .arg(m_savedRoomName).arg(foundRoomId));
        joinRoom(foundRoomId, m_savedRoomPasswordProtected ? m_savedRoomPassword : QString());
    } else if (matches.size() > 1) {
        emit errorOccurred(QStringLiteral("Multiple matching rooms found; please choose one manually."));
        emit roomListUpdated(rooms);
    } else {
        emit statusMessage(UiStrings::text("status.reconnectRoomMissing")
                           .arg(m_savedRoomName));
        createRoom(m_savedRoomName, m_savedMaxPlayers,
                   m_savedTcpPolicy, m_savedUdpPolicy,
                   m_savedRoomMtu, m_savedRoomPasswordProtected,
                   m_savedRoomPassword);
    }
}

} // namespace VLan

#include "room_manager.h"
#include "signal_client.h"
#include "data_channel.h"
#include "nat_detector.h"
#include "hole_puncher.h"
#include "../core/tunnel_manager.h"
#include "../core/tun_adapter.h"
#include "../core/kcp_tunnel.h"
#include "../core/raw_udp_tunnel.h"
#include "../core/p2p_peer.h"
#include "../ui/log_manager.h"
#include <QTimer>

namespace VLan {

RoomManager::RoomManager(QObject* parent)
    : QObject(parent),
      m_dataChannel(nullptr),
      m_tun(nullptr), m_natDetector(nullptr), m_puncher(nullptr),
      m_port(DEFAULT_PORT),
      m_isDefaultServer(false),
      m_tcpRelayTimer(nullptr), m_latencyTimer(nullptr), m_trafficTimer(nullptr),
      m_lastUploadBytes(0), m_lastDownloadBytes(0),
      m_currentRoomId(0), m_myVirtualIP(0),
      m_myNatType(NAT_UNKNOWN), m_transportMode(MODE_RELAY_KCP), m_fecMode(FEC_NONE),
      m_roomMtu(ROOM_MTU_DEFAULT),
      m_encrypted(false), m_cipher(nullptr),
      m_wantReconnect(false), m_reconnectAttempts(0), m_wasInRoom(false),
      m_savedMaxPlayers(8), m_savedTransportMode(MODE_RELAY_KCP),
      m_savedFecMode(FEC_NONE), m_savedRoomMtu(ROOM_MTU_DEFAULT),
      m_savedEncrypted(false)
{
    m_signal = new SignalClient(this);
    m_tunnel = new TunnelManager(this);

    connect(m_signal, &SignalClient::connected,     this, &RoomManager::onSignalConnected);
    connect(m_signal, &SignalClient::disconnected,   this, &RoomManager::onSignalDisconnected);
    connect(m_signal, &SignalClient::loginResponse,  this, &RoomManager::onLoginResponse);
    connect(m_signal, &SignalClient::roomCreated,    this, &RoomManager::onRoomCreated);
    connect(m_signal, &SignalClient::joinResponse,   this, &RoomManager::onJoinResponse);
    connect(m_signal, &SignalClient::peerJoined,     this, &RoomManager::onPeerJoined);
    connect(m_signal, &SignalClient::peerLeft,       this, &RoomManager::onPeerLeft);
    connect(m_signal, &SignalClient::punchNotify,    this, &RoomManager::onPunchNotify);
    connect(m_signal, &SignalClient::relayReady,     this, &RoomManager::onRelayReady);
    connect(m_signal, &SignalClient::serverError, this, [this](const QString& msg) {
        emit errorOccurred(msg);
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
        if (m_wantReconnect && m_wasInRoom && !m_savedRoomName.isEmpty()) {
            handleReconnectRoomList(rooms);
            return;
        }
        emit roomListUpdated(rooms);
    });
    connect(m_signal, &SignalClient::serverRttUpdated,
            this, &RoomManager::serverRttUpdated);

    connect(m_tunnel, &TunnelManager::tunnelDead,
            this, &RoomManager::onTunnelDead);

    m_latencyTimer = new QTimer(this);
    connect(m_latencyTimer, &QTimer::timeout, this, &RoomManager::onLatencyUpdate);

    m_trafficTimer = new QTimer(this);
    connect(m_trafficTimer, &QTimer::timeout, this, &RoomManager::onTrafficUpdate);
}

RoomManager::~RoomManager() {
    teardownTun();
}

void RoomManager::setServerAddress(const QString& host, quint16 port) {
    m_serverHost   = host;
    m_port         = port;
    m_resolvedAddr = QHostAddress();
}

void RoomManager::connectAndLogin(const QString& playerName) {
    m_playerName = playerName;
    if (m_isDefaultServer)
        emit statusMessage(QString::fromUtf8("正在连接默认服务器 ..."));
    else
        emit statusMessage(QString::fromUtf8("正在连接服务器 %1:%2 ...").arg(m_serverHost).arg(m_port));
    resolveAndConnect();
}

void RoomManager::disconnectFromServer() {
    m_wantReconnect = false;
    m_playerName.clear();
    m_signal->disconnect();
}

void RoomManager::resolveAndConnect() {
    QHostAddress directAddr(m_serverHost);
    if (!directAddr.isNull()) {
        m_resolvedAddr = directAddr;
        if (m_isDefaultServer)
            LogManager::instance().addMaskedKeyword(m_resolvedAddr.toString());
        proceedWithConnection();
        return;
    }
    if (m_isDefaultServer)
        emit statusMessage(QString::fromUtf8("正在解析服务器地址 ..."));
    else
        emit statusMessage(QString::fromUtf8("正在解析域名 %1 ...").arg(m_serverHost));
    QHostInfo::lookupHost(m_serverHost, this, SLOT(onHostResolved(QHostInfo)));
}

void RoomManager::onHostResolved(const QHostInfo& hostInfo) {
    if (hostInfo.error() != QHostInfo::NoError || hostInfo.addresses().isEmpty()) {
        emit errorOccurred(QString::fromUtf8("域名解析失败: %1").arg(hostInfo.errorString()));
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

    if (m_isDefaultServer) {
        LogManager::instance().addMaskedKeyword(m_resolvedAddr.toString());
        emit statusMessage(QString::fromUtf8("服务器地址已解析"));
    } else {
        emit statusMessage(QString::fromUtf8("域名已解析: %1 -> %2").arg(m_serverHost).arg(m_resolvedAddr.toString()));
    }
    proceedWithConnection();
}

void RoomManager::proceedWithConnection() {
    m_tunnel->setServerEndpoint(m_resolvedAddr, m_port);
    m_signal->connectToServer(m_serverHost, m_port);
}

uint32_t RoomManager::myPeerId() const {
    return m_signal->myPeerId();
}

// ───────── Room operations ─────────

void RoomManager::createRoom(const QString& roomName, uint8_t maxPlayers,
                             TransportMode mode, FecMode fecMode,
                             uint16_t mtu,
                             bool encrypted, const QString& password) {
    uint16_t normalizedMtu = normalizeRoomMtu(mtu);
    m_savedRoomName = roomName;
    m_savedMaxPlayers = maxPlayers;
    m_savedRoomMtu = normalizedMtu;
    m_roomMtu = normalizedMtu;
    m_encrypted = encrypted;
    m_roomPassword = encrypted ? password : QString();
    QByteArray pwdHash;
    if (encrypted && !password.isEmpty()) {
        m_intermediate = PayloadCipher::computeIntermediate(password);
        pwdHash = PayloadCipher::hashFromIntermediate(m_intermediate);
    }
    m_signal->createRoom(roomName, maxPlayers, mode, fecMode,
                         normalizedMtu, encrypted, pwdHash);
}

void RoomManager::joinRoom(uint32_t roomId, const QString& password) {
    for (int i = 0; i < m_cachedRoomList.size(); ++i) {
        if (m_cachedRoomList[i].roomId == roomId) {
            m_savedRoomName = QString::fromUtf8(m_cachedRoomList[i].roomName);
            m_savedMaxPlayers = m_cachedRoomList[i].maxPlayers;
            m_savedRoomMtu = normalizeRoomMtu(m_cachedRoomList[i].mtu);
            m_roomMtu = m_savedRoomMtu;
            break;
        }
    }
    m_roomPassword = password;
    QByteArray authHash;
    if (!password.isEmpty()) {
        m_intermediate = PayloadCipher::computeIntermediate(password);
        authHash = PayloadCipher::hashFromIntermediate(m_intermediate);
    }
    m_signal->joinRoom(roomId, authHash);
}

void RoomManager::leaveRoom() {
    m_signal->leaveRoom();
    teardownTun();
    m_currentRoomId = 0;
    m_myVirtualIP   = 0;
    m_roomMtu       = ROOM_MTU_DEFAULT;
    m_encrypted     = false;
    m_roomPassword.clear();
    m_savedRoomName.clear();
    m_wasInRoom = false;
    if (!m_intermediate.isEmpty()) {
        crypto_wipe(reinterpret_cast<uint8_t*>(m_intermediate.data()), m_intermediate.size());
        m_intermediate.clear();
    }
    emit roomLeft();
}

void RoomManager::refreshRoomList() {
    m_signal->listRooms();
}

// ───────── Signal slots ─────────

void RoomManager::onSignalConnected() {
    emit statusMessage(QString::fromUtf8("已连接，正在登录..."));
    emit connectionStatusChanged(true);
    m_signal->login(m_playerName);
}

void RoomManager::onSignalDisconnected() {
    bool wasInRoom = (m_currentRoomId != 0);

    if (!m_wantReconnect) {
        if (wasInRoom) {
            m_wasInRoom = true;
            m_savedTransportMode = m_transportMode;
            m_savedFecMode = m_fecMode;
            m_savedRoomMtu = m_roomMtu;
            m_savedEncrypted = m_encrypted;
            m_savedRoomPassword = m_roomPassword;
        } else {
            m_wasInRoom = false;
        }
    }

    teardownTun();
    m_currentRoomId = 0;
    m_myVirtualIP   = 0;
    m_roomMtu       = ROOM_MTU_DEFAULT;
    if (wasInRoom) emit roomLeft();
    emit connectionStatusChanged(false);

    if (!m_wantReconnect && !m_playerName.isEmpty()) {
        m_wantReconnect = true;
        m_reconnectAttempts = 0;
        emit statusMessage(QString::fromUtf8("与服务器断开连接，将在 %1 秒后尝试重连 (最多 %2 次)%3")
                           .arg(RECONNECT_INTERVAL_MS / 1000)
                           .arg(MAX_RECONNECT_ATTEMPTS)
                           .arg(m_wasInRoom ? QString::fromUtf8("，将自动回到房间") : QString()));
        scheduleReconnectAttempt();
    } else if (m_wantReconnect) {
        scheduleReconnectAttempt();
    } else {
        emit statusMessage(QString::fromUtf8("与服务器断开连接"));
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
            m_wasInRoom = false;
            m_savedRoomName.clear();
            emit statusMessage(QString::fromUtf8("自动重连失败 (已尝试 %1 次)")
                               .arg(MAX_RECONNECT_ATTEMPTS));
            return;
        }
        m_reconnectAttempts++;
        emit statusMessage(QString::fromUtf8("正在重新连接 (%1/%2)...")
                           .arg(m_reconnectAttempts).arg(MAX_RECONNECT_ATTEMPTS));
        resolveAndConnect();
    });
}

void RoomManager::onLoginResponse(uint32_t peerId) {
    emit statusMessage(QString::fromUtf8("登录成功，PeerId=%1").arg(peerId));
    emit loggedIn(peerId);
    m_tunnel->setMyPeerId(peerId);

    if (m_wantReconnect) {
        if (m_wasInRoom && !m_savedRoomName.isEmpty()) {
            emit statusMessage(QString::fromUtf8("重连成功，正在查找房间 \"%1\"...")
                               .arg(m_savedRoomName));
            m_signal->listRooms();
        } else {
            m_wantReconnect = false;
            emit statusMessage(QString::fromUtf8("重连成功"));
        }
    }

    // Create data channel for TCP relay traffic
    if (!m_dataChannel) {
        m_dataChannel = new DataChannel(this);
        connect(m_dataChannel, &DataChannel::connected,
                this, &RoomManager::onDataChannelConnected);
        connect(m_dataChannel, &DataChannel::disconnected,
                this, &RoomManager::onDataChannelDisconnected);
        connect(m_dataChannel, &DataChannel::relayDataReceived,
                this, &RoomManager::onDataChannelRelayReceived);
    }
    m_dataChannel->connectToServer(m_serverHost, m_port, peerId);

    delete m_puncher;
    m_puncher = new HolePuncher(m_tunnel->udpSocket(), peerId, this);
    connect(m_puncher, &HolePuncher::punchSucceeded, this, &RoomManager::onPunchSucceeded);
    connect(m_puncher, &HolePuncher::punchFailed,    this, &RoomManager::onPunchFailed);

    delete m_natDetector;
    m_natDetector = new NatDetector(m_tunnel->udpSocket(), this);
    m_natDetector->setMyPeerId(peerId);
    connect(m_natDetector, &NatDetector::detected, this, &RoomManager::onNatDetected);
    m_natDetector->detect(m_resolvedAddr, m_port);

    if (m_rawUdpConnection)
        disconnect(m_rawUdpConnection);
    m_rawUdpConnection = connect(m_tunnel, &TunnelManager::rawUdpReceived,
            [this](QByteArray data, QHostAddress senderAddr, quint16 senderPort) {
        if (data.isEmpty()) return;
        uint8_t pktType = static_cast<uint8_t>(data[0]);
        const uint8_t* raw = reinterpret_cast<const uint8_t*>(data.constData());
        if (pktType == UDP_STUN_RESPONSE && m_natDetector) {
            m_natDetector->handleStunResponse(raw, data.size());
        } else if ((pktType == UDP_PUNCH || pktType == UDP_PUNCH_ACK) && m_puncher) {
            m_puncher->handleIncomingPacket(raw, data.size(), senderAddr, senderPort);
        }
    });

}

void RoomManager::onRoomCreated(uint32_t roomId, uint32_t virtualIP,
                                TransportMode transportMode,
                                FecMode fecMode,
                                uint16_t mtu,
                                bool encrypted, QByteArray salt,
                                QByteArray sessionSeed) {
    m_currentRoomId  = roomId;
    m_myVirtualIP    = virtualIP;
    m_transportMode  = transportMode;
    m_fecMode        = fecMode;
    m_roomMtu        = normalizeRoomMtu(mtu);
    m_savedRoomMtu   = m_roomMtu;
    m_encrypted      = encrypted;
    m_tunnel->setMyVirtualIP(virtualIP);

    if (encrypted && m_intermediate.size() == 32
        && salt.size() == 16 && sessionSeed.size() == 16) {
        QByteArray key = PayloadCipher::deriveKey(m_intermediate, salt);
        delete m_cipher;
        m_cipher = new PayloadCipher(
            reinterpret_cast<const uint8_t*>(key.constData()), myPeerId(),
            reinterpret_cast<const uint8_t*>(sessionSeed.constData()));
        crypto_wipe(reinterpret_cast<uint8_t*>(key.data()), key.size());
        crypto_wipe(reinterpret_cast<uint8_t*>(m_intermediate.data()), m_intermediate.size());
        m_intermediate.clear();
    }

    setupTun();
    m_latencyTimer->start(3000);
    emit roomCreated(roomId);
    emit statusMessage(QString::fromUtf8("房间已创建 (ID=%1, IP=%2, MTU=%3)")
                       .arg(roomId).arg(virtualIPToString(virtualIP)).arg(m_roomMtu));
}

void RoomManager::onJoinResponse(uint32_t roomId, uint32_t virtualIP,
                                  TransportMode transportMode,
                                  FecMode fecMode,
                                  uint16_t mtu,
                                  bool encrypted, QByteArray salt,
                                  QByteArray sessionSeed,
                                  QList<PeerInfo> members)
{
    m_currentRoomId  = roomId;
    m_myVirtualIP    = virtualIP;
    m_transportMode  = transportMode;
    m_fecMode        = fecMode;
    m_roomMtu        = normalizeRoomMtu(mtu);
    m_savedRoomMtu   = m_roomMtu;
    m_encrypted      = encrypted;
    m_tunnel->setMyVirtualIP(virtualIP);

    if (encrypted && m_intermediate.size() == 32
        && salt.size() == 16 && sessionSeed.size() == 16) {
        QByteArray key = PayloadCipher::deriveKey(m_intermediate, salt);
        delete m_cipher;
        m_cipher = new PayloadCipher(
            reinterpret_cast<const uint8_t*>(key.constData()), myPeerId(),
            reinterpret_cast<const uint8_t*>(sessionSeed.constData()));
        crypto_wipe(reinterpret_cast<uint8_t*>(key.data()), key.size());
        crypto_wipe(reinterpret_cast<uint8_t*>(m_intermediate.data()), m_intermediate.size());
        m_intermediate.clear();
    }

    setupTun();

    m_latencyTimer->start(3000);

    emit roomJoined(roomId);
    emit statusMessage(QString::fromUtf8("已加入房间 (ID=%1, IP=%2, MTU=%3)")
                       .arg(roomId).arg(virtualIPToString(virtualIP)).arg(m_roomMtu));

    for (const PeerInfo& pi : members) {
        if (pi.peerId == myPeerId()) continue;
        QString name = QString::fromStdString(pi.name);
        if (name.isEmpty()) name = QString("Peer%1").arg(pi.peerId);
        P2PPeer* peer = m_tunnel->addPeer(pi.peerId, pi.virtualIP, name);
        peer->setNatType(pi.natType);
        if (m_cipher) peer->setCipher(m_cipher);
        connect(peer, &P2PPeer::latencyPongReply,
                this, &RoomManager::onLatencyPongReply);
        emit peerConnected(pi.peerId, pi.virtualIP, name);

        if (m_transportMode == MODE_RELAY_TCP) {
            setupTcpRelayTunnel(pi.peerId);
        } else if (m_transportMode == MODE_RELAY_KCP) {
            m_signal->requestRelay(pi.peerId);
        } else if (m_transportMode == MODE_RELAY_RAW_UDP) {
            m_signal->requestRelay(pi.peerId);
        } else if (m_transportMode == MODE_P2P_ONLY && pi.publicIP != 0) {
            initiatePunch(pi.peerId, pi.publicIP, pi.publicPort);
        }
    }
}

void RoomManager::onPeerJoined(PeerInfo info) {
    QString name = QString::fromStdString(info.name);
    if (name.isEmpty()) name = QString("Peer%1").arg(info.peerId);
    P2PPeer* peer = m_tunnel->addPeer(info.peerId, info.virtualIP, name);
    if (peer) {
        peer->setNatType(info.natType);
        if (m_cipher) peer->setCipher(m_cipher);
        connect(peer, &P2PPeer::latencyPongReply,
                this, &RoomManager::onLatencyPongReply);
    }
    emit peerConnected(info.peerId, info.virtualIP, name);
    emit statusMessage(QString::fromUtf8("玩家 %1 加入房间 (IP=%2)")
                       .arg(name).arg(virtualIPToString(info.virtualIP)));

    if (m_transportMode == MODE_RELAY_TCP) {
        setupTcpRelayTunnel(info.peerId);
    } else if (m_transportMode == MODE_RELAY_KCP) {
        m_signal->requestRelay(info.peerId);
    } else if (m_transportMode == MODE_RELAY_RAW_UDP) {
        m_signal->requestRelay(info.peerId);
    }
}

void RoomManager::onPeerLeft(uint32_t peerId) {
    P2PPeer* peer = m_tunnel->peerById(peerId);
    QString name = peer ? peer->name() : QString("Peer%1").arg(peerId);
    m_tunnel->removePeer(peerId);
    emit peerDisconnected(peerId);
    emit statusMessage(QString::fromUtf8("玩家 %1 离开房间").arg(name));
}

void RoomManager::onPunchNotify(uint32_t peerId, uint32_t virtualIP,
                                 NatType natType, uint32_t publicIP, uint16_t publicPort)
{
    P2PPeer* peer = m_tunnel->peerById(peerId);
    if (!peer) {
        peer = m_tunnel->addPeer(peerId, virtualIP,
                                  QString("Peer%1").arg(peerId));
        if (m_cipher) peer->setCipher(m_cipher);
        connect(peer, &P2PPeer::latencyPongReply,
                this, &RoomManager::onLatencyPongReply);
    }
    peer->setNatType(natType);

    if (m_transportMode == MODE_RELAY_TCP) {
        setupTcpRelayTunnel(peerId);
        return;
    }
    if (m_transportMode == MODE_RELAY_KCP) {
        m_signal->requestRelay(peerId);
        return;
    }
    if (m_transportMode == MODE_RELAY_RAW_UDP) {
        m_signal->requestRelay(peerId);
        return;
    }

    if (publicIP != 0) {
        peer->setPublicEndpoint(QHostAddress(publicIP), publicPort);
        initiatePunch(peerId, publicIP, publicPort);
    }
}

void RoomManager::onRelayReady(uint32_t peerId) {
    P2PPeer* peer = m_tunnel->peerById(peerId);
    if (!peer) return;
    if (peer->transport() != TRANSPORT_NONE) return;
    QString name = peer->name();

    if (m_transportMode == MODE_RELAY_TCP) {
        setupTcpRelayTunnel(peerId);
        return;
    }
    if (m_transportMode == MODE_RELAY_RAW_UDP) {
        setupRawUdpRelayTunnel(peerId);
        return;
    }
    if (m_transportMode == MODE_RELAY_KCP) {
        setupRelayTunnel(peerId);
        emit statusMessage(QString::fromUtf8("%1 已通过 KCP中继(UDP) 连接成功 (IP=%2)")
                           .arg(name).arg(virtualIPToString(peer->virtualIP())));
        return;
    }
    LogManager::instance().logError(QString("[room] onRelayReady: unexpected transportMode %1 for peer %2").arg(m_transportMode).arg(peerId));
}

void RoomManager::onNatDetected(NatType type, uint32_t publicIP, uint16_t publicPort) {
    Q_UNUSED(publicIP); Q_UNUSED(publicPort);
    m_myNatType = type;
    m_signal->reportNatType(type);
    emit natDetected(type);
    emit statusMessage(QString::fromUtf8("NAT类型: %1").arg(natTypeName(type)));
}

void RoomManager::onPunchSucceeded(uint32_t peerId, QHostAddress addr, quint16 port) {
    P2PPeer* peer = m_tunnel->peerById(peerId);
    if (!peer) return;
    QString name = peer->name();

    m_tunnel->createKcpTunnel(peer, addr, port, TRANSPORT_P2P_KCP,
                              FEC_NONE, m_roomMtu);
    m_signal->reportPunchResult(peerId, true);
    emit peerTransportChanged(peerId, TRANSPORT_P2P_KCP);
    emit statusMessage(QString::fromUtf8("%1 已通过 P2P直连(UDP) 连接成功 (IP=%2)")
                       .arg(name).arg(virtualIPToString(peer->virtualIP())));
}

void RoomManager::onPunchFailed(uint32_t peerId) {
    P2PPeer* peer = m_tunnel->peerById(peerId);
    QString name = peer ? peer->name() : QString("Peer%1").arg(peerId);

    m_signal->reportPunchResult(peerId, false);

    emit statusMessage(QString::fromUtf8("%1 P2P打洞失败，当前为P2P直连模式，不回退中继")
                       .arg(name));
}

// ───────── Internal ─────────

void RoomManager::initiatePunch(uint32_t peerId, uint32_t publicIP, uint16_t publicPort) {
    if (!m_puncher || publicIP == 0) return;
    m_puncher->startPunch(peerId, QHostAddress(publicIP), publicPort);
}

void RoomManager::setupRelayTunnel(uint32_t peerId) {
    P2PPeer* peer = m_tunnel->peerById(peerId);
    if (!peer) return;
    if (peer->transport() != TRANSPORT_NONE) return;

    KcpTunnel* kcp = m_tunnel->createKcpTunnel(
        peer, m_resolvedAddr, m_port, TRANSPORT_RELAY_KCP, m_fecMode, m_roomMtu);
    kcp->setRelayMode(myPeerId(), peerId);
    emit peerTransportChanged(peerId, TRANSPORT_RELAY_KCP);
}

void RoomManager::setupRawUdpRelayTunnel(uint32_t peerId) {
    P2PPeer* peer = m_tunnel->peerById(peerId);
    if (!peer) return;
    if (peer->transport() != TRANSPORT_NONE) return;

    RawUdpTunnel* tunnel = m_tunnel->createRawUdpTunnel(
        peer, m_resolvedAddr, m_port, m_fecMode, m_roomMtu);
    tunnel->setRelayMode(myPeerId(), peerId);
    emit peerTransportChanged(peerId, TRANSPORT_RELAY_RAW_UDP);

    QString name = peer->name();
    emit statusMessage(QString::fromUtf8("%1 已通过 Raw UDP中继 连接成功 (IP=%2)")
                       .arg(name).arg(virtualIPToString(peer->virtualIP())));
}

void RoomManager::setupTcpRelayTunnel(uint32_t peerId) {
    P2PPeer* peer = m_tunnel->peerById(peerId);
    if (!peer) return;
    if (peer->transport() != TRANSPORT_NONE) return;

    uint32_t myId = myPeerId();
    peer->setTcpRelaySender([this, myId](uint32_t dstPeerId, const QByteArray& data) {
        if (m_dataChannel && m_dataChannel->isConnected()) {
            m_dataChannel->sendRelayData(myId, dstPeerId, data);
        }
    });
    peer->setTransport(TRANSPORT_RELAY_TCP);
    emit peerTransportChanged(peerId, TRANSPORT_RELAY_TCP);

    if (!m_tcpRelayTimer) {
        m_tcpRelayTimer = new QTimer(this);
        connect(m_tcpRelayTimer, &QTimer::timeout,
                this, &RoomManager::onTcpRelayHealthCheck);
        m_tcpRelayTimer->start(TCP_RELAY_KEEPALIVE_MS / 2);
    }

    QString name = peer->name();
    emit statusMessage(QString::fromUtf8("%1 已通过 TCP中继 连接成功 (IP=%2)")
                       .arg(name).arg(virtualIPToString(peer->virtualIP())));
}

void RoomManager::handleTcpRelayReceived(uint32_t srcPeerId, QByteArray data) {
    P2PPeer* peer = m_tunnel->peerById(srcPeerId);
    if (!peer) return;
    peer->onTcpRelayDataReceived();
    if (data.isEmpty()) return;
    if (data.size() >= 2 && static_cast<uint8_t>(data[0]) == LATENCY_PROBE_MARKER) {
        peer->handleLatencyProbe(data);
        return;
    }
    if (m_cipher && data.size() >= 20 && (static_cast<uint8_t>(data[0]) & 0xF0) == 0x40) {
        data = m_cipher->decrypt(data, srcPeerId);
        if (data.isEmpty()) return;
    }
    if (m_tun && m_tun->writePacket(data))
        m_tunnel->addTunDownloadBytes(static_cast<quint64>(data.size()));
}

void RoomManager::onDataChannelConnected() {
    emit statusMessage(QString::fromUtf8("数据通道已建立"));
}

void RoomManager::onDataChannelDisconnected() {
    emit statusMessage(QString::fromUtf8("数据通道断开，正在自动重连..."));
}

void RoomManager::onDataChannelRelayReceived(uint32_t srcPeerId, QByteArray data) {
    handleTcpRelayReceived(srcPeerId, data);
}

void RoomManager::setupTun() {
    if (m_tun) return;

    m_tun = new TunAdapter(this);
    connect(m_tun, &TunAdapter::firewallRuleChanged,
            this, [this](bool added, bool success) {
        if (added) {
            emit statusMessage(success
                ? QString::fromUtf8("防火墙规则已添加：允许 10.10.0.0/24 入站")
                : QString::fromUtf8("防火墙规则添加失败，游戏端口可能无法联通"));
        } else {
            emit statusMessage(success
                ? QString::fromUtf8("防火墙规则已删除")
                : QString::fromUtf8("防火墙规则删除失败（可能已不存在）"));
        }
    });
    if (!m_tun->initialize()) {
        emit errorOccurred(QString::fromUtf8("虚拟网卡初始化失败（需要管理员权限）"));
        delete m_tun; m_tun = nullptr;
        return;
    }
    int mtu = static_cast<int>(normalizeRoomMtu(m_roomMtu));
    if (!m_tun->configureIP(m_myVirtualIP, VNET_MASK, mtu)) {
        emit errorOccurred(QString::fromUtf8("虚拟网卡IP配置失败"));
        delete m_tun; m_tun = nullptr;
        return;
    }
    if (!m_tun->startSession()) {
        emit errorOccurred(QString::fromUtf8("虚拟网卡会话启动失败"));
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
    emit statusMessage(QString::fromUtf8("虚拟网卡已启动 IP=%1 MTU=%2")
                       .arg(virtualIPToString(m_myVirtualIP)).arg(mtu));
}

void RoomManager::teardownTun() {
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
    delete m_cipher;
    m_cipher = nullptr;
    m_encrypted = false;
    m_roomPassword.clear();
    if (!m_intermediate.isEmpty()) {
        crypto_wipe(reinterpret_cast<uint8_t*>(m_intermediate.data()), m_intermediate.size());
        m_intermediate.clear();
    }
    m_tunnel->removeAllPeers();
    if (m_tun) {
        m_tunnel->setTunAdapter(nullptr);
        m_tun->shutdown();
        delete m_tun;
        m_tun = nullptr;
    }
}

void RoomManager::onTcpRelayHealthCheck() {
    QList<uint32_t> deadPeers;

    for (P2PPeer* peer : m_tunnel->allPeers()) {
        if (peer->transport() != TRANSPORT_RELAY_TCP) continue;
        peer->sendTcpRelayKeepalive();
        if (peer->isTcpRelayDead()) {
            deadPeers.append(peer->peerId());
        }
    }

    for (uint32_t pid : deadPeers) {
        P2PPeer* peer = m_tunnel->peerById(pid);
        if (!peer) continue;
        QString name = peer->name();
        uint32_t vip = peer->virtualIP();

        emit statusMessage(QString::fromUtf8("%1 TCP中继超时断开，正在尝试重建连接...")
                           .arg(name));

        m_tunnel->removePeer(pid);
        peer = m_tunnel->addPeer(pid, vip, name);
        if (!peer) continue;
        if (m_cipher) peer->setCipher(m_cipher);
        connect(peer, &P2PPeer::latencyPongReply,
                this, &RoomManager::onLatencyPongReply);
        emit peerTransportChanged(pid, TRANSPORT_NONE);
        setupTcpRelayTunnel(pid);
    }
}

void RoomManager::onLatencyUpdate() {
    for (P2PPeer* peer : m_tunnel->allPeers()) {
        if (peer->transport() == TRANSPORT_RELAY_TCP)
            peer->sendLatencyPing();
        int lat = peer->latencyMs();
        if (lat >= 0)
            emit peerLatencyUpdated(peer->peerId(), lat);
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

void RoomManager::onLatencyPongReply(uint32_t peerId, QByteArray pongData) {
    if (m_dataChannel && m_dataChannel->isConnected())
        m_dataChannel->sendRelayData(myPeerId(), peerId, pongData);
}

void RoomManager::onTunnelDead(uint32_t peerId) {
    P2PPeer* peer = m_tunnel->peerById(peerId);
    if (!peer) return;
    QString name = peer->name();
    uint32_t vip = peer->virtualIP();
    QHostAddress savedPubAddr = peer->publicAddress();
    quint16 savedPubPort = peer->publicPort();

    emit statusMessage(QString::fromUtf8("%1 隧道超时断开，正在尝试重建连接...")
                       .arg(name));

    m_tunnel->removePeer(peerId);
    peer = m_tunnel->addPeer(peerId, vip, name);
    if (!peer) return;
    if (m_cipher) peer->setCipher(m_cipher);
    connect(peer, &P2PPeer::latencyPongReply,
            this, &RoomManager::onLatencyPongReply);

    emit peerTransportChanged(peerId, TRANSPORT_NONE);

    if (m_transportMode == MODE_RELAY_TCP) {
        setupTcpRelayTunnel(peerId);
    } else if (m_transportMode == MODE_RELAY_KCP) {
        m_signal->requestRelay(peerId);
    } else if (m_transportMode == MODE_RELAY_RAW_UDP) {
        m_signal->requestRelay(peerId);
    } else if (m_transportMode == MODE_P2P_ONLY) {
        if (!savedPubAddr.isNull() && savedPubPort != 0) {
            peer->setPublicEndpoint(savedPubAddr, savedPubPort);
            initiatePunch(peerId, savedPubAddr.toIPv4Address(), savedPubPort);
        }
    }
}

void RoomManager::handleReconnectRoomList(QList<RoomListItem> rooms) {
    m_wantReconnect = false;

    uint32_t foundRoomId = 0;
    for (int i = 0; i < rooms.size(); ++i) {
        if (QString::fromUtf8(rooms[i].roomName) == m_savedRoomName) {
            foundRoomId = rooms[i].roomId;
            break;
        }
    }

    if (foundRoomId != 0) {
        emit statusMessage(QString::fromUtf8("找到房间 \"%1\" (ID=%2)，正在重新加入...")
                           .arg(m_savedRoomName).arg(foundRoomId));
        if (m_savedEncrypted && !m_savedRoomPassword.isEmpty())
            joinRoom(foundRoomId, m_savedRoomPassword);
        else
            joinRoom(foundRoomId, QString());
    } else {
        emit statusMessage(QString::fromUtf8("房间 \"%1\" 已不存在，正在重新创建...")
                           .arg(m_savedRoomName));
        createRoom(m_savedRoomName, m_savedMaxPlayers,
                   m_savedTransportMode, m_savedFecMode,
                   m_savedRoomMtu, m_savedEncrypted, m_savedRoomPassword);
    }
}

} // namespace VLan

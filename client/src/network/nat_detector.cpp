#include "nat_detector.h"
#include "net_common.h"
#include "../ui/log_manager.h"
#include <cstring>
#include <random>

namespace VLan {

NatDetector::NatDetector(QUdpSocket* socket, QObject* parent)
    : QObject(parent),
      m_socket(socket), m_stunPort(0),
      m_token(0), m_myPeerId(0),
      m_observedIP(0), m_observedPort(0),
      m_gotResponse(false), m_retryCount(0)
{
    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    connect(m_timer, &QTimer::timeout, this, &NatDetector::onTimeout);
}

void NatDetector::detect(const QHostAddress& serverAddr, quint16 stunPort)
{
    m_serverAddr = serverAddr;
    m_stunPort   = stunPort;
    m_gotResponse = false;
    m_retryCount = 0;
    std::random_device rd;
    m_token = rd();

    sendProbe();
    m_timer->start(2000);
}

void NatDetector::sendProbe() {
    StunRequest req;
    memset(&req, 0, sizeof(req));
    req.type      = UDP_STUN_REQUEST;
    req.token     = htonl(m_token);
    req.localPort = htons(m_socket->localPort());
    req.peerId    = htonl(m_myPeerId);

    LogManager::instance().logDetail(QString("[NAT] sendProbe to %1:%2 token=%3 localPort=%4 retry=%5").arg(m_serverAddr.toString()).arg(m_stunPort).arg(m_token).arg(m_socket->localPort()).arg(m_retryCount));

    m_socket->writeDatagram(reinterpret_cast<const char*>(&req), sizeof(req),
                            m_serverAddr, m_stunPort);
}

void NatDetector::handleStunResponse(const uint8_t* data, size_t len) {
    if (len < sizeof(StunResponse)) return;
    const StunResponse* resp = reinterpret_cast<const StunResponse*>(data);
    if (resp->type != UDP_STUN_RESPONSE) return;
    if (ntohl(resp->token) != m_token) return;
    if (m_gotResponse) return;

    m_observedIP   = ntohl(resp->observedIP);
    m_observedPort = ntohs(resp->observedPort);
    m_gotResponse  = true;

    LogManager::instance().logDetail(QString("[NAT] STUN response: observed=%1:%2").arg(QString::fromStdString(ipToString(m_observedIP))).arg(m_observedPort));

    m_timer->stop();

    NatType result;
    if (m_observedPort == m_socket->localPort())
        result = NAT_FULL_CONE;
    else
        result = NAT_SYMMETRIC;

    LogManager::instance().logDetail(QString("[NAT] Result: %1 (localPort=%2 observedPort=%3) public %4:%5").arg(natTypeName(result)).arg(m_socket->localPort()).arg(m_observedPort).arg(QString::fromStdString(ipToString(m_observedIP))).arg(m_observedPort));

    emit detected(result, m_observedIP, m_observedPort);
}

void NatDetector::onTimeout() {
    if (m_retryCount < 3) {
        ++m_retryCount;
        LogManager::instance().logDetail(QString("[NAT] Timeout, retry=%1").arg(m_retryCount));
        sendProbe();
        m_timer->start(2000);
    } else {
        LogManager::instance().logDetail(QString("[NAT] STUN probe failed after retries"));
        emit detected(NAT_UNKNOWN, 0, 0);
    }
}

} // namespace VLan

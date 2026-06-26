#include "hole_puncher.h"
#include "net_common.h"
#include "../ui/log_manager.h"
#include <cstring>
#include <random>

namespace VLan {

HolePuncher::HolePuncher(QUdpSocket* socket, uint32_t myPeerId, QObject* parent)
    : QObject(parent), m_socket(socket), m_myPeerId(myPeerId)
{
    m_retryTimer = new QTimer(this);
    connect(m_retryTimer, &QTimer::timeout, this, &HolePuncher::onRetryTimer);
}

void HolePuncher::startPunch(uint32_t targetPeerId,
                              const QHostAddress& targetAddr, quint16 targetPort)
{
    PunchAttempt& a = m_attempts[targetPeerId];
    a.targetPeerId = targetPeerId;
    a.targetAddr   = targetAddr;
    a.targetPort   = targetPort;
    std::random_device rd;
    a.token        = rd();
    a.attempts     = 0;
    a.ackReceived  = false;

    sendPunchPacket(a);

    if (!m_retryTimer->isActive())
        m_retryTimer->start(PUNCH_RETRY_INTERVAL);

    LogManager::instance().logDetail(QString("[punch] Start punching peer %1 at %2:%3").arg(targetPeerId).arg(targetAddr.toString()).arg(targetPort));
}

void HolePuncher::cancelPunch(uint32_t targetPeerId) {
    m_attempts.remove(targetPeerId);
    if (m_attempts.isEmpty()) m_retryTimer->stop();
}

void HolePuncher::sendPunchPacket(const PunchAttempt& a) {
    PunchPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.type   = UDP_PUNCH;
    pkt.peerId = htonl(m_myPeerId);
    pkt.token  = htonl(a.token);

    LogManager::instance().logDetail(QString("[punch] sendPunchPacket target=%1:%2 token=%3 attempt=%4").arg(a.targetAddr.toString()).arg(a.targetPort).arg(a.token).arg(a.attempts));

    m_socket->writeDatagram(reinterpret_cast<const char*>(&pkt), sizeof(pkt),
                            a.targetAddr, a.targetPort);
}

void HolePuncher::sendAck(uint32_t peerId, uint32_t token,
                           const QHostAddress& addr, quint16 port)
{
    PunchPacket ack;
    memset(&ack, 0, sizeof(ack));
    ack.type   = UDP_PUNCH_ACK;
    ack.peerId = htonl(m_myPeerId);
    ack.token  = htonl(token);

    m_socket->writeDatagram(reinterpret_cast<const char*>(&ack), sizeof(ack),
                            addr, port);
}

void HolePuncher::handleIncomingPacket(const uint8_t* data, size_t len,
                                       const QHostAddress& from, quint16 fromPort)
{
    if (len < sizeof(PunchPacket)) return;
    const PunchPacket* pkt = reinterpret_cast<const PunchPacket*>(data);
    uint32_t remotePeerId = ntohl(pkt->peerId);
    uint32_t token        = ntohl(pkt->token);

    if (pkt->type == UDP_PUNCH) {
        // Respond with ACK
        sendAck(remotePeerId, token, from, fromPort);

        // If we're also trying to punch this peer, mark success
        auto it = m_attempts.find(remotePeerId);
        if (it != m_attempts.end() && !it.value().ackReceived) {
            it.value().ackReceived = true;
            LogManager::instance().logDetail(QString("[punch] Received punch from peer %1 - bidirectional hole open").arg(remotePeerId));
            emit punchSucceeded(remotePeerId, from, fromPort);
            m_attempts.remove(remotePeerId);
            if (m_attempts.isEmpty()) m_retryTimer->stop();
        }
    } else if (pkt->type == UDP_PUNCH_ACK) {
        auto it = m_attempts.find(remotePeerId);
        if (it != m_attempts.end() && !it.value().ackReceived) {
            it.value().ackReceived = true;
            LogManager::instance().logDetail(QString("[punch] ACK from peer %1 at %2:%3").arg(remotePeerId).arg(from.toString()).arg(fromPort));
            emit punchSucceeded(remotePeerId, from, fromPort);
            m_attempts.remove(remotePeerId);
            if (m_attempts.isEmpty()) m_retryTimer->stop();
        }
    }
}

void HolePuncher::onRetryTimer() {
    QList<uint32_t> failed;

    for (auto it = m_attempts.begin(); it != m_attempts.end(); ++it) {
        PunchAttempt& a = it.value();
        if (a.ackReceived) continue;

        a.attempts++;
        int maxAttempts = PUNCH_TIMEOUT_MS / PUNCH_RETRY_INTERVAL;
        LogManager::instance().logDetail(QString("[punch] retry peer=%1 attempt=%2/%3").arg(it.key()).arg(a.attempts).arg(maxAttempts));
        if (a.attempts >= maxAttempts) {
            failed.append(it.key());
        } else {
            sendPunchPacket(a);
        }
    }

    for (uint32_t pid : failed) {
        LogManager::instance().logDetail(QString("[punch] Timeout for peer %1").arg(pid));
        m_attempts.remove(pid);
        emit punchFailed(pid);
    }

    if (m_attempts.isEmpty()) m_retryTimer->stop();
}

} // namespace VLan

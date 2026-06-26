#ifndef VLAN_HOLE_PUNCHER_H
#define VLAN_HOLE_PUNCHER_H

#include <QObject>
#include <QUdpSocket>
#include <QHostAddress>
#include <QTimer>
#include <QMap>
#include "protocol.h"

namespace VLan {

/*
 * Attempts UDP hole punching with a remote peer.
 *
 * Both sides send PunchPackets to each other's public endpoint simultaneously.
 * If an ACK is received, the punch succeeded and a direct P2P path exists.
 */
class HolePuncher : public QObject {
    Q_OBJECT
public:
    explicit HolePuncher(QUdpSocket* socket, uint32_t myPeerId,
                         QObject* parent = nullptr);

    void startPunch(uint32_t targetPeerId,
                    const QHostAddress& targetAddr, quint16 targetPort);
    void cancelPunch(uint32_t targetPeerId);
    void handleIncomingPacket(const uint8_t* data, size_t len,
                              const QHostAddress& from, quint16 fromPort);

signals:
    void punchSucceeded(uint32_t peerId, QHostAddress addr, quint16 port);
    void punchFailed(uint32_t peerId);

private slots:
    void onRetryTimer();

private:
    struct PunchAttempt {
        uint32_t     targetPeerId;
        QHostAddress targetAddr;
        quint16      targetPort;
        uint32_t     token;
        int          attempts;
        bool         ackReceived;
    };

    void sendPunchPacket(const PunchAttempt& a);
    void sendAck(uint32_t peerId, uint32_t token,
                 const QHostAddress& addr, quint16 port);

    QUdpSocket* m_socket;
    uint32_t    m_myPeerId;
    QTimer*     m_retryTimer;
    QMap<uint32_t, PunchAttempt> m_attempts; // targetPeerId -> attempt
};

} // namespace VLan
#endif // VLAN_HOLE_PUNCHER_H

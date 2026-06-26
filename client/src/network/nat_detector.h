#ifndef VLAN_NAT_DETECTOR_H
#define VLAN_NAT_DETECTOR_H

#include <QObject>
#include <QUdpSocket>
#include <QHostAddress>
#include <QTimer>
#include "protocol.h"

namespace VLan {

/*
 * Sends a single STUN probe to the server UDP port to discover the
 * client's public IP:Port and register the UDP address with the server.
 */
class NatDetector : public QObject {
    Q_OBJECT
public:
    explicit NatDetector(QUdpSocket* socket, QObject* parent = nullptr);

    void setMyPeerId(uint32_t id) { m_myPeerId = id; }

    void detect(const QHostAddress& serverAddr, quint16 stunPort);

    uint32_t observedIP()       const { return m_observedIP; }
    uint16_t observedPort()     const { return m_observedPort; }

signals:
    void detected(VLan::NatType type, uint32_t publicIP, uint16_t publicPort);

private slots:
    void onTimeout();

public:
    void handleStunResponse(const uint8_t* data, size_t len);

private:
    void sendProbe();

    QUdpSocket*  m_socket;
    QHostAddress m_serverAddr;
    quint16      m_stunPort;
    QTimer*      m_timer;

    uint32_t m_token;
    uint32_t m_myPeerId;
    uint32_t m_observedIP;
    uint16_t m_observedPort;
    bool     m_gotResponse;
    int      m_retryCount;
};

} // namespace VLan
#endif // VLAN_NAT_DETECTOR_H

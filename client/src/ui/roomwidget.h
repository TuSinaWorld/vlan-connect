#ifndef VLAN_ROOMWIDGET_H
#define VLAN_ROOMWIDGET_H

#include <QWidget>
#include <QTableWidget>
#include <QLabel>
#include <QMap>
#include "protocol.h"

namespace VLan {

/*
 * Displays room member list and connection status for each peer.
 */
class RoomWidget : public QWidget {
    Q_OBJECT
public:
    explicit RoomWidget(QWidget* parent = nullptr);

    void setMyInfo(uint32_t peerId, uint32_t virtualIP);
    void clear();

    void setFecMode(VLan::FecMode mode);

public slots:
    void addPeer(uint32_t peerId, uint32_t virtualIP, QString name);
    void removePeer(uint32_t peerId);
    void updatePeerTransport(uint32_t peerId, VLan::TransportType type);
    void updatePeerLatency(uint32_t peerId, int latencyMs);
    void setNatType(VLan::NatType type);

private:
    void updateRow(int row, uint32_t peerId);
    int  findRow(uint32_t peerId);

    struct PeerEntry {
        uint32_t      peerId;
        uint32_t      virtualIP;
        QString       name;
        TransportType transport;
    };

    QLabel*       m_infoLabel;
    QTableWidget* m_table;
    QLabel*       m_natLabel;

    QMap<uint32_t, PeerEntry> m_peers;
    uint32_t m_myPeerId;
    uint32_t m_myVirtualIP;
    FecMode  m_fecMode;
};

} // namespace VLan
#endif // VLAN_ROOMWIDGET_H

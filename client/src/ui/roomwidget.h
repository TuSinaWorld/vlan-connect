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
    void setRoomContext(uint32_t roomId, uint16_t mtu);
    void clear();
    void retranslateUi();

    void setPolicies(VLan::RoomTrafficPolicy tcpPolicy,
                     VLan::RoomTrafficPolicy udpPolicy);

public slots:
    void addPeer(uint32_t peerId, uint32_t virtualIP, QString name);
    void removePeer(uint32_t peerId);
    void updatePeerTransport(uint32_t peerId, VLan::TrafficClass cls,
                             VLan::TransportType type);
    void updatePeerLatency(uint32_t peerId, VLan::TrafficClass cls,
                           int latencyMs);

private:
    int  findRow(uint32_t peerId);
    uint32_t peerIdFromRow(int row) const;
    void refreshInfoLabel();
    void refreshSummaryLabels();
    void refreshHeaders();
    void refreshPeerRow(uint32_t peerId);
    void showPeerDetail(uint32_t peerId);
    QString transportText(VLan::TrafficClass cls, VLan::TransportType type) const;
    QString latencyText(int latencyMs) const;
    QString policySummary(VLan::TrafficClass cls) const;

    struct PeerEntry {
        uint32_t      peerId;
        uint32_t      virtualIP;
        QString       name;
        TransportType transport[3];
        int           latency[3];
    };

    QLabel*       m_titleLabel;
    QLabel*       m_infoLabel;
    QLabel*       m_roomStatusLabel;
    QLabel*       m_memberCountLabel;
    QTableWidget* m_table;

    QMap<uint32_t, PeerEntry> m_peers;
    uint32_t m_myPeerId;
    uint32_t m_myVirtualIP;
    uint32_t m_roomId;
    uint16_t m_roomMtu;
    RoomTrafficPolicy m_tcpPolicy;
    RoomTrafficPolicy m_udpPolicy;
};

} // namespace VLan
#endif // VLAN_ROOMWIDGET_H

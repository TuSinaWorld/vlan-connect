#include "roomwidget.h"
#include "ui_strings.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QAbstractItemView>
#include <QApplication>
#include <QClipboard>
#include <QDialog>
#include <QFrame>
#include <QFont>
#include <QColor>
#include <QBrush>
#include <QList>
#include <QPushButton>
#include <QScrollArea>
#include <QVariant>
#include <QEvent>
#include <QFontMetrics>
#include <QTimer>
#include <algorithm>

namespace VLan {

RoomWidget::RoomWidget(QWidget* parent)
    : QWidget(parent),
      m_titleLabel(nullptr),
      m_infoLabel(nullptr),
      m_roomStatusLabel(nullptr),
      m_memberCountLabel(nullptr),
      m_qualityBanner(nullptr),
      m_table(nullptr),
      m_myPeerId(0),
      m_myVirtualIP(0),
      m_roomId(0),
      m_roomMtu(ROOM_MTU_DEFAULT),
      m_tcpPolicy(makeDefaultTcpPolicy()),
      m_udpPolicy(makeDefaultUdpPolicy())
{
    setObjectName("CardPanel");

    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(14, 12, 14, 12);
    layout->setSpacing(8);

    m_titleLabel = new QLabel();
    m_titleLabel->setObjectName("SectionTitle");
    layout->addWidget(m_titleLabel);

    QWidget* toolbar = new QWidget();
    toolbar->setObjectName("MembersToolbar");
    toolbar->setStyleSheet(
        "QWidget#MembersToolbar { background: #ffffff; border: 1px solid #dce9f6; border-radius: 8px; }"
        "QLabel#MembersInfoLabel { color: #147a72; font-size: 13px; font-weight: 800; }"
        "QLabel#MembersPill { color: #536274; background: #f4f8ff; border: 1px solid #dce9f6; border-radius: 8px; padding: 5px 10px; font-size: 12px; font-weight: 800; }");
    QHBoxLayout* infoRow = new QHBoxLayout(toolbar);
    infoRow->setContentsMargins(12, 8, 12, 8);
    infoRow->setSpacing(10);

    m_infoLabel = new QLabel();
    m_infoLabel->setObjectName("MembersInfoLabel");
    infoRow->addWidget(m_infoLabel);
    infoRow->addStretch();
    m_roomStatusLabel = new QLabel();
    m_roomStatusLabel->setObjectName("MembersPill");
    infoRow->addWidget(m_roomStatusLabel);
    m_memberCountLabel = new QLabel();
    m_memberCountLabel->setObjectName("MembersPill");
    infoRow->addWidget(m_memberCountLabel);

    layout->addWidget(toolbar);

    m_qualityBanner = new QLabel();
    m_qualityBanner->setObjectName("MemberQualityBanner");
    m_qualityBanner->setWordWrap(true);
    m_qualityBanner->setTextFormat(Qt::PlainText);
    m_qualityBanner->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_qualityBanner->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    layout->addWidget(m_qualityBanner);

    m_table = new QTableWidget(0, 7);
    m_table->setObjectName("MemberQualityTable");
    m_table->setTextElideMode(Qt::ElideRight);
    m_table->setMinimumWidth(0);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Fixed);
    m_table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Fixed);
    m_table->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Fixed);
    m_table->horizontalHeader()->setSectionResizeMode(6, QHeaderView::Fixed);
    m_table->setColumnWidth(0, 64);
    m_table->setColumnWidth(1, 110);
    m_table->setColumnWidth(3, 172);
    m_table->setColumnWidth(4, 86);
    m_table->setColumnWidth(5, 172);
    m_table->setColumnWidth(6, 86);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setShowGrid(false);
    m_table->verticalHeader()->setVisible(false);
    m_table->verticalHeader()->setDefaultSectionSize(32);
    m_table->setAlternatingRowColors(true);
    layout->addWidget(m_table);
    connect(m_table, &QTableWidget::cellDoubleClicked,
            this, [this](int row, int) {
        uint32_t peerId = peerIdFromRow(row);
        if (peerId != 0)
            showPeerDetail(peerId);
    });

    retranslateUi();
}

void RoomWidget::setMyInfo(uint32_t peerId, uint32_t virtualIP) {
    m_myPeerId    = peerId;
    m_myVirtualIP = virtualIP;
    refreshInfoLabel();
    refreshSummaryLabels();
    const QList<uint32_t> keys = m_peers.keys();
    for (uint32_t id : keys)
        refreshPeerRow(id);
}

void RoomWidget::setRoomContext(uint32_t roomId, uint16_t mtu) {
    m_roomId = roomId;
    m_roomMtu = normalizeRoomMtu(mtu);
    refreshSummaryLabels();
}

void RoomWidget::clear() {
    m_table->setRowCount(0);
    m_peers.clear();
    m_tcpPolicy = makeDefaultTcpPolicy();
    m_udpPolicy = makeDefaultUdpPolicy();
    m_myPeerId = 0;
    m_myVirtualIP = 0;
    m_roomId = 0;
    m_roomMtu = ROOM_MTU_DEFAULT;
    refreshInfoLabel();
    refreshSummaryLabels();
}

void RoomWidget::setPolicies(RoomTrafficPolicy tcpPolicy,
                             RoomTrafficPolicy udpPolicy) {
    m_tcpPolicy = normalizeTrafficPolicy(tcpPolicy.transportMode,
                                         tcpPolicy.fecMode,
                                         tcpPolicy.kcpProfile,
                                         makeDefaultTcpPolicy());
    m_udpPolicy = normalizeTrafficPolicy(udpPolicy.transportMode,
                                         udpPolicy.fecMode,
                                         udpPolicy.kcpProfile,
                                         makeDefaultUdpPolicy());
    const QList<uint32_t> keys = m_peers.keys();
    for (uint32_t peerId : keys)
        refreshPeerRow(peerId);
}

void RoomWidget::retranslateUi() {
    if (m_titleLabel)
        m_titleLabel->setText(UiStrings::text("members.title"));
    refreshHeaders();
    refreshInfoLabel();
    refreshSummaryLabels();
    const QList<uint32_t> keys = m_peers.keys();
    for (uint32_t peerId : keys)
        refreshPeerRow(peerId);
}

void RoomWidget::addPeer(uint32_t peerId, uint32_t virtualIP, QString name) {
    bool isNewPeer = !m_peers.contains(peerId);
    PeerEntry& e = m_peers[peerId];
    e.peerId    = peerId;
    e.virtualIP = virtualIP;
    e.name      = name;
    if (isNewPeer) {
        for (int i = 0; i < 3; ++i) {
            e.transport[i] = TRANSPORT_NONE;
            e.lastTransport[i] = TRANSPORT_NONE;
            e.latency[i] = -1;
        }
    }

    int row = findRow(peerId);
    if (row < 0) {
        row = m_table->rowCount();
        m_table->insertRow(row);
        for (int col = 0; col < m_table->columnCount(); ++col) {
            QTableWidgetItem* item = new QTableWidgetItem();
            item->setTextAlignment(Qt::AlignCenter);
            m_table->setItem(row, col, item);
        }
    }
    if (QTableWidgetItem* idItem = m_table->item(row, 0)) {
        idItem->setText(QString::number(peerId));
        idItem->setData(Qt::UserRole, peerId);
    }
    refreshPeerRow(peerId);
    refreshSummaryLabels();
}

void RoomWidget::removePeer(uint32_t peerId) {
    int row = findRow(peerId);
    if (row >= 0) m_table->removeRow(row);
    m_peers.remove(peerId);
    refreshSummaryLabels();
}

void RoomWidget::updatePeerTransport(uint32_t peerId, TrafficClass cls,
                                     TransportType type) {
    if (!m_peers.contains(peerId)) return;
    int idx = static_cast<int>(cls);
    if (idx < 0 || idx > 2) idx = TRAFFIC_UDP;
    if (type != TRANSPORT_NONE) {
        if (m_peers[peerId].lastTransport[idx] != TRANSPORT_NONE &&
            m_peers[peerId].lastTransport[idx] != type)
            m_peers[peerId].quality[idx] = ProbeQualitySnapshot();
        m_peers[peerId].lastTransport[idx] = type;
    }
    m_peers[peerId].transport[idx] = type;
    refreshPeerRow(peerId);
    refreshQualityBanner();
}

void RoomWidget::updatePeerLatency(uint32_t peerId, TrafficClass cls,
                                   int latencyMs) {
    if (!m_peers.contains(peerId)) return;
    int idx = static_cast<int>(cls);
    if (idx < 0 || idx > 2) idx = TRAFFIC_UDP;
    m_peers[peerId].latency[idx] = latencyMs;
    refreshPeerRow(peerId);
}

void RoomWidget::updatePeerQuality(uint32_t peerId, TrafficClass cls,
                                  ProbeQualitySnapshot quality) {
    if (!m_peers.contains(peerId) || peerId == m_myPeerId) return;
    int idx = static_cast<int>(cls);
    if (idx < 0 || idx > 2) idx = TRAFFIC_UDP;
    m_peers[peerId].quality[idx] = quality;
    m_peers[peerId].latency[idx] = quality.rttMs;
    refreshPeerRow(peerId);
    refreshQualityBanner();
}

void RoomWidget::refreshInfoLabel() {
    if (!m_infoLabel) return;
    if (m_myPeerId == 0) {
        m_infoLabel->setText(UiStrings::text("members.notJoined"));
        return;
    }
    m_infoLabel->setText(UiStrings::text("members.myInfo")
        .arg(m_myPeerId).arg(virtualIPToString(m_myVirtualIP)));
}

void RoomWidget::refreshSummaryLabels() {
    if (m_roomStatusLabel) {
        m_roomStatusLabel->setText(m_roomId == 0
            ? UiStrings::text("members.roomNone")
            : UiStrings::text("members.roomStatus").arg(m_roomId).arg(m_roomMtu));
    }
    if (m_memberCountLabel) {
        int totalMembers = m_myPeerId == 0 ? 0 :
            (m_peers.size() + (m_peers.contains(m_myPeerId) ? 0 : 1));
        m_memberCountLabel->setText(UiStrings::text("members.memberCount").arg(totalMembers));
    }
    refreshQualityBanner();
}

void RoomWidget::refreshHeaders() {
    if (!m_table) return;
    m_table->setHorizontalHeaderLabels(QStringList()
        << UiStrings::text("members.col.peerId")
        << UiStrings::text("members.col.virtualIp")
        << UiStrings::text("members.col.name")
        << UiStrings::text("members.col.tcp")
        << UiStrings::text("members.col.tcpRtt")
        << UiStrings::text("members.col.udp")
        << UiStrings::text("members.col.udpRtt"));
    refreshQualityGeometry();
}

void RoomWidget::refreshPeerRow(uint32_t peerId) {
    int row = findRow(peerId);
    if (row < 0 || !m_peers.contains(peerId)) return;
    const PeerEntry& e = m_peers[peerId];

    QString values[7] = {
        QString::number(e.peerId),
        e.virtualIP ? virtualIPToString(e.virtualIP) : QStringLiteral("-"),
        e.name,
        e.peerId == m_myPeerId ? QString::fromUtf8("—") : transportText(TRAFFIC_TCP, e.transport[TRAFFIC_TCP]),
        qualityText(e, TRAFFIC_TCP),
        e.peerId == m_myPeerId ? QString::fromUtf8("—") : transportText(TRAFFIC_UDP, e.transport[TRAFFIC_UDP]),
        qualityText(e, TRAFFIC_UDP)
    };

    const bool abnormal = e.peerId != m_myPeerId &&
        (e.quality[TRAFFIC_TCP].outage || e.quality[TRAFFIC_TCP].localSendFailure ||
         e.quality[TRAFFIC_UDP].outage || e.quality[TRAFFIC_UDP].localSendFailure);

    for (int col = 0; col < 7; ++col) {
        QTableWidgetItem* item = m_table->item(row, col);
        if (!item) {
            item = new QTableWidgetItem();
            item->setTextAlignment(Qt::AlignCenter);
            m_table->setItem(row, col, item);
        }
        item->setText(values[col]);
        item->setBackground(abnormal ? QBrush(QColor("#fff5d9")) : QBrush());
        item->setToolTip(col == 4 || col == 6
            ? qualityDetails(e.quality[col == 4 ? TRAFFIC_TCP : TRAFFIC_UDP]) +
              QLatin1Char('\n') + UiStrings::text("quality.description") : QString());
        if (col == 4 || col == 6)
            item->setForeground(QBrush());
    }

    int tcpLatency = e.latency[TRAFFIC_TCP];
    int udpLatency = e.latency[TRAFFIC_UDP];
    QTableWidgetItem* tcpItem = m_table->item(row, 4);
    QTableWidgetItem* udpItem = m_table->item(row, 6);
    if (tcpItem && tcpLatency >= 0) {
        if (tcpLatency < 50) tcpItem->setForeground(QColor("#10b981"));
        else if (tcpLatency < 100) tcpItem->setForeground(QColor("#3b82f6"));
        else if (tcpLatency < 200) tcpItem->setForeground(QColor("#f59e0b"));
        else tcpItem->setForeground(QColor("#ef4444"));
    }
    if (udpItem && udpLatency >= 0) {
        if (udpLatency < 50) udpItem->setForeground(QColor("#10b981"));
        else if (udpLatency < 100) udpItem->setForeground(QColor("#3b82f6"));
        else if (udpLatency < 200) udpItem->setForeground(QColor("#f59e0b"));
        else udpItem->setForeground(QColor("#ef4444"));
    }
    if (tcpItem && (e.quality[TRAFFIC_TCP].outage || e.quality[TRAFFIC_TCP].localSendFailure))
        tcpItem->setForeground(QColor("#9a5800"));
    if (udpItem && (e.quality[TRAFFIC_UDP].outage || e.quality[TRAFFIC_UDP].localSendFailure))
        udpItem->setForeground(QColor("#9a5800"));
}

void RoomWidget::refreshQualityGeometry() {
    if (!m_table) return;
    const QFontMetrics metrics(m_table->font());
    int qualityWidth = metrics.width(UiStrings::text("quality.rate").arg("100.0"));
    const char* qualityKeys[] = {"members.col.tcpRtt", "members.col.udpRtt",
                                "quality.sendFailure", "quality.noReply", "quality.sampling"};
    for (const char* key : qualityKeys)
        qualityWidth = std::max(qualityWidth, metrics.width(UiStrings::text(key)));
    qualityWidth += 20;
    m_table->horizontalHeader()->setMinimumSectionSize(
        std::max(48, metrics.width(UiStrings::text("members.col.name")) + 20));
    m_table->setColumnWidth(0, std::max(64, metrics.width(UiStrings::text("members.col.peerId")) + 20));
    m_table->setColumnWidth(1, std::max(110, metrics.width(QStringLiteral("255.255.255.255")) + 20));
    const int transportWidth = std::max(172,
        std::max(metrics.width(UiStrings::text("members.kcpRealtime") + QStringLiteral(" +100%")),
                 std::max(metrics.width(UiStrings::text("members.col.tcp")),
                          metrics.width(UiStrings::text("members.col.udp")))) + 20);
    m_table->setColumnWidth(3, transportWidth);
    m_table->setColumnWidth(5, transportWidth);
    m_table->setColumnWidth(4, qualityWidth);
    m_table->setColumnWidth(6, qualityWidth);
    m_table->verticalHeader()->setDefaultSectionSize(std::max(44, metrics.lineSpacing() * 2 + 14));
}

void RoomWidget::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::FontChange || event->type() == QEvent::ApplicationFontChange ||
        event->type() == QEvent::StyleChange)
        refreshQualityGeometry();
}

void RoomWidget::refreshQualityBanner() {
    if (!m_qualityBanner) return;
    int remoteCount = 0;
    int abnormalCount = 0;
    bool udpProblem = false, tcpProblem = false, localProblem = false;
    for (auto it = m_peers.constBegin(); it != m_peers.constEnd(); ++it) {
        const PeerEntry& peer = it.value();
        if (peer.peerId == m_myPeerId) continue;
        ++remoteCount;
        bool abnormal = false;
        for (int idx = TRAFFIC_TCP; idx <= TRAFFIC_UDP; ++idx) {
            const ProbeQualitySnapshot& quality = peer.quality[idx];
            if (quality.localSendFailure) {
                localProblem = abnormal = true;
                continue;
            }
            if (!quality.outage) continue;
            abnormal = true;
            if (peer.lastTransport[idx] == TRANSPORT_RELAY_KCP ||
                peer.lastTransport[idx] == TRANSPORT_RELAY_RAW_UDP)
                udpProblem = true;
            else
                tcpProblem = true;
        }
        if (abnormal) ++abnormalCount;
    }
    if (m_myPeerId == 0 || (remoteCount > 0 && abnormalCount == 0)) {
        m_qualityBanner->hide();
        return;
    }
    if (remoteCount == 0) {
        m_qualityBanner->setText(UiStrings::text("quality.solo"));
        m_qualityBanner->setStyleSheet("QLabel#MemberQualityBanner { color: #536274; background: #f4f8ff; border: 1px solid #dce9f6; border-radius: 6px; padding: 8px 10px; }");
    } else {
        QStringList lines;
        lines << UiStrings::text("quality.abnormalMembers").arg(abnormalCount);
        if (udpProblem) lines << UiStrings::text("quality.banner.udp");
        if (tcpProblem) lines << UiStrings::text("quality.banner.tcp");
        if (localProblem) lines << UiStrings::text("quality.banner.local");
        m_qualityBanner->setText(lines.join(QLatin1Char('\n')));
        m_qualityBanner->setStyleSheet("QLabel#MemberQualityBanner { color: #8a5100; background: #fff5d9; border: 1px solid #edcd80; border-radius: 6px; padding: 8px 10px; }");
    }
    m_qualityBanner->show();
}

QString RoomWidget::qualityText(const PeerEntry& peer, TrafficClass cls) const {
    if (peer.peerId == m_myPeerId) return QString::fromUtf8("—");
    const ProbeQualitySnapshot& quality = peer.quality[static_cast<int>(cls)];
    QString latency = latencyText(quality.rttMs);
    if (quality.localSendFailure)
        latency = UiStrings::text("quality.sendFailure");
    else if (quality.outage)
        latency = UiStrings::text("quality.noReply");
    else if (peer.transport[static_cast<int>(cls)] == TRANSPORT_NONE)
        return UiStrings::text("quality.connecting");
    return latency + QLatin1Char('\n') + (quality.sampling
        ? UiStrings::text("quality.sampling")
        : UiStrings::text("quality.rate").arg(QString::number(quality.timeoutPercent, 'f', 1)));
}

QString RoomWidget::qualityDetails(const ProbeQualitySnapshot& quality) const {
    return UiStrings::text("quality.samples")
        .arg(quality.replies).arg(quality.timeouts).arg(quality.pending).arg(quality.sendFailures);
}

QString RoomWidget::transportText(TrafficClass cls, TransportType type) const {
    const RoomTrafficPolicy& policy = cls == TRAFFIC_TCP ? m_tcpPolicy : m_udpPolicy;
    QString text;
    switch (type) {
    case TRANSPORT_RELAY_KCP:
        text = policy.kcpProfile == KCP_PROFILE_BULK
            ? UiStrings::text("members.kcpBulk")
            : UiStrings::text("members.kcpRealtime");
        break;
    case TRANSPORT_RELAY_RAW_UDP:
        text = QStringLiteral("Raw UDP");
        break;
    case TRANSPORT_RELAY_TCP:
        text = QStringLiteral("TCP Relay");
        break;
    default:
        return UiStrings::text("members.disconnected");
    }

    if (policy.fecMode != FEC_NONE &&
        (type == TRANSPORT_RELAY_KCP || type == TRANSPORT_RELAY_RAW_UDP)) {
        text += QStringLiteral(" +%1").arg(fecModeName(policy.fecMode));
    }
    return text;
}

QString RoomWidget::latencyText(int latencyMs) const {
    if (latencyMs < 0)
        return QStringLiteral("-");
    return QStringLiteral("%1ms").arg(latencyMs);
}

QString RoomWidget::policySummary(TrafficClass cls) const {
    const RoomTrafficPolicy& policy = cls == TRAFFIC_TCP ? m_tcpPolicy : m_udpPolicy;
    QString mode;
    switch (policy.transportMode) {
    case MODE_RELAY_KCP:
        mode = policy.kcpProfile == KCP_PROFILE_BULK
            ? UiStrings::text("members.kcpBulk")
            : UiStrings::text("members.kcpRealtime");
        break;
    case MODE_RELAY_RAW_UDP:
        mode = QStringLiteral("Raw UDP");
        break;
    case MODE_RELAY_TCP:
        mode = QStringLiteral("TCP Relay");
        break;
    default:
        mode = UiStrings::text("members.disconnected");
        break;
    }

    QString fec = policy.fecMode == FEC_NONE
        ? UiStrings::text("value.none")
        : QString::fromLatin1(fecModeName(policy.fecMode));
    return UiStrings::text("members.policySummary").arg(mode).arg(fec);
}

void RoomWidget::showPeerDetail(uint32_t peerId) {
    if (!m_peers.contains(peerId))
        return;
    const PeerEntry e = m_peers.value(peerId);

    QDialog dialog(this);
    dialog.setObjectName("MemberDetailDialog");
    dialog.setWindowTitle(UiStrings::text("members.detail.title"));
    dialog.setMinimumSize(380, 420);
    dialog.resize(520, 560);
    dialog.setStyleSheet(
        "QDialog#MemberDetailDialog { background: #f7fbff; font-family: 'Microsoft YaHei', 'Segoe UI'; }"
        "QWidget#MemberDetailHeader { background: qlineargradient(x1:0,y1:0,x2:1,y2:1,stop:0 #ffffff,stop:0.55 #effcf9,stop:1 #f4f8ff); border: 1px solid #c9f2ea; border-radius: 8px; }"
        "QWidget#MemberDetailCard { background: #ffffff; border: 1px solid #dce9f6; border-radius: 8px; }"
        "QLabel#MemberDetailTitle { color: #243041; font-size: 18px; font-weight: 900; background: transparent; }"
        "QLabel#MemberDetailSubtitle { color: #647282; font-size: 12px; font-family: Consolas, 'Microsoft YaHei'; background: transparent; }"
        "QLabel#MemberDetailPill { color: #147a72; background: #eafff9; border: 1px solid #c9f2ea; border-radius: 8px; font-size: 11px; font-weight: 900; padding: 4px 10px; }"
        "QLabel#DetailKey { color: #647282; font-size: 12px; font-weight: 900; background: transparent; }"
        "QLabel#DetailValue { color: #394456; font-size: 12px; font-family: Consolas, 'Microsoft YaHei'; font-weight: 700; background: transparent; }"
        "QFrame#DetailLine { background: #eef3f7; border: none; }"
        "QPushButton#GhostBtn { background: #ffffff; color: #394456; border: 1px solid #dce9f6; border-radius: 8px; font-size: 13px; font-weight: 800; padding: 0 18px; }"
        "QPushButton#GhostBtn:hover { background: #f4f8ff; }"
        "QPushButton#PrimaryBtn { background: #2ec4b6; color: #ffffff; border: none; border-radius: 8px; font-size: 13px; font-weight: 900; padding: 0 18px; }"
        "QPushButton#PrimaryBtn:hover { background: #24a99d; }");

    QVBoxLayout* mainLayout = new QVBoxLayout(&dialog);
    mainLayout->setContentsMargins(20, 20, 20, 16);
    mainLayout->setSpacing(14);

    QWidget* header = new QWidget();
    header->setObjectName("MemberDetailHeader");
    QHBoxLayout* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(18, 14, 18, 14);
    QVBoxLayout* titleLayout = new QVBoxLayout();
    titleLayout->setSpacing(4);
    QLabel* title = new QLabel(UiStrings::text("members.detail.title"));
    title->setObjectName("MemberDetailTitle");
    QLabel* subtitle = new QLabel(e.name.isEmpty() ? QStringLiteral("-") : e.name);
    subtitle->setObjectName("MemberDetailSubtitle");
    titleLayout->addWidget(title);
    titleLayout->addWidget(subtitle);
    QLabel* tag = new QLabel(QStringLiteral("Peer %1").arg(e.peerId));
    tag->setObjectName("MemberDetailPill");
    tag->setAlignment(Qt::AlignCenter);
    headerLayout->addLayout(titleLayout, 1);
    headerLayout->addWidget(tag, 0, Qt::AlignRight | Qt::AlignVCenter);
    mainLayout->addWidget(header);

    QWidget* content = new QWidget();
    content->setObjectName("MemberDetailCard");
    QVBoxLayout* contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(18, 14, 18, 14);
    contentLayout->setSpacing(0);

    QList<QPair<QString, QLabel*> > detailRows;
    auto addInfoRow = [&](const QString& label, const QString& value, const QColor& color) -> QLabel* {
        QHBoxLayout* row = new QHBoxLayout();
        row->setContentsMargins(0, 8, 0, 8);
        row->setSpacing(14);
        QLabel* key = new QLabel(label);
        key->setObjectName("DetailKey");
        key->setFixedWidth(std::max(112, key->fontMetrics().width(label) + 4));
        QLabel* val = new QLabel(value);
        val->setObjectName("DetailValue");
        val->setWordWrap(true);
        val->setTextInteractionFlags(Qt::TextSelectableByMouse);
        if (color.isValid()) {
            val->setStyleSheet(QString("color: %1; font-size: 12px; font-family: Consolas, 'Microsoft YaHei'; font-weight: 800; background: transparent;")
                               .arg(color.name()));
        }
        row->addWidget(key);
        row->addWidget(val, 1);
        contentLayout->addLayout(row);
        QFrame* line = new QFrame();
        line->setObjectName("DetailLine");
        line->setFixedHeight(1);
        contentLayout->addWidget(line);
        detailRows.append(qMakePair(label, val));
        return val;
    };

    addInfoRow(UiStrings::text("members.detail.peerId"), QString::number(e.peerId), QColor("#147a72"));
    addInfoRow(UiStrings::text("members.detail.virtualIp"), e.virtualIP ? virtualIPToString(e.virtualIP) : QStringLiteral("-"), QColor());
    addInfoRow(UiStrings::text("members.detail.name"), e.name.isEmpty() ? QStringLiteral("-") : e.name, QColor());
    addInfoRow(UiStrings::text("members.detail.tcp"), transportText(TRAFFIC_TCP, e.transport[TRAFFIC_TCP]), QColor("#2ec4b6"));
    QLabel* tcpQuality = addInfoRow(UiStrings::text("members.detail.tcpRtt"), qualityText(e, TRAFFIC_TCP), QColor());
    QLabel* tcpSamples = addInfoRow(UiStrings::text("quality.detail.tcpSamples"), qualityDetails(e.quality[TRAFFIC_TCP]), QColor());
    addInfoRow(UiStrings::text("members.detail.udp"), transportText(TRAFFIC_UDP, e.transport[TRAFFIC_UDP]), QColor("#3b82f6"));
    QLabel* udpQuality = addInfoRow(UiStrings::text("members.detail.udpRtt"), qualityText(e, TRAFFIC_UDP), QColor());
    QLabel* udpSamples = addInfoRow(UiStrings::text("quality.detail.udpSamples"), qualityDetails(e.quality[TRAFFIC_UDP]), QColor());
    addInfoRow(UiStrings::text("members.detail.room"), m_roomId == 0 ? QStringLiteral("-") : QString::number(m_roomId), QColor());
    addInfoRow(UiStrings::text("members.detail.mtu"), QString::number(m_roomMtu), QColor());
    addInfoRow(UiStrings::text("members.detail.tcpPolicy"), policySummary(TRAFFIC_TCP), QColor());
    addInfoRow(UiStrings::text("members.detail.udpPolicy"), policySummary(TRAFFIC_UDP), QColor());
    addInfoRow(UiStrings::text("quality.detail.metric"), UiStrings::text("quality.description"), QColor());

    QScrollArea* scroll = new QScrollArea();
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet(QStringLiteral("QScrollArea { background: transparent; border: none; }"));
    scroll->setWidget(content);
    mainLayout->addWidget(scroll, 1);

    QHBoxLayout* footer = new QHBoxLayout();
    footer->setSpacing(10);
    QLabel* idLabel = new QLabel(QStringLiteral("ID: %1").arg(e.peerId));
    idLabel->setStyleSheet(QStringLiteral("color: #9fb0c0; font-size: 10px; font-family: Consolas;"));
    QPushButton* copyBtn = new QPushButton(UiStrings::text("members.detail.copy"));
    copyBtn->setObjectName("GhostBtn");
    copyBtn->setMinimumSize(76, 34);
    copyBtn->setCursor(Qt::PointingHandCursor);
    QPushButton* closeBtn = new QPushButton(UiStrings::text("members.detail.close"));
    closeBtn->setObjectName("PrimaryBtn");
    closeBtn->setMinimumSize(76, 34);
    closeBtn->setCursor(Qt::PointingHandCursor);
    connect(copyBtn, &QPushButton::clicked, [detailRows]() {
        QString copyText;
        for (const auto& info : detailRows)
            copyText += info.first + QStringLiteral(": ") + info.second->text() + QLatin1Char('\n');
        QApplication::clipboard()->setText(copyText.trimmed());
    });
    connect(closeBtn, &QPushButton::clicked, &dialog, &QDialog::accept);
    footer->addWidget(idLabel, 1);
    footer->addWidget(copyBtn);
    footer->addWidget(closeBtn);
    mainLayout->addLayout(footer);

    QTimer qualityTimer;
    connect(&qualityTimer, &QTimer::timeout, &dialog, [&, peerId]() {
        if (!m_peers.contains(peerId)) {
            dialog.reject();
            return;
        }
        const PeerEntry& current = m_peers[peerId];
        tcpQuality->setText(qualityText(current, TRAFFIC_TCP));
        udpQuality->setText(qualityText(current, TRAFFIC_UDP));
        tcpSamples->setText(qualityDetails(current.quality[TRAFFIC_TCP]));
        udpSamples->setText(qualityDetails(current.quality[TRAFFIC_UDP]));
    });
    qualityTimer.start(1000);
    dialog.exec();
}

int RoomWidget::findRow(uint32_t peerId) {
    QString idStr = QString::number(peerId);
    for (int i = 0; i < m_table->rowCount(); ++i) {
        QTableWidgetItem* item = m_table->item(i, 0);
        if (!item) continue;
        QVariant idData = item->data(Qt::UserRole);
        if (idData.isValid() && idData.toUInt() == peerId)
            return i;
        if (item->text() == idStr)
            return i;
    }
    return -1;
}

uint32_t RoomWidget::peerIdFromRow(int row) const {
    if (!m_table || row < 0 || row >= m_table->rowCount())
        return 0;
    QTableWidgetItem* item = m_table->item(row, 0);
    if (!item)
        return 0;
    QVariant idData = item->data(Qt::UserRole);
    if (idData.isValid())
        return idData.toUInt();
    return item->text().toUInt();
}

} // namespace VLan

#include "roomwidget.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QFont>
#include <QColor>

namespace VLan {

RoomWidget::RoomWidget(QWidget* parent)
    : QWidget(parent), m_myPeerId(0), m_myVirtualIP(0), m_fecMode(FEC_NONE)
{
    setObjectName("CardPanel");

    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(14, 12, 14, 12);
    layout->setSpacing(8);

    QLabel* title = new QLabel(QString::fromUtf8("\u623f\u95f4\u6210\u5458"));
    title->setObjectName("SectionTitle");
    layout->addWidget(title);

    QHBoxLayout* infoRow = new QHBoxLayout();
    infoRow->setSpacing(16);

    m_infoLabel = new QLabel(QString::fromUtf8("\u672a\u52a0\u5165\u623f\u95f4"));
    m_infoLabel->setObjectName("InfoLabel");
    infoRow->addWidget(m_infoLabel);

    m_natLabel = new QLabel(QString::fromUtf8("NAT: \u672a\u68c0\u6d4b"));
    m_natLabel->setObjectName("NatLabel");
    infoRow->addWidget(m_natLabel);
    infoRow->addStretch();

    layout->addLayout(infoRow);

    m_table = new QTableWidget(0, 5);
    m_table->setHorizontalHeaderLabels(
        QStringList()
        << "PeerID"
        << QString::fromUtf8("\u865a\u62dfIP")
        << QString::fromUtf8("\u6635\u79f0")
        << QString::fromUtf8("\u8fde\u63a5\u65b9\u5f0f")
        << QString::fromUtf8("\u5ef6\u8fdf"));
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Fixed);
    m_table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Fixed);
    m_table->setColumnWidth(0, 60);
    m_table->setColumnWidth(1, 110);
    m_table->setColumnWidth(3, 150);
    m_table->setColumnWidth(4, 70);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setShowGrid(false);
    m_table->verticalHeader()->setVisible(false);
    m_table->verticalHeader()->setDefaultSectionSize(32);
    m_table->setAlternatingRowColors(true);
    layout->addWidget(m_table);
}

void RoomWidget::setMyInfo(uint32_t peerId, uint32_t virtualIP) {
    m_myPeerId    = peerId;
    m_myVirtualIP = virtualIP;
    m_infoLabel->setText(
        QString::fromUtf8("\u6211\u7684ID: %1  |  \u865a\u62dfIP: %2")
        .arg(peerId).arg(virtualIPToString(virtualIP)));
}

void RoomWidget::clear() {
    m_table->setRowCount(0);
    m_peers.clear();
    m_fecMode = FEC_NONE;
    m_infoLabel->setText(QString::fromUtf8("\u672a\u52a0\u5165\u623f\u95f4"));
    m_natLabel->setText(QString::fromUtf8("NAT: \u672a\u68c0\u6d4b"));
}

void RoomWidget::setFecMode(FecMode mode) {
    m_fecMode = mode;
}

void RoomWidget::addPeer(uint32_t peerId, uint32_t virtualIP, QString name) {
    PeerEntry& e = m_peers[peerId];
    e.peerId    = peerId;
    e.virtualIP = virtualIP;
    e.name      = name;
    e.transport = TRANSPORT_NONE;

    int row = m_table->rowCount();
    m_table->insertRow(row);
    auto* idItem = new QTableWidgetItem(QString::number(peerId));
    idItem->setTextAlignment(Qt::AlignCenter);
    m_table->setItem(row, 0, idItem);

    auto* ipItem = new QTableWidgetItem(
        virtualIP ? virtualIPToString(virtualIP) : "-");
    ipItem->setTextAlignment(Qt::AlignCenter);
    m_table->setItem(row, 1, ipItem);

    auto* nameItem = new QTableWidgetItem(name);
    nameItem->setTextAlignment(Qt::AlignCenter);
    m_table->setItem(row, 2, nameItem);

    auto* connItem = new QTableWidgetItem(
        QString::fromUtf8("\u8fde\u63a5\u4e2d..."));
    connItem->setTextAlignment(Qt::AlignCenter);
    m_table->setItem(row, 3, connItem);

    auto* latItem = new QTableWidgetItem("-");
    latItem->setTextAlignment(Qt::AlignCenter);
    m_table->setItem(row, 4, latItem);
}

void RoomWidget::removePeer(uint32_t peerId) {
    int row = findRow(peerId);
    if (row >= 0) m_table->removeRow(row);
    m_peers.remove(peerId);
}

void RoomWidget::updatePeerTransport(uint32_t peerId, TransportType type) {
    if (!m_peers.contains(peerId)) return;
    m_peers[peerId].transport = type;

    int row = findRow(peerId);
    if (row < 0) return;

    QString text;
    switch (type) {
    case TRANSPORT_P2P_KCP:       text = QString::fromUtf8("P2P"); break;
    case TRANSPORT_RELAY_KCP:     text = QString::fromUtf8("KCP"); break;
    case TRANSPORT_RELAY_RAW_UDP: text = QString::fromUtf8("Raw UDP"); break;
    case TRANSPORT_RELAY_TCP:     text = QString::fromUtf8("TCP"); break;
    default:                      text = QString::fromUtf8("\u672a\u8fde\u63a5"); break;
    }
    if (m_fecMode != FEC_NONE &&
        (type == TRANSPORT_RELAY_KCP || type == TRANSPORT_RELAY_RAW_UDP)) {
        text += QString("+%1").arg(fecModeName(m_fecMode));
    }
    auto* item = new QTableWidgetItem(text);
    item->setTextAlignment(Qt::AlignCenter);
    m_table->setItem(row, 3, item);
}

void RoomWidget::updatePeerLatency(uint32_t peerId, int latencyMs) {
    int row = findRow(peerId);
    if (row < 0) return;

    QString text;
    if (latencyMs < 0)
        text = "-";
    else
        text = QString("%1ms").arg(latencyMs);

    auto* item = new QTableWidgetItem(text);
    item->setTextAlignment(Qt::AlignCenter);

    if (latencyMs >= 0 && latencyMs < 50)
        item->setForeground(QColor("#10b981"));
    else if (latencyMs >= 50 && latencyMs < 100)
        item->setForeground(QColor("#3b82f6"));
    else if (latencyMs >= 100 && latencyMs < 200)
        item->setForeground(QColor("#f59e0b"));
    else if (latencyMs >= 200)
        item->setForeground(QColor("#ef4444"));

    m_table->setItem(row, 4, item);
}

void RoomWidget::setNatType(NatType type) {
    m_natLabel->setText(
        QString::fromUtf8("NAT\u7c7b\u578b: %1").arg(natTypeName(type)));
}

int RoomWidget::findRow(uint32_t peerId) {
    QString idStr = QString::number(peerId);
    for (int i = 0; i < m_table->rowCount(); ++i) {
        if (m_table->item(i, 0) && m_table->item(i, 0)->text() == idStr)
            return i;
    }
    return -1;
}

} // namespace VLan

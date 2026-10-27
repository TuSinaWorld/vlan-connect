#include <QApplication>
#include <QFontMetrics>
#include <QLabel>
#include <QTableWidget>
#include <QHeaderView>
#include <QPixmap>
#include <QTimer>
#include <QDialog>
#include <QElapsedTimer>
#include "../../client/src/ui/roomwidget.h"
#include "../../client/src/ui/ui_strings.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdio>
#include <memory>

using namespace VLan;

static ProbeQualitySnapshot quality(unsigned replies, unsigned timeouts,
                                    int rtt, bool outage = false) {
    ProbeQualitySnapshot value;
    value.active = true;
    value.replies = replies;
    value.timeouts = timeouts;
    value.rttMs = rtt;
    value.outage = outage;
    const unsigned completed = replies + timeouts;
    value.sampling = completed < 5;
    value.timeoutPercent = completed ? 100.0 * timeouts / completed : 0;
    return value;
}

int main(int argc, char** argv) {
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication app(argc, argv);
    UiStrings::setLanguage(AppLanguage::Chinese);
    RoomWidget widget;
    widget.resize(1040, 550);
    widget.setMyInfo(1, 0x0a0a0002u);
    widget.setRoomContext(3200, ROOM_MTU_DEFAULT);
    widget.show();
    app.processEvents();
    QLabel* banner = widget.findChild<QLabel*>("MemberQualityBanner");
    QTableWidget* table = widget.findChild<QTableWidget*>("MemberQualityTable");
    assert(banner && table && table->columnCount() == 7);
    assert(!banner->isHidden() && banner->text() == UiStrings::text("quality.solo"));

    // The production view normally lists remote peers only. Defensively ignore
    // self notifications too, including their quality and aggregate warnings.
    widget.addPeer(1, 0x0a0a0002u, QStringLiteral("Self"));
    widget.updatePeerQuality(1, TRAFFIC_TCP, quality(0, 20, -1, true));
    assert(table->item(0, 4)->text() == QString::fromUtf8("—"));
    assert(banner->text() == UiStrings::text("quality.solo"));

    widget.addPeer(2, 0x0a0a0003u, QString::fromUtf8("玩家 A"));
    assert(banner->isHidden());
    assert(table->item(1, 4)->text() == UiStrings::text("quality.connecting"));
    widget.updatePeerTransport(2, TRAFFIC_TCP, TRANSPORT_RELAY_RAW_UDP);
    widget.updatePeerTransport(2, TRAFFIC_UDP, TRANSPORT_RELAY_KCP);
    assert(table->item(1, 4)->text().contains(UiStrings::text("quality.sampling")));
    widget.updatePeerQuality(2, TRAFFIC_TCP, quality(5, 0, 24));
    assert(table->item(1, 4)->text().contains("0.0%"));
    assert(table->item(1, 4)->toolTip().contains(UiStrings::text("quality.description")));
    assert(banner->isHidden());

    // The member dialog remains live while open.
    QTimer::singleShot(0, [&]() {
        QDialog* dialog = widget.findChild<QDialog*>("MemberDetailDialog");
        assert(dialog);
        widget.updatePeerQuality(2, TRAFFIC_TCP, quality(6, 1, 31));
        const QString expectedSamples = UiStrings::text("quality.samples")
            .arg(6).arg(1).arg(0).arg(0);
        QTimer* poll = new QTimer(dialog);
        std::shared_ptr<QElapsedTimer> deadline(new QElapsedTimer());
        deadline->start();
        QObject::connect(poll, &QTimer::timeout, dialog, [dialog, poll, deadline, expectedSamples]() {
            bool foundSamples = false;
            for (QLabel* label : dialog->findChildren<QLabel*>())
                foundSamples = foundSamples || label->text() == expectedSamples;
            if (!foundSamples && deadline->elapsed() < 3000)
                return;
            assert(foundSamples);
            poll->stop();
            dialog->accept();
        });
        poll->start(25);
    });
    assert(QMetaObject::invokeMethod(table, "cellDoubleClicked", Qt::DirectConnection,
                                    Q_ARG(int, 1), Q_ARG(int, 4)));

    widget.updatePeerQuality(2, TRAFFIC_TCP, quality(0, 12, -1, true));
    widget.updatePeerQuality(2, TRAFFIC_UDP, quality(0, 12, -1, true));
    assert(!banner->isHidden());
    assert(banner->text().contains(UiStrings::text("quality.banner.udp")));
    assert(banner->text().contains(UiStrings::text("quality.abnormalMembers").arg(1)));
    assert(table->item(1, 4)->text().contains("100.0%"));
    assert(table->item(1, 0)->background().color() == QColor("#fff5d9"));

    widget.addPeer(3, 0x0a0a0004u, QString::fromUtf8("玩家 B"));
    widget.updatePeerTransport(3, TRAFFIC_TCP, TRANSPORT_RELAY_TCP);
    widget.updatePeerTransport(3, TRAFFIC_UDP, TRANSPORT_RELAY_TCP);
    widget.updatePeerQuality(3, TRAFFIC_TCP, quality(60, 0, 42));
    widget.updatePeerQuality(3, TRAFFIC_UDP, quality(60, 0, 44));
    app.processEvents();
    if (argc > 1) assert(widget.grab().save(QString::fromLocal8Bit(argv[1])));

    widget.updatePeerQuality(3, TRAFFIC_UDP, quality(0, 15, -1, true));
    assert(banner->text().contains(UiStrings::text("quality.banner.tcp")));
    assert(banner->text().contains(UiStrings::text("quality.abnormalMembers").arg(2)));
    widget.removePeer(2);
    assert(!banner->text().contains(UiStrings::text("quality.banner.udp")));
    assert(banner->text().contains(UiStrings::text("quality.banner.tcp")));
    ProbeQualitySnapshot localFailure = quality(0, 0, -1);
    localFailure.localSendFailure = true;
    // Retained network outage must not misclassify a current local send error.
    localFailure.outage = true;
    localFailure.sendFailures = 2;
    widget.updatePeerQuality(3, TRAFFIC_UDP, localFailure);
    assert(banner->text().contains(UiStrings::text("quality.banner.local")));
    assert(!banner->text().contains(UiStrings::text("quality.banner.tcp")));
    widget.updatePeerQuality(3, TRAFFIC_UDP, quality(10, 0, 45));
    assert(banner->isHidden());
    assert(table->item(1, 0)->background().style() == Qt::NoBrush);
    widget.removePeer(3);
    assert(!banner->isHidden() && banner->text() == UiStrings::text("quality.solo"));

    widget.addPeer(2, 0x0a0a0003u, QStringLiteral("Rejoined"));
    assert(table->item(1, 4)->text() == UiStrings::text("quality.connecting"));
    assert(banner->isHidden());
    widget.updatePeerTransport(2, TRAFFIC_TCP, TRANSPORT_RELAY_RAW_UDP);
    widget.updatePeerQuality(2, TRAFFIC_TCP, quality(0, 20, -1, true));
    // Switching actual transport kind immediately drops the old quality.
    widget.updatePeerTransport(2, TRAFFIC_TCP, TRANSPORT_RELAY_TCP);
    assert(banner->isHidden());
    assert(table->item(1, 4)->text().contains(UiStrings::text("quality.sampling")));

    const int originalWidth = widget.width();
    QFont enlarged = widget.font();
    enlarged.setPixelSize(28);
    widget.setFont(enlarged);
    app.processEvents();
    const QFontMetrics metrics(table->font());
    assert(table->verticalHeader()->defaultSectionSize() >= metrics.lineSpacing() * 2 + 14);
    assert(table->columnWidth(4) >= metrics.width(UiStrings::text("quality.rate").arg("100.0")));
    assert(widget.width() == originalWidth);
    if (argc > 2) assert(widget.grab().save(QString::fromLocal8Bit(argv[2])));

    UiStrings::setLanguage(AppLanguage::English);
    widget.retranslateUi();
    assert(table->horizontalHeaderItem(4)->text() == QStringLiteral("TCP quality"));
    widget.removePeer(2);
    assert(banner->text() == UiStrings::text("quality.solo"));
    widget.clear();
    assert(banner->isHidden() && table->rowCount() == 0);
    std::puts("room_quality_ui_tests: PASS (solo, sampling, UDP/TCP/local warnings, recovery, removal, rejoin, fonts, translation)");
    return 0;
}

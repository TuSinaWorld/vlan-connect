#include "mainwindow.h"
#include "roomwidget.h"
#include "log_manager.h"
#include "modern_tray_menu.h"
#include "app_settings.h"
#include "ui_strings.h"
#include "../network/room_manager.h"
#include "../network/signal_client.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGridLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QDateTime>
#include <QFrame>
#include <QStyle>
#include <QScrollBar>
#include <QScrollArea>
#include <QDialog>
#include <QDialogButtonBox>
#include <QInputDialog>
#include <QScreen>
#include <QApplication>
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QTimer>
#include <QAbstractButton>
#include <QSignalBlocker>
#include <QVariant>
#include <string>

namespace VLan {

namespace {

void repolish(QWidget* widget)
{
    if (!widget) return;
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
}

void applyTextBinding(QWidget* widget)
{
    if (!widget) return;
    QVariant textKey = widget->property("i18nTextKey");
    if (textKey.isValid()) {
        QString text = UiStrings::text(textKey.toByteArray().constData());
        if (QLabel* label = qobject_cast<QLabel*>(widget)) {
            label->setText(text);
        } else if (QAbstractButton* button = qobject_cast<QAbstractButton*>(widget)) {
            button->setText(text);
        }
    }

    QVariant placeholderKey = widget->property("i18nPlaceholderKey");
    if (placeholderKey.isValid()) {
        if (QLineEdit* edit = qobject_cast<QLineEdit*>(widget))
            edit->setPlaceholderText(UiStrings::text(placeholderKey.toByteArray().constData()));
    }
}

void bindText(QWidget* widget, const char* key)
{
    if (!widget) return;
    widget->setProperty("i18nTextKey", QByteArray(key));
    applyTextBinding(widget);
}

void bindPlaceholder(QLineEdit* edit, const char* key)
{
    if (!edit) return;
    edit->setProperty("i18nPlaceholderKey", QByteArray(key));
    applyTextBinding(edit);
}

void retranslateTree(QWidget* root)
{
    if (!root) return;
    applyTextBinding(root);
    QList<QWidget*> widgets = root->findChildren<QWidget*>();
    for (QWidget* widget : widgets)
        applyTextBinding(widget);
}

QWidget* createCard(const char* titleKey, QVBoxLayout** bodyLayout = nullptr)
{
    QWidget* card = new QWidget();
    card->setObjectName("Card");

    QVBoxLayout* layout = new QVBoxLayout(card);
    layout->setContentsMargins(18, 16, 18, 18);
    layout->setSpacing(12);

    if (titleKey && titleKey[0] != '\0') {
        QLabel* label = new QLabel();
        label->setObjectName("CardTitle");
        bindText(label, titleKey);
        layout->addWidget(label);
    }

    if (bodyLayout) {
        *bodyLayout = layout;
    }
    return card;
}

QWidget* createMetricTile(const char* key, QLabel** valueLabel, const QString& initialValue)
{
    QWidget* tile = new QWidget();
    tile->setObjectName("MetricTile");
    QVBoxLayout* layout = new QVBoxLayout(tile);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(6);

    QLabel* keyLabel = new QLabel();
    keyLabel->setObjectName("MetricKey");
    bindText(keyLabel, key);
    layout->addWidget(keyLabel);

    QLabel* value = new QLabel(initialValue);
    value->setObjectName("MetricValue");
    value->setWordWrap(true);
    layout->addWidget(value);
    layout->addStretch();

    if (valueLabel) {
        *valueLabel = value;
    }
    return tile;
}

QString formatSpeed(quint64 bytesPerSec)
{
    if (bytesPerSec == 0)
        return QStringLiteral("0 B/s");
    double value = static_cast<double>(bytesPerSec);
    const char* unit = "B/s";
    if (value >= 1024.0) {
        value /= 1024.0;
        unit = "KB/s";
    }
    if (value >= 1024.0) {
        value /= 1024.0;
        unit = "MB/s";
    }
    if (value >= 1024.0) {
        value /= 1024.0;
        unit = "GB/s";
    }
    int precision = value >= 10.0 ? 0 : 1;
    return QString("%1 %2").arg(value, 0, 'f', precision).arg(unit);
}

QIcon createTrayIcon(bool active, bool connecting)
{
    QIcon base(QStringLiteral(":/icons/vlan.ico"));
    QPixmap pixmap = base.pixmap(32, 32);
    if (pixmap.isNull()) {
        pixmap = QPixmap(32, 32);
        pixmap.fill(Qt::transparent);
    }

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QColor dot = connecting ? QColor("#f5ad42")
                             : (active ? QColor("#2ec4b6") : QColor("#ff6b8f"));
    painter.setPen(QPen(QColor("#ffffff"), 2));
    painter.setBrush(dot);
    painter.drawEllipse(QRectF(20, 20, 10, 10));
    return QIcon(pixmap);
}

QString transportModeLabel(TransportMode mode)
{
    switch (mode) {
    case MODE_RELAY_KCP:     return QString::fromUtf8("KCP");
    case MODE_RELAY_RAW_UDP: return QString::fromUtf8("Raw UDP");
    case MODE_RELAY_TCP:     return QString::fromUtf8("TCP Relay");
    default:                 return QString::fromUtf8("?");
    }
}

QString policyLabel(const RoomTrafficPolicy& policy)
{
    QString text = transportModeLabel(policy.transportMode);
    if (policy.transportMode == MODE_RELAY_KCP) {
        text += policy.kcpProfile == KCP_PROFILE_BULK
            ? QStringLiteral("/bulk")
            : QStringLiteral("/realtime");
    }
    if (policy.fecMode != FEC_NONE)
        text += QString(" +%1").arg(fecModeName(policy.fecMode));
    return text;
}

} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent),
      m_roomWidget(nullptr),
      m_quitRequested(false),
      m_navBar(nullptr),
      m_mainStack(nullptr),
      m_pageTitleLabel(nullptr),
      m_pageSubtitleLabel(nullptr),
      m_dashConnectionLabel(nullptr),
      m_dashRoomLabel(nullptr),
      m_dashPeerLabel(nullptr),
      m_dashServerRttLabel(nullptr),
      m_trafficLabel(nullptr),
      m_trafficTitleLabel(nullptr),
      m_brandSubtitleLabel(nullptr),
      m_shellBadgeLabel(nullptr),
      m_trayIcon(nullptr),
      m_trayMenu(nullptr),
      m_showDetailLog(AppSettings::verboseLogsDefault()),
      m_languageBox(nullptr),
      m_settingsServerEdit(nullptr),
      m_settingsPortBox(nullptr),
      m_settingsNameEdit(nullptr),
      m_settingsVerboseCheck(nullptr),
      m_serverRttMs(-1),
      m_lastUploadRate(0),
      m_lastDownloadRate(0)
{
    UiStrings::setLanguage(AppSettings::language());
    m_roomMgr = new RoomManager(this);
    setupUI();
    loadPersistentSettings();
    initTray();

    connect(m_roomMgr, &RoomManager::connectionStatusChanged,
            this, &MainWindow::onConnectionStatusChanged);
    connect(m_roomMgr, &RoomManager::connectionStatusChanged,
            this, [this](bool connected) {
        if (m_quitRequested && !connected)
            finishProgramQuit();
    });
    connect(m_roomMgr, &RoomManager::loggedIn,
            this, &MainWindow::onLoggedIn);
    connect(m_roomMgr, &RoomManager::roomCreated,
            this, &MainWindow::onRoomCreated);
    connect(m_roomMgr, &RoomManager::roomJoined,
            this, &MainWindow::onRoomJoined);
    connect(m_roomMgr, &RoomManager::roomLeft,
            this, &MainWindow::onRoomLeft);
    connect(m_roomMgr, &RoomManager::roomListUpdated,
            this, &MainWindow::onRoomListUpdated);
    connect(m_roomMgr, &RoomManager::statusMessage,
            this, &MainWindow::onStatusMessage);
    connect(m_roomMgr, &RoomManager::errorOccurred,
            this, &MainWindow::onErrorOccurred);
    connect(m_roomMgr, &RoomManager::tunSpeedUpdated,
            this, &MainWindow::onTunSpeedUpdated);
    setRoomControlsEnabled(false);
    refreshDashboardState();
    updateTrayState();

    QScreen* screen = QApplication::primaryScreen();
    QRect avail = screen->availableGeometry();
    int minW = qBound(640, (int)(avail.width() * 0.55), 900);
    int minH = qBound(480, (int)(avail.height() * 0.55), 700);
    setMinimumSize(minW, minH);
    int defW = qBound(minW, (int)(avail.width() * 0.72), 1100);
    int defH = qBound(minH, (int)(avail.height() * 0.70), 700);
    resize(defW, defH);
}

MainWindow::~MainWindow() {}

void MainWindow::setupUI() {
    setWindowTitle(UiStrings::text("app.windowTitle"));

    QWidget* central = new QWidget(this);
    central->setObjectName("CentralWidget");
    setCentralWidget(central);

    QHBoxLayout* mainLayout = new QHBoxLayout(central);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    QScreen* screen = QApplication::primaryScreen();
    int screenWidth = screen ? screen->availableGeometry().width() : 1280;
    int navWidth = (screenWidth < 900) ? 204 : 244;

    QWidget* sidebar = new QWidget();
    sidebar->setObjectName("Sidebar");
    sidebar->setFixedWidth(navWidth);
    QVBoxLayout* sidebarLayout = new QVBoxLayout(sidebar);
    sidebarLayout->setContentsMargins(18, 18, 18, 18);
    sidebarLayout->setSpacing(18);

    QWidget* brand = new QWidget();
    brand->setObjectName("BrandPanel");
    QHBoxLayout* brandLayout = new QHBoxLayout(brand);
    brandLayout->setContentsMargins(12, 12, 12, 12);
    brandLayout->setSpacing(10);
    QLabel* brandMark = new QLabel(QStringLiteral("V"));
    brandMark->setObjectName("BrandMark");
    brandMark->setFixedSize(38, 38);
    brandMark->setAlignment(Qt::AlignCenter);
    QVBoxLayout* brandTextLayout = new QVBoxLayout();
    brandTextLayout->setContentsMargins(0, 0, 0, 0);
    brandTextLayout->setSpacing(2);
    QLabel* brandTitle = new QLabel(QStringLiteral("VLan"));
    brandTitle->setObjectName("BrandTitle");
    m_brandSubtitleLabel = new QLabel();
    m_brandSubtitleLabel->setObjectName("BrandSubtitle");
    bindText(m_brandSubtitleLabel, "brand.subtitle");
    brandTextLayout->addWidget(brandTitle);
    brandTextLayout->addWidget(m_brandSubtitleLabel);
    brandLayout->addWidget(brandMark);
    brandLayout->addLayout(brandTextLayout, 1);

    m_navBar = new QListWidget();
    m_navBar->setObjectName("NavList");
    m_navBar->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_navBar->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_navBar->addItem(UiStrings::text("nav.login"));
    m_navBar->addItem(UiStrings::text("nav.lobby"));
    m_navBar->addItem(UiStrings::text("nav.create"));
    m_navBar->addItem(UiStrings::text("nav.members"));
    m_navBar->addItem(UiStrings::text("nav.logs"));
    m_navBar->addItem(UiStrings::text("nav.settings"));
    m_navBar->setCurrentRow(PageLogin);
    connect(m_navBar, &QListWidget::currentRowChanged, this, &MainWindow::changePage);

    QWidget* trafficPanel = new QWidget();
    trafficPanel->setObjectName("SidebarTraffic");
    QVBoxLayout* trafficLayout = new QVBoxLayout(trafficPanel);
    trafficLayout->setContentsMargins(10, 8, 10, 8);
    trafficLayout->setSpacing(4);
    m_trafficTitleLabel = new QLabel();
    m_trafficTitleLabel->setObjectName("SidebarTrafficTitle");
    m_trafficTitleLabel->setAlignment(Qt::AlignCenter);
    bindText(m_trafficTitleLabel, "traffic.title");
    m_trafficLabel = new QLabel(UiStrings::text("traffic.value")
                                .arg(formatSpeed(0)).arg(formatSpeed(0)));
    m_trafficLabel->setObjectName("SidebarTrafficValue");
    m_trafficLabel->setAlignment(Qt::AlignCenter);
    m_trafficLabel->setWordWrap(true);
    trafficLayout->addWidget(m_trafficTitleLabel);
    trafficLayout->addWidget(m_trafficLabel);

    sidebarLayout->addWidget(brand);
    sidebarLayout->addWidget(m_navBar, 1);
    sidebarLayout->addWidget(trafficPanel);
    mainLayout->addWidget(sidebar);

    QWidget* content = new QWidget();
    content->setObjectName("ContentPane");
    QVBoxLayout* contentLayout = new QVBoxLayout(content);
    int contentMargin = (screenWidth < 900) ? 16 : 26;
    contentLayout->setContentsMargins(contentMargin, 22, contentMargin, contentMargin);
    contentLayout->setSpacing(16);

    QWidget* pageHeader = new QWidget();
    pageHeader->setObjectName("PageHeader");
    QHBoxLayout* pageHeaderLayout = new QHBoxLayout(pageHeader);
    pageHeaderLayout->setContentsMargins(0, 0, 0, 0);
    pageHeaderLayout->setSpacing(12);
    QVBoxLayout* titleLayout = new QVBoxLayout();
    titleLayout->setContentsMargins(0, 0, 0, 0);
    titleLayout->setSpacing(4);
    m_pageTitleLabel = new QLabel(UiStrings::text("page.login.title"));
    m_pageTitleLabel->setObjectName("PageTitle");
    m_pageSubtitleLabel = new QLabel(UiStrings::text("page.login.subtitle"));
    m_pageSubtitleLabel->setObjectName("PageSubtitle");
    titleLayout->addWidget(m_pageTitleLabel);
    titleLayout->addWidget(m_pageSubtitleLabel);
    m_shellBadgeLabel = new QLabel();
    m_shellBadgeLabel->setObjectName("ShellBadge");
    bindText(m_shellBadgeLabel, "shell.badge");
    pageHeaderLayout->addLayout(titleLayout, 1);
    pageHeaderLayout->addWidget(m_shellBadgeLabel, 0, Qt::AlignTop);
    contentLayout->addWidget(pageHeader);

    m_mainStack = new QStackedWidget();
    m_mainStack->setObjectName("MainStack");
    m_mainStack->addWidget(createLoginPage());
    m_mainStack->addWidget(createLobbyPage());
    m_mainStack->addWidget(createCreateRoomPage());
    m_mainStack->addWidget(createMembersPage());
    m_mainStack->addWidget(createLogPage());
    m_mainStack->addWidget(createSettingsPage());
    contentLayout->addWidget(m_mainStack, 1);

    mainLayout->addWidget(content, 1);

    // ======== Signal/Slot Connections ========
    connect(m_connectBtn, &QPushButton::clicked, this, &MainWindow::onConnectClicked);
    connect(m_createBtn,  &QPushButton::clicked, this, &MainWindow::onCreateRoomClicked);
    connect(m_joinBtn,    &QPushButton::clicked, this, &MainWindow::onJoinRoomClicked);
    connect(m_leaveBtn,   &QPushButton::clicked, this, &MainWindow::onLeaveRoomClicked);
    connect(m_refreshBtn, &QPushButton::clicked, this, &MainWindow::onRefreshClicked);
    connect(m_roomTable, &QTableWidget::cellDoubleClicked,
            this, [this](int, int) { onJoinRoomClicked(); });
    connect(m_advancedCheck, &QCheckBox::toggled, this, [this](bool checked) {
        m_advancedOptionsWidget->setVisible(checked);
        updatePolicyControlState();
    });
    connect(m_tcpModeBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onTransportModeChanged);
    connect(m_udpModeBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onTransportModeChanged);
    connect(m_tcpFecBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onTransportModeChanged);
    connect(m_udpFecBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onTransportModeChanged);
    connect(m_detailLogCheck, &QCheckBox::toggled,
            this, &MainWindow::onDetailLogToggled);
    if (m_languageBox) {
        connect(m_languageBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this](int index) {
            AppLanguage language = static_cast<AppLanguage>(
                m_languageBox->itemData(index).toInt());
            UiStrings::setLanguage(language);
            AppSettings::setLanguage(language);
            applyLanguage();
        });
    }
    if (m_settingsServerEdit) {
        connect(m_settingsServerEdit, &QLineEdit::editingFinished, this, [this]() {
            AppSettings::setDefaultServerHost(m_settingsServerEdit->text());
            if (m_serverEdit && !m_roomMgr->signalClient()->isConnected() &&
                !m_roomMgr->isConnecting()) {
                m_serverEdit->setText(settingsEndpointText());
            }
        });
    }
    if (m_settingsPortBox) {
        connect(m_settingsPortBox, QOverload<int>::of(&QSpinBox::valueChanged),
                this, [this](int value) {
            AppSettings::setDefaultServerPort(static_cast<quint16>(value));
            if (m_serverEdit && !m_roomMgr->signalClient()->isConnected() &&
                !m_roomMgr->isConnecting()) {
                m_serverEdit->setText(settingsEndpointText());
            }
        });
    }
    if (m_settingsNameEdit) {
        connect(m_settingsNameEdit, &QLineEdit::editingFinished, this, [this]() {
            AppSettings::setDefaultPlayerName(m_settingsNameEdit->text());
            if (m_nameEdit && !m_roomMgr->signalClient()->isConnected() &&
                !m_roomMgr->isConnecting()) {
                m_nameEdit->setText(m_settingsNameEdit->text().trimmed());
            }
        });
    }
    if (m_settingsVerboseCheck) {
        connect(m_settingsVerboseCheck, &QCheckBox::toggled, this, [this](bool checked) {
            AppSettings::setVerboseLogsDefault(checked);
            if (m_detailLogCheck && m_detailLogCheck->isChecked() != checked)
                m_detailLogCheck->setChecked(checked);
        });
    }
    connect(&LogManager::instance(), &LogManager::logMessage,
            this, &MainWindow::onLogMessage, Qt::QueuedConnection);

    connect(m_roomMgr, &RoomManager::peerConnected, m_roomWidget, &RoomWidget::addPeer);
    connect(m_roomMgr, &RoomManager::peerDisconnected,
            m_roomWidget, &RoomWidget::removePeer);
    connect(m_roomMgr, &RoomManager::peerConnected,
            this, [this](uint32_t, uint32_t, QString) { m_roomMgr->refreshRoomList(); });
    connect(m_roomMgr, &RoomManager::peerDisconnected,
            this, [this](uint32_t) { m_roomMgr->refreshRoomList(); });
    connect(m_roomMgr, &RoomManager::peerTransportChanged,
            m_roomWidget, &RoomWidget::updatePeerTransport);
    connect(m_roomMgr, &RoomManager::peerLatencyUpdated,
            m_roomWidget, &RoomWidget::updatePeerLatency);
    connect(m_roomMgr, &RoomManager::serverRttUpdated,
            this, &MainWindow::onServerRttUpdated);
    connect(m_roomMgr, &RoomManager::serverPasswordRequired,
            this, &MainWindow::promptServerPassword);
}

QWidget* MainWindow::createLoginPage()
{
    QScrollArea* scroll = new QScrollArea();
    scroll->setObjectName("PageScrollArea");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    QWidget* content = new QWidget();
    content->setObjectName("PageScrollContent");
    QVBoxLayout* layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(16);

    QGridLayout* metrics = new QGridLayout();
    metrics->setContentsMargins(0, 0, 0, 0);
    metrics->setSpacing(14);
    metrics->addWidget(createMetricTile("metric.server", &m_dashConnectionLabel,
                                        UiStrings::text("metric.disconnected")), 0, 0);
    metrics->addWidget(createMetricTile("metric.serverRtt", &m_dashServerRttLabel,
                                        QStringLiteral("-")), 0, 1);
    metrics->addWidget(createMetricTile("metric.room", &m_dashRoomLabel,
                                        UiStrings::text("metric.notJoined")), 0, 2);
    metrics->addWidget(createMetricTile("metric.peer", &m_dashPeerLabel,
                                        QStringLiteral("-")), 0, 3);
    layout->addLayout(metrics);

    QVBoxLayout* connLayout = nullptr;
    QWidget* connCard = createCard("login.card", &connLayout);

    QFormLayout* form = new QFormLayout();
    form->setContentsMargins(0, 0, 0, 0);
    form->setHorizontalSpacing(12);
    form->setVerticalSpacing(12);
    form->setLabelAlignment(Qt::AlignRight);

    m_serverEdit = new QLineEdit();
    bindPlaceholder(m_serverEdit, "login.serverPlaceholder");
    m_serverEdit->setMinimumWidth(220);
    QLabel* serverLabel = new QLabel();
    bindText(serverLabel, "login.server");
    form->addRow(serverLabel, m_serverEdit);

    m_nameEdit = new QLineEdit();
    bindPlaceholder(m_nameEdit, "login.namePlaceholder");
    QLabel* nameLabel = new QLabel();
    bindText(nameLabel, "login.name");
    form->addRow(nameLabel, m_nameEdit);

    QWidget* statusRow = new QWidget();
    QHBoxLayout* statusLayout = new QHBoxLayout(statusRow);
    statusLayout->setContentsMargins(0, 0, 0, 0);
    statusLayout->setSpacing(10);
    m_connStatusLabel = new QLabel(UiStrings::text("metric.disconnected"));
    m_connStatusLabel->setObjectName("ConnStatusDisconnected");
    m_connStatusLabel->setAlignment(Qt::AlignCenter);
    m_connStatusLabel->setMinimumWidth(96);
    statusLayout->addWidget(m_connStatusLabel);
    statusLayout->addStretch();
    QLabel* statusLabel = new QLabel();
    bindText(statusLabel, "login.status");
    form->addRow(statusLabel, statusRow);

    connLayout->addLayout(form);

    QHBoxLayout* connActions = new QHBoxLayout();
    connActions->setContentsMargins(0, 2, 0, 0);
    connActions->addStretch();
    m_connectBtn = new QPushButton();
    m_connectBtn->setObjectName("ConnectBtn");
    m_connectBtn->setMinimumWidth(112);
    bindText(m_connectBtn, "login.connect");
    connActions->addWidget(m_connectBtn);
    connLayout->addLayout(connActions);
    layout->addWidget(connCard);

    QLabel* hint = new QLabel();
    hint->setObjectName("MutedText");
    bindText(hint, "login.hint");
    hint->setWordWrap(true);
    layout->addWidget(hint);
    layout->addStretch();

    scroll->setWidget(content);
    return scroll;
}

QWidget* MainWindow::createLobbyPage()
{
    QWidget* page = new QWidget();
    page->setObjectName("LobbyPage");
    QVBoxLayout* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(16);

    QVBoxLayout* listLayout = nullptr;
    QWidget* listCard = createCard("lobby.card", &listLayout);

    QHBoxLayout* toolbar = new QHBoxLayout();
    toolbar->setContentsMargins(0, 0, 0, 0);
    toolbar->setSpacing(10);
    QLabel* hint = new QLabel();
    hint->setObjectName("MutedText");
    bindText(hint, "lobby.hint");
    toolbar->addWidget(hint, 1);
    m_refreshBtn = new QPushButton();
    m_refreshBtn->setObjectName("SecondaryBtn");
    bindText(m_refreshBtn, "lobby.refresh");
    m_joinBtn = new QPushButton();
    m_joinBtn->setObjectName("PrimaryBtn");
    bindText(m_joinBtn, "lobby.join");
    m_leaveBtn = new QPushButton();
    m_leaveBtn->setObjectName("LeaveBtn");
    bindText(m_leaveBtn, "lobby.leave");
    m_leaveBtn->setEnabled(false);
    toolbar->addWidget(m_refreshBtn);
    toolbar->addWidget(m_joinBtn);
    toolbar->addWidget(m_leaveBtn);
    listLayout->addLayout(toolbar);

    m_roomTable = new QTableWidget(0, 5);
    m_roomTable->setHorizontalHeaderLabels(QStringList()
        << UiStrings::text("lobby.col.id")
        << UiStrings::text("lobby.col.name")
        << UiStrings::text("lobby.col.players")
        << UiStrings::text("lobby.col.transport")
        << UiStrings::text("lobby.col.mtu"));
    m_roomTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
    m_roomTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_roomTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Fixed);
    m_roomTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_roomTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Fixed);
    m_roomTable->setColumnWidth(0, 72);
    m_roomTable->setColumnWidth(2, 86);
    m_roomTable->setColumnWidth(4, 72);
    m_roomTable->horizontalHeaderItem(0)->setTextAlignment(Qt::AlignCenter);
    m_roomTable->horizontalHeaderItem(1)->setTextAlignment(Qt::AlignCenter);
    m_roomTable->horizontalHeaderItem(2)->setTextAlignment(Qt::AlignCenter);
    m_roomTable->horizontalHeaderItem(3)->setTextAlignment(Qt::AlignCenter);
    m_roomTable->horizontalHeaderItem(4)->setTextAlignment(Qt::AlignCenter);
    m_roomTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_roomTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_roomTable->setShowGrid(false);
    m_roomTable->verticalHeader()->setVisible(false);
    m_roomTable->verticalHeader()->setDefaultSectionSize(36);
    m_roomTable->setAlternatingRowColors(true);
    listLayout->addWidget(m_roomTable, 1);

    layout->addWidget(listCard, 1);
    return page;
}

QWidget* MainWindow::createCreateRoomPage()
{
    QScrollArea* scroll = new QScrollArea();
    scroll->setObjectName("PageScrollArea");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    QWidget* content = new QWidget();
    content->setObjectName("PageScrollContent");
    QVBoxLayout* layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(16);

    QVBoxLayout* formLayoutOuter = nullptr;
    QWidget* createCardWidget = createCard("create.card", &formLayoutOuter);

    QFormLayout* form = new QFormLayout();
    form->setContentsMargins(0, 0, 0, 0);
    form->setHorizontalSpacing(14);
    form->setVerticalSpacing(12);
    form->setLabelAlignment(Qt::AlignRight);

    m_roomNameEdit = new QLineEdit();
    bindPlaceholder(m_roomNameEdit, "create.roomNamePlaceholder");
    QLabel* roomNameLabel = new QLabel();
    bindText(roomNameLabel, "create.roomName");
    form->addRow(roomNameLabel, m_roomNameEdit);

    m_maxPlayersBox = new QSpinBox();
    m_maxPlayersBox->setRange(2, MAX_PLAYERS);
    m_maxPlayersBox->setValue(8);
    QLabel* maxPlayersLabel = new QLabel();
    bindText(maxPlayersLabel, "create.maxPlayers");
    form->addRow(maxPlayersLabel, m_maxPlayersBox);

    m_passwordEdit = new QLineEdit();
    m_passwordEdit->setEchoMode(QLineEdit::Password);
    bindPlaceholder(m_passwordEdit, "create.roomPasswordPlaceholder");
    QLabel* roomPasswordLabel = new QLabel();
    bindText(roomPasswordLabel, "create.roomPassword");
    form->addRow(roomPasswordLabel, m_passwordEdit);

    m_advancedCheck = new QCheckBox();
    bindText(m_advancedCheck, "create.advanced");
    form->addRow(QString(), m_advancedCheck);

    formLayoutOuter->addLayout(form);

    m_advancedOptionsWidget = new QWidget();
    QFormLayout* advancedForm = new QFormLayout(m_advancedOptionsWidget);
    advancedForm->setContentsMargins(0, 0, 0, 0);
    advancedForm->setHorizontalSpacing(14);
    advancedForm->setVerticalSpacing(12);
    advancedForm->setLabelAlignment(Qt::AlignRight);

    auto fillModeBox = [](QComboBox* box) {
        box->addItem(QString::fromUtf8("Raw UDP"), MODE_RELAY_RAW_UDP);
        box->addItem(QString::fromUtf8("KCP"), MODE_RELAY_KCP);
        box->addItem(QString::fromUtf8("TCP Relay"), MODE_RELAY_TCP);
    };
    auto fillFecBox = [](QComboBox* box) {
        box->addItem(UiStrings::text("value.none"), FEC_NONE);
        box->addItem(QString::fromUtf8("FEC 10%"), FEC_10);
        box->addItem(QString::fromUtf8("FEC 30%"), FEC_30);
        box->addItem(QString::fromUtf8("FEC 50%"), FEC_50);
        box->addItem(QString::fromUtf8("FEC 70%"), FEC_70);
        box->addItem(QString::fromUtf8("FEC 100%"), FEC_100);
        box->addItem(QString::fromUtf8("FEC 200%"), FEC_200);
    };
    auto fillProfileBox = [](QComboBox* box) {
        box->addItem(UiStrings::text("value.realtime"), KCP_PROFILE_REALTIME);
        box->addItem(UiStrings::text("value.bulk"), KCP_PROFILE_BULK);
    };

    m_tcpModeBox = new QComboBox();
    fillModeBox(m_tcpModeBox);
    m_tcpModeBox->setCurrentIndex(0);
    QLabel* tcpProtocolLabel = new QLabel();
    bindText(tcpProtocolLabel, "create.tcpProtocol");
    advancedForm->addRow(tcpProtocolLabel, m_tcpModeBox);
    m_tcpFecBox = new QComboBox();
    fillFecBox(m_tcpFecBox);
    QLabel* tcpFecLabel = new QLabel();
    bindText(tcpFecLabel, "create.tcpFec");
    advancedForm->addRow(tcpFecLabel, m_tcpFecBox);
    m_tcpKcpProfileBox = new QComboBox();
    fillProfileBox(m_tcpKcpProfileBox);
    QLabel* tcpProfileLabel = new QLabel();
    bindText(tcpProfileLabel, "create.tcpProfile");
    advancedForm->addRow(tcpProfileLabel, m_tcpKcpProfileBox);

    m_udpModeBox = new QComboBox();
    fillModeBox(m_udpModeBox);
    m_udpModeBox->setCurrentIndex(1);
    QLabel* udpProtocolLabel = new QLabel();
    bindText(udpProtocolLabel, "create.udpProtocol");
    advancedForm->addRow(udpProtocolLabel, m_udpModeBox);
    m_udpFecBox = new QComboBox();
    fillFecBox(m_udpFecBox);
    QLabel* udpFecLabel = new QLabel();
    bindText(udpFecLabel, "create.udpFec");
    advancedForm->addRow(udpFecLabel, m_udpFecBox);
    m_udpKcpProfileBox = new QComboBox();
    fillProfileBox(m_udpKcpProfileBox);
    QLabel* udpProfileLabel = new QLabel();
    bindText(udpProfileLabel, "create.udpProfile");
    advancedForm->addRow(udpProfileLabel, m_udpKcpProfileBox);

    m_mtuModeBox = new QComboBox();
    m_mtuModeBox->addItem(UiStrings::text("value.balancedMtu"), ROOM_MTU_BALANCED);
    m_mtuModeBox->addItem(UiStrings::text("value.aggressiveMtu"), ROOM_MTU_AGGRESSIVE);
    m_mtuModeBox->addItem(UiStrings::text("value.safeMtu"), ROOM_MTU_SAFE);
    QLabel* mtuLabel = new QLabel();
    bindText(mtuLabel, "create.mtu");
    advancedForm->addRow(mtuLabel, m_mtuModeBox);
    m_advancedOptionsWidget->setVisible(false);
    formLayoutOuter->addWidget(m_advancedOptionsWidget);

    QHBoxLayout* btnRow = new QHBoxLayout();
    btnRow->addStretch();
    m_createBtn = new QPushButton();
    m_createBtn->setObjectName("PrimaryBtn");
    m_createBtn->setMinimumWidth(118);
    bindText(m_createBtn, "create.create");
    btnRow->addWidget(m_createBtn);
    formLayoutOuter->addLayout(btnRow);

    layout->addWidget(createCardWidget);
    QLabel* note = new QLabel();
    note->setObjectName("MutedText");
    bindText(note, "create.note");
    note->setWordWrap(true);
    layout->addWidget(note);
    layout->addStretch();

    scroll->setWidget(content);
    return scroll;
}

QWidget* MainWindow::createMembersPage()
{
    QWidget* page = new QWidget();
    page->setObjectName("MembersPage");
    QVBoxLayout* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(14);

    m_roomWidget = new RoomWidget();
    layout->addWidget(m_roomWidget, 1);

    return page;
}

QWidget* MainWindow::createLogPage()
{
    QWidget* page = new QWidget();
    page->setObjectName("LogPage");
    QVBoxLayout* pageLayout = new QVBoxLayout(page);
    pageLayout->setContentsMargins(0, 0, 0, 0);
    pageLayout->setSpacing(14);

    QWidget* logCard = new QWidget();
    logCard->setObjectName("Card");
    QVBoxLayout* logLayout = new QVBoxLayout(logCard);
    logLayout->setContentsMargins(18, 16, 18, 18);
    logLayout->setSpacing(12);

    QHBoxLayout* header = new QHBoxLayout();
    header->setContentsMargins(0, 0, 0, 0);
    QLabel* title = new QLabel();
    title->setObjectName("CardTitle");
    bindText(title, "logs.title");
    header->addWidget(title);
    header->addStretch();
    m_detailLogCheck = new QCheckBox();
    m_detailLogCheck->setObjectName("DetailLogCheck");
    bindText(m_detailLogCheck, "logs.verbose");
    m_detailLogCheck->setChecked(m_showDetailLog);
    header->addWidget(m_detailLogCheck);
    logLayout->addLayout(header);

    m_logEdit = new QTextEdit();
    m_logEdit->setObjectName("LogConsole");
    m_logEdit->setReadOnly(true);
    m_logEdit->setMinimumHeight(360);
    logLayout->addWidget(m_logEdit, 1);

    pageLayout->addWidget(logCard, 1);
    return page;
}

QWidget* MainWindow::createSettingsPage()
{
    QScrollArea* scroll = new QScrollArea();
    scroll->setObjectName("PageScrollArea");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    QWidget* content = new QWidget();
    content->setObjectName("PageScrollContent");
    QVBoxLayout* layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(16);

    QVBoxLayout* languageLayout = nullptr;
    QWidget* languageCard = createCard("settings.card.language", &languageLayout);
    QFormLayout* languageForm = new QFormLayout();
    languageForm->setContentsMargins(0, 0, 0, 0);
    languageForm->setHorizontalSpacing(14);
    languageForm->setVerticalSpacing(12);
    languageForm->setLabelAlignment(Qt::AlignRight);

    m_languageBox = new QComboBox();
    m_languageBox->addItem(UiStrings::text("settings.language.english"),
                           static_cast<int>(AppLanguage::English));
    m_languageBox->addItem(UiStrings::text("settings.language.chinese"),
                           static_cast<int>(AppLanguage::Chinese));
    QLabel* languageLabel = new QLabel();
    bindText(languageLabel, "settings.language");
    languageForm->addRow(languageLabel, m_languageBox);
    languageLayout->addLayout(languageForm);

    QVBoxLayout* defaultsLayout = nullptr;
    QWidget* defaultsCard = createCard("settings.card.defaults", &defaultsLayout);
    QFormLayout* defaultsForm = new QFormLayout();
    defaultsForm->setContentsMargins(0, 0, 0, 0);
    defaultsForm->setHorizontalSpacing(14);
    defaultsForm->setVerticalSpacing(12);
    defaultsForm->setLabelAlignment(Qt::AlignRight);

    m_settingsServerEdit = new QLineEdit();
    bindPlaceholder(m_settingsServerEdit, "settings.serverPlaceholder");
    QLabel* defaultServerLabel = new QLabel();
    bindText(defaultServerLabel, "settings.server");
    defaultsForm->addRow(defaultServerLabel, m_settingsServerEdit);

    m_settingsPortBox = new QSpinBox();
    m_settingsPortBox->setRange(1, 65535);
    m_settingsPortBox->setValue(DEFAULT_PORT);
    QLabel* defaultPortLabel = new QLabel();
    bindText(defaultPortLabel, "settings.port");
    defaultsForm->addRow(defaultPortLabel, m_settingsPortBox);

    m_settingsNameEdit = new QLineEdit();
    bindPlaceholder(m_settingsNameEdit, "settings.namePlaceholder");
    QLabel* defaultNameLabel = new QLabel();
    bindText(defaultNameLabel, "settings.name");
    defaultsForm->addRow(defaultNameLabel, m_settingsNameEdit);

    m_settingsVerboseCheck = new QCheckBox();
    bindText(m_settingsVerboseCheck, "settings.verbose");
    defaultsForm->addRow(QString(), m_settingsVerboseCheck);

    defaultsLayout->addLayout(defaultsForm);

    QLabel* note = new QLabel();
    note->setObjectName("MutedText");
    note->setWordWrap(true);
    bindText(note, "settings.note");
    defaultsLayout->addWidget(note);

    layout->addWidget(languageCard);
    layout->addWidget(defaultsCard);
    layout->addStretch();

    scroll->setWidget(content);
    return scroll;
}

void MainWindow::changePage(int index)
{
    if (!m_mainStack || index < 0 || index >= m_mainStack->count()) {
        return;
    }

    m_mainStack->setCurrentIndex(index);

    QStringList titles = QStringList()
        << UiStrings::text("page.login.title")
        << UiStrings::text("page.lobby.title")
        << UiStrings::text("page.create.title")
        << UiStrings::text("page.members.title")
        << UiStrings::text("page.logs.title")
        << UiStrings::text("page.settings.title");
    QStringList subtitles = QStringList()
        << UiStrings::text("page.login.subtitle")
        << UiStrings::text("page.lobby.subtitle")
        << UiStrings::text("page.create.subtitle")
        << UiStrings::text("page.members.subtitle")
        << UiStrings::text("page.logs.subtitle")
        << UiStrings::text("page.settings.subtitle");

    if (index < titles.size()) {
        m_pageTitleLabel->setText(titles[index]);
        m_pageSubtitleLabel->setText(subtitles[index]);
    }
}

void MainWindow::initTray()
{
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        return;
    }

    QApplication::setQuitOnLastWindowClosed(false);

    m_trayIcon = new QSystemTrayIcon(this);
    m_trayMenu = new ModernTrayMenu(this);
    m_trayMenu->addHeader(QStringLiteral("VLan"), UiStrings::text("metric.disconnected"));
    m_trayMenu->addSeparator();
    m_trayMenu->addAction(QStringLiteral("show"), UiStrings::text("tray.show"));
    m_trayMenu->addAction(QStringLiteral("toggle_connection"), UiStrings::text("tray.connect"));
    m_trayMenu->addSeparator();
    m_trayMenu->addAction(QStringLiteral("refresh_rooms"), UiStrings::text("tray.refresh"));
    m_trayMenu->addAction(QStringLiteral("leave_room"), UiStrings::text("tray.leave"));
    m_trayMenu->addSeparator();
    m_trayMenu->addAction(QStringLiteral("quit"), UiStrings::text("tray.quit"));

    connect(m_trayIcon, &QSystemTrayIcon::activated,
            this, &MainWindow::onTrayIconActivated);
    connect(m_trayMenu, &ModernTrayMenu::itemClicked,
            this, &MainWindow::onTrayMenuItemClicked);

    m_trayIcon->show();
}

void MainWindow::updateTrayState()
{
    if (!m_trayIcon) {
        return;
    }

    bool connected = m_roomMgr && m_roomMgr->signalClient()->isConnected();
    bool connecting = m_roomMgr && m_roomMgr->isConnecting();
    bool inRoom = m_roomMgr && m_roomMgr->inRoom();

    QString stateText;
    if (connecting) {
        stateText = UiStrings::text("metric.connecting");
    } else if (connected && inRoom) {
        stateText = UiStrings::text("tray.roomConnected").arg(m_roomMgr->currentRoomId());
    } else if (connected) {
        stateText = UiStrings::text("metric.connected");
    } else {
        stateText = UiStrings::text("metric.disconnected");
    }

    m_trayIcon->setIcon(createTrayIcon(connected, connecting));
    m_trayIcon->setToolTip(QString::fromUtf8("VLan - %1").arg(stateText));

    if (m_trayMenu) {
        m_trayMenu->setHeaderSubtitle(stateText);
        m_trayMenu->setItemText(QStringLiteral("show"),
                                UiStrings::text("tray.show"));
        m_trayMenu->setItemText(QStringLiteral("toggle_connection"),
            (connected || connecting) ? UiStrings::text("tray.disconnect")
                                      : UiStrings::text("tray.connect"));
        m_trayMenu->setItemText(QStringLiteral("refresh_rooms"),
                                UiStrings::text("tray.refresh"));
        m_trayMenu->setItemText(QStringLiteral("leave_room"),
                                UiStrings::text("tray.leave"));
        m_trayMenu->setItemText(QStringLiteral("quit"),
                                UiStrings::text("tray.quit"));
        m_trayMenu->setItemEnabled(QStringLiteral("refresh_rooms"), connected);
        m_trayMenu->setItemEnabled(QStringLiteral("leave_room"), inRoom);
    }
}

void MainWindow::refreshDashboardState()
{
    if (!m_roomMgr) {
        return;
    }

    bool connected = m_roomMgr->signalClient()->isConnected();
    bool connecting = m_roomMgr->isConnecting();
    bool inRoom = m_roomMgr->inRoom();

    if (m_dashConnectionLabel) {
        if (connecting) {
            m_dashConnectionLabel->setText(UiStrings::text("metric.connecting"));
            m_dashConnectionLabel->setObjectName("MetricValuePending");
        } else if (connected) {
            m_dashConnectionLabel->setText(UiStrings::text("metric.connected"));
            m_dashConnectionLabel->setObjectName("MetricValueGood");
        } else {
            m_dashConnectionLabel->setText(UiStrings::text("metric.disconnected"));
            m_dashConnectionLabel->setObjectName("MetricValueBad");
        }
        repolish(m_dashConnectionLabel);
    }

    if (m_dashServerRttLabel) {
        m_dashServerRttLabel->setText(connected && m_serverRttMs >= 0
            ? QStringLiteral("%1ms").arg(m_serverRttMs)
            : QStringLiteral("-"));
        m_dashServerRttLabel->setObjectName(connected && m_serverRttMs >= 0
            ? "MetricValueGood" : "MetricValueMuted");
        repolish(m_dashServerRttLabel);
    }

    if (m_dashRoomLabel) {
        m_dashRoomLabel->setText(inRoom
            ? UiStrings::text("metric.roomValue").arg(m_roomMgr->currentRoomId())
            : UiStrings::text("metric.notJoined"));
        m_dashRoomLabel->setObjectName(inRoom ? "MetricValueGood" : "MetricValue");
        repolish(m_dashRoomLabel);
    }

    if (m_dashPeerLabel) {
        uint32_t peerId = m_roomMgr->myPeerId();
        m_dashPeerLabel->setText(connected && peerId != 0
            ? UiStrings::text("metric.peerValue").arg(peerId)
            : QStringLiteral("-"));
        m_dashPeerLabel->setObjectName(connected ? "MetricValue" : "MetricValueMuted");
        repolish(m_dashPeerLabel);
    }
}

void MainWindow::onTrayIconActivated(QSystemTrayIcon::ActivationReason reason)
{
    if (reason == QSystemTrayIcon::Context) {
        updateTrayState();
        if (m_trayMenu) {
            m_trayMenu->showAtCursor();
        }
    } else if (reason == QSystemTrayIcon::Trigger ||
               reason == QSystemTrayIcon::DoubleClick) {
        showFromTray();
    }
}

void MainWindow::onTrayMenuItemClicked(const QString& id)
{
    if (id == QStringLiteral("show")) {
        showFromTray();
    } else if (id == QStringLiteral("toggle_connection")) {
        onConnectClicked();
    } else if (id == QStringLiteral("refresh_rooms")) {
        onRefreshClicked();
    } else if (id == QStringLiteral("leave_room")) {
        if (m_roomMgr->inRoom()) {
            onLeaveRoomClicked();
        }
    } else if (id == QStringLiteral("quit")) {
        quitProgram();
    }
    updateTrayState();
}

void MainWindow::showFromTray()
{
    if (isMinimized()) {
        showNormal();
    } else {
        show();
    }
    raise();
    activateWindow();
}

void MainWindow::quitProgram()
{
    if (m_quitRequested) {
        return;
    }
    m_quitRequested = true;
    if (m_trayMenu) {
        m_trayMenu->hide();
    }
    if (m_trayIcon) {
        m_trayIcon->hide();
    }
    if (m_roomMgr->signalClient()->isConnected() ||
        m_roomMgr->isConnecting()) {
        m_roomMgr->disconnectFromServer();
        QTimer::singleShot(1200, this, [this]() {
            if (m_quitRequested)
                finishProgramQuit();
        });
        return;
    }
    finishProgramQuit();
}

void MainWindow::finishProgramQuit()
{
    QApplication::quit();
}

// ==================== Slots ====================

void MainWindow::onConnectClicked() {
    if (m_roomMgr->signalClient()->isConnected() ||
        m_roomMgr->isConnecting()) {
        m_roomMgr->disconnectFromServer();
        onConnectionStatusChanged(false);
        updateTrayState();
        return;
    }

    QString host;
    quint16 port;

    LogManager::instance().clearMaskedKeywords();
    QString addr = m_serverEdit->text().trimmed();
    if (addr.isEmpty()) {
        QMessageBox::warning(this, UiStrings::text("dialog.notice"),
                             UiStrings::text("error.enterServer"));
        return;
    }
    host = addr.section(':', 0, 0);
    port = addr.section(':', 1, 1).toUShort();
    if (port == 0) port = DEFAULT_PORT;

    QString name = m_nameEdit->text().trimmed();
    if (name.isEmpty()) {
        QMessageBox::warning(this, UiStrings::text("dialog.notice"),
                             UiStrings::text("error.enterName"));
        return;
    }
    QByteArray nameBytes = name.toUtf8();
    if (!isValidPlayerName(std::string(nameBytes.constData(), nameBytes.size()))) {
        QMessageBox::warning(this, UiStrings::text("dialog.notice"),
                             UiStrings::text("error.enterName"));
        return;
    }

    updateConnectButton(false);
    m_connectBtn->setText(UiStrings::text("login.cancel"));
    m_connectBtn->setEnabled(true);
    m_serverEdit->setEnabled(false);
    m_nameEdit->setEnabled(false);
    m_connStatusLabel->setText(UiStrings::text("metric.connecting"));
    m_connStatusLabel->setObjectName("ConnStatusDisconnected");
    repolish(m_connStatusLabel);
    m_serverRttMs = -1;
    saveConnectionDefaults(host, port, name);

    m_roomMgr->setServerAddress(host, port);
    m_roomMgr->connectAndLogin(name);
    refreshDashboardState();
    updateTrayState();
}

void MainWindow::onTransportModeChanged(int index) {
    Q_UNUSED(index);
    updatePolicyControlState();
}

void MainWindow::onCreateRoomClicked() {
    if (m_roomMgr->inRoom())
        return;

    QString name = m_roomNameEdit->text().trimmed();
    QByteArray roomNameBytes = name.toUtf8();
    if (!isValidRoomName(std::string(roomNameBytes.constData(), roomNameBytes.size()))) {
        QMessageBox::warning(this, UiStrings::text("dialog.notice"),
                             UiStrings::text("error.invalidRoomName"));
        return;
    }
    int maxP = m_maxPlayersBox->value();
    if (maxP < 2 || maxP > MAX_PLAYERS) {
        QMessageBox::warning(this, UiStrings::text("dialog.notice"),
                             UiStrings::text("error.invalidRoomName"));
        return;
    }

    QString password = m_passwordEdit->text();
    bool passwordProtected = !password.isEmpty();
    if (passwordProtected) {
        QByteArray pwdBytes = password.toUtf8();
        if (!isValidRoomPassword(std::string(pwdBytes.constData(), pwdBytes.size()))) {
            QMessageBox::warning(this, UiStrings::text("dialog.notice"),
                                 UiStrings::text("error.invalidRoomPassword"));
            return;
        }
    }

    bool advanced = m_advancedCheck->isChecked();
    RoomTrafficPolicy tcpPolicy = advanced ? policyFromControls(true) : makeDefaultTcpPolicy();
    RoomTrafficPolicy udpPolicy = advanced ? policyFromControls(false) : makeDefaultUdpPolicy();
    uint16_t mtu = advanced
        ? normalizeRoomMtu(m_mtuModeBox->currentData().toInt())
        : static_cast<uint16_t>(ROOM_MTU_DEFAULT);
    m_roomMgr->createRoom(name, static_cast<uint8_t>(maxP),
                          tcpPolicy, udpPolicy, mtu,
                          passwordProtected, password);
}

void MainWindow::onJoinRoomClicked() {
    if (m_roomMgr->inRoom() || !m_roomMgr->signalClient()->isConnected())
        return;

    int row = m_roomTable->currentRow();
    if (row < 0) {
        QMessageBox::warning(this, UiStrings::text("dialog.notice"),
                             UiStrings::text("error.selectRoom"));
        return;
    }

    QTableWidgetItem* countItem = m_roomTable->item(row, 2);
    if (countItem) {
        QString countText = countItem->text();
        QStringList parts = countText.split('/');
        if (parts.size() == 2) {
            int current = parts[0].toInt();
            int max     = parts[1].toInt();
            if (current >= max) {
                QMessageBox::warning(this, UiStrings::text("dialog.notice"),
                                     UiStrings::text("error.selectRoom"));
                return;
            }
        }
    }

    uint32_t roomId = m_roomTable->item(row, 0)->text().toUInt();

    bool passwordProtected = false;
    QTableWidgetItem* modeItem = m_roomTable->item(row, 3);
    if (modeItem && modeItem->data(Qt::UserRole).toBool())
        passwordProtected = true;

    QString password;
    if (passwordProtected) {
        bool ok = false;
        password = QInputDialog::getText(this,
            UiStrings::text("dialog.roomPassword.title"),
            UiStrings::text("dialog.roomPassword.prompt"),
            QLineEdit::Password, QString(), &ok);
        if (!ok || password.isEmpty()) return;
        QByteArray pwdBytes = password.toUtf8();
        if (!isValidRoomPassword(std::string(pwdBytes.constData(), pwdBytes.size()))) {
            QMessageBox::warning(this, UiStrings::text("dialog.notice"),
                                 UiStrings::text("error.invalidRoomPassword"));
            return;
        }
    }

    m_roomMgr->joinRoom(roomId, password);
}

void MainWindow::onLeaveRoomClicked() {
    m_roomMgr->leaveRoom();
}

void MainWindow::onRefreshClicked() {
    m_roomMgr->refreshRoomList();
}

void MainWindow::onConnectionStatusChanged(bool connected) {
    m_connStatusLabel->setText(connected
        ? UiStrings::text("metric.connected")
        : UiStrings::text("metric.disconnected"));
    m_connStatusLabel->setObjectName(connected
        ? "ConnStatusConnected" : "ConnStatusDisconnected");
    repolish(m_connStatusLabel);

    updateConnectButton(connected);
    if (!connected)
        m_serverRttMs = -1;
    m_connectBtn->setEnabled(true);
    m_serverEdit->setEnabled(!connected);
    m_nameEdit->setEnabled(!connected);
    setRoomControlsEnabled(m_roomMgr->inRoom());
    refreshDashboardState();
    updateTrayState();
}

void MainWindow::onConnectFailed(QString reason) {
    m_connStatusLabel->setText(UiStrings::text("metric.connectFailed"));
    m_connStatusLabel->setObjectName("ConnStatusDisconnected");
    repolish(m_connStatusLabel);
    updateConnectButton(false);
    m_connectBtn->setEnabled(true);
    m_serverEdit->setEnabled(true);
    m_nameEdit->setEnabled(true);
    onStatusMessage(UiStrings::text("error.connectFailed").arg(reason));
    setRoomControlsEnabled(false);
    refreshDashboardState();
    updateTrayState();
}

void MainWindow::onLoggedIn(uint32_t peerId) {
    m_connStatusLabel->setText(UiStrings::text("metric.loggedIn").arg(peerId));
    m_roomMgr->refreshRoomList();
    refreshDashboardState();
    updateTrayState();
}

void MainWindow::onRoomCreated(uint32_t roomId) {
    Q_UNUSED(roomId);
    setRoomControlsEnabled(true);
    m_roomWidget->clear();
    m_roomWidget->setPolicies(m_roomMgr->tcpPolicy(), m_roomMgr->udpPolicy());
    m_roomWidget->setRoomContext(m_roomMgr->currentRoomId(), m_roomMgr->roomMtu());
    m_roomWidget->setMyInfo(m_roomMgr->myPeerId(), m_roomMgr->myVirtualIP());
    m_roomMgr->refreshRoomList();
    if (m_navBar) m_navBar->setCurrentRow(PageMembers);
    refreshDashboardState();
    updateTrayState();
}

void MainWindow::onRoomJoined(uint32_t roomId) {
    Q_UNUSED(roomId);
    setRoomControlsEnabled(true);
    m_roomWidget->clear();
    m_roomWidget->setPolicies(m_roomMgr->tcpPolicy(), m_roomMgr->udpPolicy());
    m_roomWidget->setRoomContext(m_roomMgr->currentRoomId(), m_roomMgr->roomMtu());
    m_roomWidget->setMyInfo(m_roomMgr->myPeerId(), m_roomMgr->myVirtualIP());
    m_roomMgr->refreshRoomList();
    if (m_navBar) m_navBar->setCurrentRow(PageMembers);
    refreshDashboardState();
    updateTrayState();
}

void MainWindow::onRoomLeft() {
    setRoomControlsEnabled(false);
    m_roomWidget->clear();
    m_roomMgr->refreshRoomList();
    refreshDashboardState();
    updateTrayState();
}

void MainWindow::onRoomListUpdated(QList<RoomListItem> rooms) {
    m_roomTable->setRowCount(rooms.size());
    for (int i = 0; i < rooms.size(); ++i) {
        const RoomListItem& r = rooms[i];
        bool passwordProtected = (r.passwordProtected != 0);

        auto* idItem = new QTableWidgetItem(QString::number(r.roomId));
        idItem->setTextAlignment(Qt::AlignCenter);
        m_roomTable->setItem(i, 0, idItem);

        QString displayName = QString::fromUtf8(r.roomName);
        if (passwordProtected) displayName.prepend(UiStrings::text("lobby.passwordPrefix"));
        auto* nameItem = new QTableWidgetItem(displayName);
        nameItem->setTextAlignment(Qt::AlignCenter);
        m_roomTable->setItem(i, 1, nameItem);

        auto* countItem = new QTableWidgetItem(
            QString("%1/%2").arg(r.playerCount).arg(r.maxPlayers));
        countItem->setTextAlignment(Qt::AlignCenter);
        m_roomTable->setItem(i, 2, countItem);

        QString modeName = QString::fromUtf8("TCP:%1 / UDP:%2")
            .arg(policyLabel(r.tcpPolicy))
            .arg(policyLabel(r.udpPolicy));
        if (passwordProtected)
            modeName += UiStrings::text("lobby.passwordSuffix");
        auto* modeItem = new QTableWidgetItem(modeName);
        modeItem->setTextAlignment(Qt::AlignCenter);
        modeItem->setData(Qt::UserRole, passwordProtected);
        m_roomTable->setItem(i, 3, modeItem);

        auto* mtuItem = new QTableWidgetItem(QString::number(normalizeRoomMtu(r.mtu)));
        mtuItem->setTextAlignment(Qt::AlignCenter);
        m_roomTable->setItem(i, 4, mtuItem);

        if (passwordProtected) {
            QColor orange(230, 140, 0);
            for (int col = 0; col < 5; ++col) {
                QTableWidgetItem* item = m_roomTable->item(i, col);
                if (item) item->setForeground(orange);
            }
        }
    }
    refreshDashboardState();
    updateTrayState();
}

void MainWindow::onStatusMessage(QString msg) {
    LogManager::instance().logNormal(msg);
}

void MainWindow::onErrorOccurred(QString msg) {
    LogManager::instance().logError(msg);
}

void MainWindow::onLogMessage(QString formattedHtml, int level) {
    if (level > LOG_LEVEL_NORMAL && !m_showDetailLog)
        return;
    m_logEdit->append(formattedHtml);
    QScrollBar* sb = m_logEdit->verticalScrollBar();
    sb->setValue(sb->maximum());
}

void MainWindow::onDetailLogToggled(bool checked) {
    m_showDetailLog = checked;
    AppSettings::setVerboseLogsDefault(checked);
    if (m_settingsVerboseCheck && m_settingsVerboseCheck->isChecked() != checked) {
        QSignalBlocker blocker(m_settingsVerboseCheck);
        m_settingsVerboseCheck->setChecked(checked);
    }
    m_logEdit->clear();
    const QList<LogEntry>& entries = LogManager::instance().allEntries();
    for (int i = 0; i < entries.size(); ++i) {
        const LogEntry& e = entries[i];
        if (!checked && e.level > LOG_LEVEL_NORMAL)
            continue;
        m_logEdit->append(LogManager::formatHtml(e));
    }
    QScrollBar* sb = m_logEdit->verticalScrollBar();
    sb->setValue(sb->maximum());
}

void MainWindow::onServerRttUpdated(int rttMs) {
    m_serverRttMs = rttMs;
    refreshDashboardState();
    updateTrayState();
}

void MainWindow::onTunSpeedUpdated(quint64 uploadBytesPerSec,
                                   quint64 downloadBytesPerSec) {
    m_lastUploadRate = uploadBytesPerSec;
    m_lastDownloadRate = downloadBytesPerSec;
    if (!m_trafficLabel) return;
    m_trafficLabel->setText(UiStrings::text("traffic.value")
                            .arg(formatSpeed(uploadBytesPerSec))
                            .arg(formatSpeed(downloadBytesPerSec)));
}

void MainWindow::applyLanguage()
{
    setWindowTitle(UiStrings::text("app.windowTitle"));
    retranslateTree(this);
    updateComboTexts();
    if (m_roomWidget)
        m_roomWidget->retranslateUi();
    if (m_navBar) {
        QStringList nav;
        nav << UiStrings::text("nav.login")
            << UiStrings::text("nav.lobby")
            << UiStrings::text("nav.create")
            << UiStrings::text("nav.members")
            << UiStrings::text("nav.logs")
            << UiStrings::text("nav.settings");
        for (int i = 0; i < nav.size() && i < m_navBar->count(); ++i)
            m_navBar->item(i)->setText(nav[i]);
        changePage(m_navBar->currentRow());
    }
    if (m_roomTable) {
        m_roomTable->setHorizontalHeaderLabels(QStringList()
            << UiStrings::text("lobby.col.id")
            << UiStrings::text("lobby.col.name")
            << UiStrings::text("lobby.col.players")
            << UiStrings::text("lobby.col.transport")
            << UiStrings::text("lobby.col.mtu"));
    }
    if (m_trafficLabel) {
        m_trafficLabel->setText(UiStrings::text("traffic.value")
            .arg(formatSpeed(m_lastUploadRate))
            .arg(formatSpeed(m_lastDownloadRate)));
    }
    bool connected = m_roomMgr && m_roomMgr->signalClient()->isConnected();
    bool connecting = m_roomMgr && m_roomMgr->isConnecting();
    updateConnectButton(connected);
    if (connecting && m_connectBtn)
        m_connectBtn->setText(UiStrings::text("login.cancel"));
    if (m_connStatusLabel && m_roomMgr) {
        if (connecting) {
            m_connStatusLabel->setText(UiStrings::text("metric.connecting"));
        } else if (connected && m_roomMgr->myPeerId() != 0) {
            m_connStatusLabel->setText(UiStrings::text("metric.loggedIn")
                                       .arg(m_roomMgr->myPeerId()));
        } else if (connected) {
            m_connStatusLabel->setText(UiStrings::text("metric.connected"));
        } else {
            m_connStatusLabel->setText(UiStrings::text("metric.disconnected"));
        }
    }
    refreshDashboardState();
    updateTrayState();
    if (m_roomMgr && m_roomMgr->signalClient()->isConnected())
        m_roomMgr->refreshRoomList();
}

void MainWindow::loadPersistentSettings()
{
    AppLanguage language = AppSettings::language();
    UiStrings::setLanguage(language);

    QString host = AppSettings::defaultServerHost();
    quint16 port = AppSettings::defaultServerPort();
    QString name = AppSettings::defaultPlayerName();
    bool verbose = AppSettings::verboseLogsDefault();

    if (m_languageBox) {
        QSignalBlocker blocker(m_languageBox);
        int idx = m_languageBox->findData(static_cast<int>(language));
        if (idx >= 0) m_languageBox->setCurrentIndex(idx);
    }
    if (m_settingsServerEdit) {
        QSignalBlocker blocker(m_settingsServerEdit);
        m_settingsServerEdit->setText(host);
    }
    if (m_settingsPortBox) {
        QSignalBlocker blocker(m_settingsPortBox);
        m_settingsPortBox->setValue(port);
    }
    if (m_settingsNameEdit) {
        QSignalBlocker blocker(m_settingsNameEdit);
        m_settingsNameEdit->setText(name);
    }
    if (m_settingsVerboseCheck) {
        QSignalBlocker blocker(m_settingsVerboseCheck);
        m_settingsVerboseCheck->setChecked(verbose);
    }
    if (m_detailLogCheck) {
        QSignalBlocker blocker(m_detailLogCheck);
        m_detailLogCheck->setChecked(verbose);
    }
    m_showDetailLog = verbose;
    g_verboseLog = verbose;

    if (m_serverEdit && !host.isEmpty())
        m_serverEdit->setText(settingsEndpointText());
    if (m_nameEdit && !name.isEmpty())
        m_nameEdit->setText(name);

    applyLanguage();
}

void MainWindow::saveConnectionDefaults(const QString& host, quint16 port,
                                        const QString& playerName)
{
    AppSettings::setDefaultServerHost(host);
    AppSettings::setDefaultServerPort(port);
    AppSettings::setDefaultPlayerName(playerName);

    if (m_settingsServerEdit) {
        QSignalBlocker blocker(m_settingsServerEdit);
        m_settingsServerEdit->setText(host);
    }
    if (m_settingsPortBox) {
        QSignalBlocker blocker(m_settingsPortBox);
        m_settingsPortBox->setValue(port == 0 ? DEFAULT_PORT : port);
    }
    if (m_settingsNameEdit) {
        QSignalBlocker blocker(m_settingsNameEdit);
        m_settingsNameEdit->setText(playerName);
    }
}

QString MainWindow::settingsEndpointText() const
{
    QString host = AppSettings::defaultServerHost();
    quint16 port = AppSettings::defaultServerPort();
    if (host.isEmpty())
        return QString();
    return QStringLiteral("%1:%2").arg(host).arg(port);
}

void MainWindow::updateComboTexts()
{
    auto setItemTextByData = [](QComboBox* box, int data, const QString& text) {
        if (!box) return;
        int idx = box->findData(data);
        if (idx >= 0) box->setItemText(idx, text);
    };

    setItemTextByData(m_tcpFecBox, FEC_NONE, UiStrings::text("value.none"));
    setItemTextByData(m_udpFecBox, FEC_NONE, UiStrings::text("value.none"));
    setItemTextByData(m_tcpKcpProfileBox, KCP_PROFILE_REALTIME, UiStrings::text("value.realtime"));
    setItemTextByData(m_udpKcpProfileBox, KCP_PROFILE_REALTIME, UiStrings::text("value.realtime"));
    setItemTextByData(m_tcpKcpProfileBox, KCP_PROFILE_BULK, UiStrings::text("value.bulk"));
    setItemTextByData(m_udpKcpProfileBox, KCP_PROFILE_BULK, UiStrings::text("value.bulk"));
    setItemTextByData(m_mtuModeBox, ROOM_MTU_BALANCED, UiStrings::text("value.balancedMtu"));
    setItemTextByData(m_mtuModeBox, ROOM_MTU_AGGRESSIVE, UiStrings::text("value.aggressiveMtu"));
    setItemTextByData(m_mtuModeBox, ROOM_MTU_SAFE, UiStrings::text("value.safeMtu"));
    setItemTextByData(m_languageBox, static_cast<int>(AppLanguage::English),
                      UiStrings::text("settings.language.english"));
    setItemTextByData(m_languageBox, static_cast<int>(AppLanguage::Chinese),
                      UiStrings::text("settings.language.chinese"));
}

RoomTrafficPolicy MainWindow::policyFromControls(bool tcpTraffic) const {
    RoomTrafficPolicy fallback = tcpTraffic ? makeDefaultTcpPolicy()
                                            : makeDefaultUdpPolicy();
    if (!m_advancedCheck || !m_advancedCheck->isChecked())
        return fallback;

    QComboBox* modeBox = tcpTraffic ? m_tcpModeBox : m_udpModeBox;
    QComboBox* fecBox = tcpTraffic ? m_tcpFecBox : m_udpFecBox;
    QComboBox* profileBox = tcpTraffic ? m_tcpKcpProfileBox : m_udpKcpProfileBox;

    return normalizeTrafficPolicy(
        modeBox->currentData().toInt(),
        fecBox->currentData().toInt(),
        profileBox->currentData().toInt(),
        fallback);
}

void MainWindow::updatePolicyControlState() {
    if (!m_advancedCheck || !m_advancedOptionsWidget)
        return;

    bool canEdit = m_roomMgr &&
                   m_roomMgr->signalClient()->isConnected() &&
                   !m_roomMgr->inRoom() &&
                   m_advancedCheck->isChecked();

    auto updateGroup = [canEdit](QComboBox* modeBox,
                                 QComboBox* fecBox,
                                 QComboBox* profileBox) {
        if (!modeBox || !fecBox || !profileBox)
            return;
        TransportMode mode = static_cast<TransportMode>(modeBox->currentData().toInt());
        bool packetMode = (mode == MODE_RELAY_KCP || mode == MODE_RELAY_RAW_UDP);
        bool kcpMode = (mode == MODE_RELAY_KCP);
        modeBox->setEnabled(canEdit);
        fecBox->setEnabled(canEdit && packetMode);
        profileBox->setEnabled(canEdit && kcpMode);
        if (!packetMode)
            fecBox->setCurrentIndex(0);
    };

    updateGroup(m_tcpModeBox, m_tcpFecBox, m_tcpKcpProfileBox);
    updateGroup(m_udpModeBox, m_udpFecBox, m_udpKcpProfileBox);
    if (m_mtuModeBox)
        m_mtuModeBox->setEnabled(canEdit);
}

void MainWindow::promptServerPassword() {
    bool ok = false;
    QString password = QInputDialog::getText(
        this,
        UiStrings::text("dialog.serverAuth.title"),
        UiStrings::text("dialog.serverAuth.prompt"),
        QLineEdit::Password,
        QString(),
        &ok);
    if (!ok || password.isEmpty()) {
        m_roomMgr->disconnectFromServer();
        return;
    }
    m_roomMgr->setServerPassword(password);
    m_roomMgr->continueServerAuth();
}

void MainWindow::setRoomControlsEnabled(bool inRoom) {
    bool connected = m_roomMgr && m_roomMgr->signalClient()->isConnected();
    bool canEditRoom = connected && !inRoom;

    m_createBtn->setEnabled(canEditRoom);
    m_joinBtn->setEnabled(canEditRoom);
    m_refreshBtn->setEnabled(connected);
    m_leaveBtn->setEnabled(connected && inRoom);
    m_roomNameEdit->setEnabled(canEditRoom);
    m_maxPlayersBox->setEnabled(canEditRoom);
    m_passwordEdit->setEnabled(canEditRoom);
    m_advancedCheck->setEnabled(canEditRoom);
    m_advancedOptionsWidget->setEnabled(canEditRoom);
    m_advancedOptionsWidget->setVisible(m_advancedCheck->isChecked());
    updatePolicyControlState();
}

void MainWindow::updateConnectButton(bool connected) {
    m_connectBtn->setText(connected
        ? UiStrings::text("login.disconnect")
        : UiStrings::text("login.connect"));
    m_connectBtn->setObjectName(connected ? "DisconnectBtn" : "ConnectBtn");
    repolish(m_connectBtn);
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (!m_quitRequested && m_trayIcon && m_trayIcon->isVisible()) {
        hide();
        updateTrayState();
        event->ignore();
        return;
    }

    if (!m_quitRequested) {
        quitProgram();
        event->ignore();
        return;
    }
    event->accept();
}

} // namespace VLan

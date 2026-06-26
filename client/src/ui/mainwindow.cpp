#include "mainwindow.h"
#include "roomwidget.h"
#include "log_manager.h"
#include "modern_tray_menu.h"
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
#include <string>

namespace VLan {

namespace {

void repolish(QWidget* widget)
{
    if (!widget) return;
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
}

QWidget* createCard(const QString& title, QVBoxLayout** bodyLayout = nullptr)
{
    QWidget* card = new QWidget();
    card->setObjectName("Card");

    QVBoxLayout* layout = new QVBoxLayout(card);
    layout->setContentsMargins(18, 16, 18, 18);
    layout->setSpacing(12);

    if (!title.isEmpty()) {
        QLabel* label = new QLabel(title);
        label->setObjectName("CardTitle");
        layout->addWidget(label);
    }

    if (bodyLayout) {
        *bodyLayout = layout;
    }
    return card;
}

QWidget* createMetricTile(const QString& key, QLabel** valueLabel, const QString& initialValue)
{
    QWidget* tile = new QWidget();
    tile->setObjectName("MetricTile");
    QVBoxLayout* layout = new QVBoxLayout(tile);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(6);

    QLabel* keyLabel = new QLabel(key);
    keyLabel->setObjectName("MetricKey");
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
      m_trafficLabel(nullptr),
      m_trayIcon(nullptr),
      m_trayMenu(nullptr),
      m_showDetailLog(false)
{
    m_roomMgr = new RoomManager(this);
    setupUI();
    initTray();

    connect(m_roomMgr, &RoomManager::connectionStatusChanged,
            this, &MainWindow::onConnectionStatusChanged);
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
    connect(m_roomMgr->signalClient(), &SignalClient::connectFailed,
            this, &MainWindow::onConnectFailed);

    setRoomControlsEnabled(false);
    refreshDashboardState();
    updateTrayState();

    QScreen* screen = QApplication::primaryScreen();
    QRect avail = screen->availableGeometry();
    int minW = qBound(640, (int)(avail.width() * 0.55), 900);
    int minH = qBound(480, (int)(avail.height() * 0.55), 700);
    setMinimumSize(minW, minH);
    int defW = qBound(minW, (int)(avail.width() * 0.72), 1100);
    int defH = qBound(minH, (int)(avail.height() * 0.82), 880);
    resize(defW, defH);
}

MainWindow::~MainWindow() {}

void MainWindow::setupUI() {
    setWindowTitle(QString::fromUtf8("VLan - 虚拟局域网联机"));

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
    QLabel* brandSubtitle = new QLabel(QString::fromUtf8("Virtual LAN Console"));
    brandSubtitle->setObjectName("BrandSubtitle");
    brandTextLayout->addWidget(brandTitle);
    brandTextLayout->addWidget(brandSubtitle);
    brandLayout->addWidget(brandMark);
    brandLayout->addLayout(brandTextLayout, 1);

    m_navBar = new QListWidget();
    m_navBar->setObjectName("NavList");
    m_navBar->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_navBar->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_navBar->addItem(QString::fromUtf8("登录"));
    m_navBar->addItem(QString::fromUtf8("房间大厅"));
    m_navBar->addItem(QString::fromUtf8("创建房间"));
    m_navBar->addItem(QString::fromUtf8("成员列表"));
    m_navBar->addItem(QString::fromUtf8("运行日志"));
    m_navBar->setCurrentRow(PageLogin);
    connect(m_navBar, &QListWidget::currentRowChanged, this, &MainWindow::changePage);

    QWidget* trafficPanel = new QWidget();
    trafficPanel->setObjectName("SidebarTraffic");
    QVBoxLayout* trafficLayout = new QVBoxLayout(trafficPanel);
    trafficLayout->setContentsMargins(10, 8, 10, 8);
    trafficLayout->setSpacing(4);
    QLabel* trafficTitle = new QLabel(QString::fromUtf8("虚拟网卡速率"));
    trafficTitle->setObjectName("SidebarTrafficTitle");
    trafficTitle->setAlignment(Qt::AlignCenter);
    m_trafficLabel = new QLabel(QString::fromUtf8("↑ 0 B/s  ↓ 0 B/s"));
    m_trafficLabel->setObjectName("SidebarTrafficValue");
    m_trafficLabel->setAlignment(Qt::AlignCenter);
    m_trafficLabel->setWordWrap(true);
    trafficLayout->addWidget(trafficTitle);
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
    m_pageTitleLabel = new QLabel(QString::fromUtf8("登录"));
    m_pageTitleLabel->setObjectName("PageTitle");
    m_pageSubtitleLabel = new QLabel(QString::fromUtf8("连接服务器并登录到 VLan"));
    m_pageSubtitleLabel->setObjectName("PageSubtitle");
    titleLayout->addWidget(m_pageTitleLabel);
    titleLayout->addWidget(m_pageSubtitleLabel);
    QLabel* badge = new QLabel(QString::fromUtf8("VLan Client"));
    badge->setObjectName("ShellBadge");
    pageHeaderLayout->addLayout(titleLayout, 1);
    pageHeaderLayout->addWidget(badge, 0, Qt::AlignTop);
    contentLayout->addWidget(pageHeader);

    m_mainStack = new QStackedWidget();
    m_mainStack->setObjectName("MainStack");
    m_mainStack->addWidget(createLoginPage());
    m_mainStack->addWidget(createLobbyPage());
    m_mainStack->addWidget(createCreateRoomPage());
    m_mainStack->addWidget(createMembersPage());
    m_mainStack->addWidget(createLogPage());
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
    connect(m_serverModeBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onServerModeChanged);
    connect(m_transportModeBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onTransportModeChanged);
    connect(m_detailLogCheck, &QCheckBox::toggled,
            this, &MainWindow::onDetailLogToggled);
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
    connect(m_roomMgr, &RoomManager::natDetected,
            m_roomWidget, &RoomWidget::setNatType);
    connect(m_roomMgr, &RoomManager::serverRttUpdated,
            this, &MainWindow::onServerRttUpdated);
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
    metrics->addWidget(createMetricTile(QString::fromUtf8("服务器状态"), &m_dashConnectionLabel,
                                        QString::fromUtf8("未连接")), 0, 0);
    metrics->addWidget(createMetricTile(QString::fromUtf8("房间状态"), &m_dashRoomLabel,
                                        QString::fromUtf8("未加入房间")), 0, 1);
    metrics->addWidget(createMetricTile(QString::fromUtf8("我的身份"), &m_dashPeerLabel,
                                        QStringLiteral("-")), 0, 2);
    layout->addLayout(metrics);

    QVBoxLayout* connLayout = nullptr;
    QWidget* connCard = createCard(QString::fromUtf8("服务器连接"), &connLayout);

    QFormLayout* form = new QFormLayout();
    form->setContentsMargins(0, 0, 0, 0);
    form->setHorizontalSpacing(12);
    form->setVerticalSpacing(12);
    form->setLabelAlignment(Qt::AlignRight);

    QWidget* serverRow = new QWidget();
    QHBoxLayout* serverRowLayout = new QHBoxLayout(serverRow);
    serverRowLayout->setContentsMargins(0, 0, 0, 0);
    serverRowLayout->setSpacing(10);
    m_serverModeBox = new QComboBox();
    m_serverModeBox->addItem(QString::fromUtf8("默认服务器"));
    m_serverModeBox->addItem(QString::fromUtf8("自定义"));
    m_serverModeBox->setMinimumWidth(132);
    m_serverEdit = new QLineEdit();
    m_serverEdit->setPlaceholderText(QString::fromUtf8("IP或域名:端口"));
    m_serverEdit->setMinimumWidth(220);
    m_serverEdit->setVisible(false);
    serverRowLayout->addWidget(m_serverModeBox);
    serverRowLayout->addWidget(m_serverEdit, 1);
    form->addRow(QString::fromUtf8("服务器"), serverRow);

    m_nameEdit = new QLineEdit();
    m_nameEdit->setPlaceholderText(QString::fromUtf8("纯英文/数字，长度符合协议限制"));
    form->addRow(QString::fromUtf8("昵称"), m_nameEdit);

    QWidget* statusRow = new QWidget();
    QHBoxLayout* statusLayout = new QHBoxLayout(statusRow);
    statusLayout->setContentsMargins(0, 0, 0, 0);
    statusLayout->setSpacing(10);
    m_connStatusLabel = new QLabel(QString::fromUtf8("未连接"));
    m_connStatusLabel->setObjectName("ConnStatusDisconnected");
    m_connStatusLabel->setAlignment(Qt::AlignCenter);
    m_connStatusLabel->setMinimumWidth(96);
    statusLayout->addWidget(m_connStatusLabel);
    statusLayout->addStretch();
    form->addRow(QString::fromUtf8("状态"), statusRow);

    connLayout->addLayout(form);

    QHBoxLayout* connActions = new QHBoxLayout();
    connActions->setContentsMargins(0, 2, 0, 0);
    connActions->addStretch();
    m_connectBtn = new QPushButton(QString::fromUtf8("连接"));
    m_connectBtn->setObjectName("ConnectBtn");
    m_connectBtn->setMinimumWidth(112);
    connActions->addWidget(m_connectBtn);
    connLayout->addLayout(connActions);
    layout->addWidget(connCard);

    QLabel* hint = new QLabel(QString::fromUtf8("提示：关闭主窗口会隐藏到系统托盘，连接与房间会继续保持。"));
    hint->setObjectName("MutedText");
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
    QWidget* listCard = createCard(QString::fromUtf8("房间大厅"), &listLayout);

    QHBoxLayout* toolbar = new QHBoxLayout();
    toolbar->setContentsMargins(0, 0, 0, 0);
    toolbar->setSpacing(10);
    QLabel* hint = new QLabel(QString::fromUtf8("选择房间后加入，也可以双击列表项快速加入。"));
    hint->setObjectName("MutedText");
    toolbar->addWidget(hint, 1);
    m_refreshBtn = new QPushButton(QString::fromUtf8("刷新"));
    m_refreshBtn->setObjectName("SecondaryBtn");
    m_joinBtn = new QPushButton(QString::fromUtf8("加入选中"));
    m_joinBtn->setObjectName("PrimaryBtn");
    m_leaveBtn = new QPushButton(QString::fromUtf8("离开房间"));
    m_leaveBtn->setObjectName("LeaveBtn");
    m_leaveBtn->setEnabled(false);
    toolbar->addWidget(m_refreshBtn);
    toolbar->addWidget(m_joinBtn);
    toolbar->addWidget(m_leaveBtn);
    listLayout->addLayout(toolbar);

    m_roomTable = new QTableWidget(0, 5);
    m_roomTable->setHorizontalHeaderLabels(
        QStringList() << "ID" << QString::fromUtf8("名称")
                      << QString::fromUtf8("人数")
                      << QString::fromUtf8("连接方式")
                      << QString::fromUtf8("MTU"));
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
    QWidget* createCardWidget = createCard(QString::fromUtf8("创建房间"), &formLayoutOuter);

    QFormLayout* form = new QFormLayout();
    form->setContentsMargins(0, 0, 0, 0);
    form->setHorizontalSpacing(14);
    form->setVerticalSpacing(12);
    form->setLabelAlignment(Qt::AlignRight);

    m_roomNameEdit = new QLineEdit();
    m_roomNameEdit->setPlaceholderText(QString::fromUtf8("输入房间名称"));
    form->addRow(QString::fromUtf8("房间名"), m_roomNameEdit);

    m_maxPlayersBox = new QSpinBox();
    m_maxPlayersBox->setRange(2, MAX_PLAYERS);
    m_maxPlayersBox->setValue(8);
    form->addRow(QString::fromUtf8("最大人数"), m_maxPlayersBox);

    m_transportModeBox = new QComboBox();
    m_transportModeBox->addItem(QString::fromUtf8("中继 KCP(UDP)"), MODE_RELAY_KCP);
    m_transportModeBox->addItem(QString::fromUtf8("中继 Raw UDP"), MODE_RELAY_RAW_UDP);
    m_transportModeBox->addItem(QString::fromUtf8("中继 TCP"), MODE_RELAY_TCP);
    m_transportModeBox->addItem(QString::fromUtf8("P2P 直连(不推荐)"), MODE_P2P_ONLY);
    form->addRow(QString::fromUtf8("连接方式"), m_transportModeBox);

    m_fecModeBox = new QComboBox();
    m_fecModeBox->addItem(QString::fromUtf8("无"), FEC_NONE);
    m_fecModeBox->addItem(QString::fromUtf8("FEC 10%"), FEC_10);
    m_fecModeBox->addItem(QString::fromUtf8("FEC 30%"), FEC_30);
    m_fecModeBox->addItem(QString::fromUtf8("FEC 50%"), FEC_50);
    m_fecModeBox->addItem(QString::fromUtf8("FEC 70%"), FEC_70);
    m_fecModeBox->addItem(QString::fromUtf8("FEC 100%"), FEC_100);
    m_fecModeBox->addItem(QString::fromUtf8("FEC 200%"), FEC_200);
    form->addRow(QString::fromUtf8("FEC"), m_fecModeBox);

    m_mtuModeBox = new QComboBox();
    m_mtuModeBox->addItem(QString::fromUtf8("均衡 1400"), ROOM_MTU_BALANCED);
    m_mtuModeBox->addItem(QString::fromUtf8("激进 1420"), ROOM_MTU_AGGRESSIVE);
    m_mtuModeBox->addItem(QString::fromUtf8("稳妥 1280"), ROOM_MTU_SAFE);
    form->addRow(QString::fromUtf8("MTU"), m_mtuModeBox);

    QWidget* encRow = new QWidget();
    QHBoxLayout* encLayout = new QHBoxLayout(encRow);
    encLayout->setContentsMargins(0, 0, 0, 0);
    encLayout->setSpacing(10);
    m_encryptCheck = new QCheckBox(QString::fromUtf8("启用加密"));
    m_passwordEdit = new QLineEdit();
    m_passwordEdit->setEchoMode(QLineEdit::Password);
    m_passwordEdit->setPlaceholderText(QString::fromUtf8("6位以上英文/数字"));
    m_passwordEdit->setVisible(false);
    encLayout->addWidget(m_encryptCheck);
    encLayout->addWidget(m_passwordEdit, 1);
    form->addRow(QString::fromUtf8("加密"), encRow);
    connect(m_encryptCheck, &QCheckBox::toggled, this, [this](bool checked) {
        m_passwordEdit->setVisible(checked);
        m_passwordEdit->setEnabled(checked && m_encryptCheck->isEnabled());
    });

    formLayoutOuter->addLayout(form);

    QHBoxLayout* btnRow = new QHBoxLayout();
    btnRow->addStretch();
    m_createBtn = new QPushButton(QString::fromUtf8("创建房间"));
    m_createBtn->setObjectName("PrimaryBtn");
    m_createBtn->setMinimumWidth(118);
    btnRow->addWidget(m_createBtn);
    formLayoutOuter->addLayout(btnRow);

    layout->addWidget(createCardWidget);
    QLabel* note = new QLabel(QString::fromUtf8("加密模式会弹出风险声明确认，创建前请确认房间用途与网络环境。"));
    note->setObjectName("MutedText");
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
    QLabel* title = new QLabel(QString::fromUtf8("运行日志"));
    title->setObjectName("CardTitle");
    header->addWidget(title);
    header->addStretch();
    m_detailLogCheck = new QCheckBox(QString::fromUtf8("详细日志"));
    m_detailLogCheck->setObjectName("DetailLogCheck");
    m_detailLogCheck->setChecked(false);
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

void MainWindow::changePage(int index)
{
    if (!m_mainStack || index < 0 || index >= m_mainStack->count()) {
        return;
    }

    m_mainStack->setCurrentIndex(index);

    static const QStringList titles = QStringList()
        << QString::fromUtf8("登录")
        << QString::fromUtf8("房间大厅")
        << QString::fromUtf8("创建房间")
        << QString::fromUtf8("成员列表")
        << QString::fromUtf8("运行日志");
    static const QStringList subtitles = QStringList()
        << QString::fromUtf8("连接服务器并登录到 VLan")
        << QString::fromUtf8("浏览可用房间，加入或离开当前房间")
        << QString::fromUtf8("配置人数、连接方式、FEC、MTU 与加密选项")
        << QString::fromUtf8("查看成员、虚拟 IP、传输方式与延迟")
        << QString::fromUtf8("观察连接、房间和隧道运行事件");

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
    m_trayMenu->addHeader(QStringLiteral("VLan"), QString::fromUtf8("未连接"));
    m_trayMenu->addSeparator();
    m_trayMenu->addAction(QStringLiteral("show"), QString::fromUtf8("显示主窗口"));
    m_trayMenu->addAction(QStringLiteral("toggle_connection"), QString::fromUtf8("连接服务器"));
    m_trayMenu->addSeparator();
    m_trayMenu->addAction(QStringLiteral("refresh_rooms"), QString::fromUtf8("刷新房间列表"));
    m_trayMenu->addAction(QStringLiteral("leave_room"), QString::fromUtf8("离开房间"));
    m_trayMenu->addSeparator();
    m_trayMenu->addAction(QStringLiteral("quit"), QString::fromUtf8("退出程序"));

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
    bool connecting = m_roomMgr && m_roomMgr->signalClient()->isConnecting();
    bool inRoom = m_roomMgr && m_roomMgr->inRoom();

    QString stateText;
    if (connecting) {
        stateText = QString::fromUtf8("连接中");
    } else if (connected && inRoom) {
        stateText = QString::fromUtf8("房间 %1 / 已连接").arg(m_roomMgr->currentRoomId());
    } else if (connected) {
        stateText = QString::fromUtf8("已连接");
    } else {
        stateText = QString::fromUtf8("未连接");
    }

    m_trayIcon->setIcon(createTrayIcon(connected, connecting));
    m_trayIcon->setToolTip(QString::fromUtf8("VLan - %1").arg(stateText));

    if (m_trayMenu) {
        m_trayMenu->setHeaderSubtitle(stateText);
        m_trayMenu->setItemText(QStringLiteral("toggle_connection"),
            (connected || connecting) ? QString::fromUtf8("断开服务器")
                                      : QString::fromUtf8("连接服务器"));
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
    bool connecting = m_roomMgr->signalClient()->isConnecting();
    bool inRoom = m_roomMgr->inRoom();

    if (m_dashConnectionLabel) {
        if (connecting) {
            m_dashConnectionLabel->setText(QString::fromUtf8("连接中"));
            m_dashConnectionLabel->setObjectName("MetricValuePending");
        } else if (connected) {
            m_dashConnectionLabel->setText(QString::fromUtf8("已连接"));
            m_dashConnectionLabel->setObjectName("MetricValueGood");
        } else {
            m_dashConnectionLabel->setText(QString::fromUtf8("未连接"));
            m_dashConnectionLabel->setObjectName("MetricValueBad");
        }
        repolish(m_dashConnectionLabel);
    }

    if (m_dashRoomLabel) {
        m_dashRoomLabel->setText(inRoom
            ? QString::fromUtf8("房间 %1").arg(m_roomMgr->currentRoomId())
            : QString::fromUtf8("未加入房间"));
        m_dashRoomLabel->setObjectName(inRoom ? "MetricValueGood" : "MetricValue");
        repolish(m_dashRoomLabel);
    }

    if (m_dashPeerLabel) {
        uint32_t peerId = m_roomMgr->myPeerId();
        m_dashPeerLabel->setText(connected && peerId != 0
            ? QString::fromUtf8("Peer %1").arg(peerId)
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
    m_quitRequested = true;
    if (m_trayIcon) {
        m_trayIcon->hide();
    }
    if (m_roomMgr->inRoom()) {
        m_roomMgr->leaveRoom();
    }
    if (m_roomMgr->signalClient()->isConnected() ||
        m_roomMgr->signalClient()->isConnecting()) {
        m_roomMgr->disconnectFromServer();
    }
    QApplication::quit();
}

// ==================== Slots ====================

void MainWindow::onConnectClicked() {
    if (m_roomMgr->signalClient()->isConnected() ||
        m_roomMgr->signalClient()->isConnecting()) {
        m_roomMgr->disconnectFromServer();
        onConnectionStatusChanged(false);
        updateTrayState();
        return;
    }

    bool isDefault = (m_serverModeBox->currentIndex() == 0);
    QString host;
    quint16 port;

    LogManager::instance().clearMaskedKeywords();
    if (isDefault) {
        host = QStringLiteral("example.invalid");
        port = 11510;
        LogManager::instance().addMaskedKeyword(host);
    } else {
        QString addr = m_serverEdit->text().trimmed();
        if (addr.isEmpty()) {
            QMessageBox::warning(this, QString::fromUtf8("\u63d0\u793a"),
                                 QString::fromUtf8("\u8bf7\u8f93\u5165\u670d\u52a1\u5668\u5730\u5740"));
            return;
        }
        host = addr.section(':', 0, 0);
        port = addr.section(':', 1, 1).toUShort();
        if (port == 0) port = DEFAULT_PORT;
    }

    QString name = m_nameEdit->text().trimmed();
    if (name.isEmpty()) {
        QMessageBox::warning(this, QString::fromUtf8("\u63d0\u793a"),
                             QString::fromUtf8("\u8bf7\u8f93\u5165\u6635\u79f0"));
        return;
    }
    QByteArray nameBytes = name.toUtf8();
    if (!isValidPlayerName(std::string(nameBytes.constData(), nameBytes.size()))) {
        QMessageBox::warning(this, QString::fromUtf8("\u63d0\u793a"),
                             QString::fromUtf8("\u6635\u79f0\u5fc5\u987b\u4e3a%1-%2\u4f4d\u7eaf\u82f1\u6587\u6216\u6570\u5b57")
                             .arg(MIN_PLAYER_NAME_LEN).arg(MAX_PLAYER_NAME_LEN));
        return;
    }

    updateConnectButton(false);
    m_connectBtn->setText(QString::fromUtf8("\u53d6\u6d88"));
    m_connectBtn->setEnabled(true);
    m_serverModeBox->setEnabled(false);
    m_serverEdit->setEnabled(false);
    m_nameEdit->setEnabled(false);
    m_connStatusLabel->setText(QString::fromUtf8("\u8fde\u63a5\u4e2d..."));
    m_connStatusLabel->setObjectName("ConnStatusDisconnected");
    repolish(m_connStatusLabel);

    m_roomMgr->setDefaultServerMode(isDefault);
    m_roomMgr->setServerAddress(host, port);
    m_roomMgr->connectAndLogin(name);
    refreshDashboardState();
    updateTrayState();
}

void MainWindow::onServerModeChanged(int index) {
    m_serverEdit->setVisible(index == 1);
}

void MainWindow::onTransportModeChanged(int index) {
    auto mode = static_cast<TransportMode>(m_transportModeBox->itemData(index).toInt());
    bool fecAllowed = (mode == MODE_RELAY_KCP || mode == MODE_RELAY_RAW_UDP);
    m_fecModeBox->setEnabled(fecAllowed);
    if (!fecAllowed) {
        m_fecModeBox->setCurrentIndex(0);
    }
}

void MainWindow::onCreateRoomClicked() {
    if (m_roomMgr->inRoom())
        return;

    QString name = m_roomNameEdit->text().trimmed();
    QByteArray roomNameBytes = name.toUtf8();
    if (!isValidRoomName(std::string(roomNameBytes.constData(), roomNameBytes.size()))) {
        QMessageBox::warning(this, QString::fromUtf8("\u63d0\u793a"),
                             QString::fromUtf8("\u623f\u95f4\u540d\u4e0d\u80fd\u4e3a\u7a7a\uff0c\u4e14\u4e0d\u80fd\u8d85\u8fc7%1\u5b57\u8282").arg(MAX_ROOM_NAME_LEN));
        return;
    }
    int maxP = m_maxPlayersBox->value();
    if (maxP < 2 || maxP > MAX_PLAYERS) {
        QMessageBox::warning(this, QString::fromUtf8("\u63d0\u793a"),
                             QString::fromUtf8("\u623f\u95f4\u4eba\u6570\u5fc5\u987b\u57282\u5230%1\u4e4b\u95f4").arg(MAX_PLAYERS));
        return;
    }

    bool encrypted = m_encryptCheck->isChecked();
    QString password;
    if (encrypted) {
        password = m_passwordEdit->text();
        QByteArray pwdBytes = password.toUtf8();
        if (!isValidRoomPassword(std::string(pwdBytes.constData(), pwdBytes.size()))) {
            QMessageBox::warning(this, QString::fromUtf8("\u63d0\u793a"),
                                 QString::fromUtf8("\u5bc6\u7801\u5fc5\u987b\u4e3a%1-%2\u4f4d\u7eaf\u82f1\u6587\u5b57\u6bcd\u6216\u6570\u5b57")
                                 .arg(MIN_ROOM_PASSWORD_LEN).arg(MAX_ROOM_PASSWORD_LEN));
            return;
        }
        if (!showGfwWarning()) {
            m_encryptCheck->setChecked(false);
            return;
        }
    }

    auto mode = static_cast<TransportMode>(m_transportModeBox->currentData().toInt());
    auto fec  = static_cast<FecMode>(m_fecModeBox->currentData().toInt());
    uint16_t mtu = normalizeRoomMtu(m_mtuModeBox->currentData().toInt());
    m_roomMgr->createRoom(name, static_cast<uint8_t>(maxP), mode, fec,
                          mtu, encrypted, password);
}

void MainWindow::onJoinRoomClicked() {
    if (m_roomMgr->inRoom() || !m_roomMgr->signalClient()->isConnected())
        return;

    int row = m_roomTable->currentRow();
    if (row < 0) {
        QMessageBox::warning(this, QString::fromUtf8("\u63d0\u793a"),
                             QString::fromUtf8("\u8bf7\u5148\u9009\u62e9\u4e00\u4e2a\u623f\u95f4"));
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
                QMessageBox::warning(this, QString::fromUtf8("\u63d0\u793a"),
                                     QString::fromUtf8("\u8be5\u623f\u95f4\u5df2\u6ee1 (%1/%2)\uff0c\u65e0\u6cd5\u52a0\u5165").arg(current).arg(max));
                return;
            }
        }
    }

    uint32_t roomId = m_roomTable->item(row, 0)->text().toUInt();

    bool isEncrypted = false;
    QTableWidgetItem* modeItem = m_roomTable->item(row, 3);
    if (modeItem && modeItem->data(Qt::UserRole).toBool())
        isEncrypted = true;

    QString password;
    if (isEncrypted) {
        bool ok = false;
        password = QInputDialog::getText(this,
            QString::fromUtf8("\u52a0\u5bc6\u623f\u95f4"),
            QString::fromUtf8("\u8bf7\u8f93\u5165\u623f\u95f4\u5bc6\u7801:"),
            QLineEdit::Password, QString(), &ok);
        if (!ok || password.isEmpty()) return;
        QByteArray pwdBytes = password.toUtf8();
        if (!isValidRoomPassword(std::string(pwdBytes.constData(), pwdBytes.size()))) {
            QMessageBox::warning(this, QString::fromUtf8("\u63d0\u793a"),
                                 QString::fromUtf8("\u5bc6\u7801\u5fc5\u987b\u4e3a%1-%2\u4f4d\u7eaf\u82f1\u6587\u5b57\u6bcd\u6216\u6570\u5b57")
                                 .arg(MIN_ROOM_PASSWORD_LEN).arg(MAX_ROOM_PASSWORD_LEN));
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
        ? QString::fromUtf8("\u5df2\u8fde\u63a5")
        : QString::fromUtf8("\u672a\u8fde\u63a5"));
    m_connStatusLabel->setObjectName(connected
        ? "ConnStatusConnected" : "ConnStatusDisconnected");
    repolish(m_connStatusLabel);

    updateConnectButton(connected);
    m_connectBtn->setEnabled(true);
    m_serverModeBox->setEnabled(!connected);
    m_serverEdit->setEnabled(!connected);
    m_nameEdit->setEnabled(!connected);
    setRoomControlsEnabled(m_roomMgr->inRoom());
    refreshDashboardState();
    updateTrayState();
}

void MainWindow::onConnectFailed(QString reason) {
    m_connStatusLabel->setText(QString::fromUtf8("\u8fde\u63a5\u5931\u8d25"));
    m_connStatusLabel->setObjectName("ConnStatusDisconnected");
    repolish(m_connStatusLabel);
    updateConnectButton(false);
    m_connectBtn->setEnabled(true);
    m_serverModeBox->setEnabled(true);
    m_serverEdit->setEnabled(true);
    m_nameEdit->setEnabled(true);
    onStatusMessage(QString::fromUtf8("\u8fde\u63a5\u5931\u8d25: %1").arg(reason));
    setRoomControlsEnabled(false);
    refreshDashboardState();
    updateTrayState();
}

void MainWindow::onLoggedIn(uint32_t peerId) {
    m_connStatusLabel->setText(
        QString::fromUtf8("\u5df2\u767b\u5f55 (ID=%1)").arg(peerId));
    m_roomMgr->refreshRoomList();
    refreshDashboardState();
    updateTrayState();
}

void MainWindow::onRoomCreated(uint32_t roomId) {
    Q_UNUSED(roomId);
    setRoomControlsEnabled(true);
    m_roomWidget->clear();
    m_roomWidget->setFecMode(m_roomMgr->fecMode());
    m_roomWidget->setMyInfo(m_roomMgr->myPeerId(), m_roomMgr->myVirtualIP());
    if (m_roomMgr->myNatType() != NAT_UNKNOWN)
        m_roomWidget->setNatType(m_roomMgr->myNatType());
    m_roomMgr->refreshRoomList();
    if (m_navBar) m_navBar->setCurrentRow(PageMembers);
    refreshDashboardState();
    updateTrayState();
}

void MainWindow::onRoomJoined(uint32_t roomId) {
    Q_UNUSED(roomId);
    setRoomControlsEnabled(true);
    m_roomWidget->clear();
    m_roomWidget->setFecMode(m_roomMgr->fecMode());
    m_roomWidget->setMyInfo(m_roomMgr->myPeerId(), m_roomMgr->myVirtualIP());
    if (m_roomMgr->myNatType() != NAT_UNKNOWN)
        m_roomWidget->setNatType(m_roomMgr->myNatType());
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
        bool enc = (r.encrypted != 0);

        auto* idItem = new QTableWidgetItem(QString::number(r.roomId));
        idItem->setTextAlignment(Qt::AlignCenter);
        m_roomTable->setItem(i, 0, idItem);

        QString displayName = QString::fromUtf8(r.roomName);
        if (enc) displayName.prepend(QString::fromUtf8("[\u52a0\u5bc6] "));
        auto* nameItem = new QTableWidgetItem(displayName);
        nameItem->setTextAlignment(Qt::AlignCenter);
        m_roomTable->setItem(i, 1, nameItem);

        auto* countItem = new QTableWidgetItem(
            QString("%1/%2").arg(r.playerCount).arg(r.maxPlayers));
        countItem->setTextAlignment(Qt::AlignCenter);
        m_roomTable->setItem(i, 2, countItem);

        QString modeName;
        switch (r.transportMode) {
        case MODE_RELAY_KCP:     modeName = QString::fromUtf8("KCP(UDP)");  break;
        case MODE_RELAY_RAW_UDP: modeName = QString::fromUtf8("Raw UDP");   break;
        case MODE_RELAY_TCP:     modeName = QString::fromUtf8("TCP");       break;
        case MODE_P2P_ONLY:      modeName = QString::fromUtf8("P2P");       break;
        default:                 modeName = QString::fromUtf8("?");         break;
        }
        if (r.fecMode != FEC_NONE)
            modeName += QString(" +%1").arg(fecModeName(r.fecMode));
        if (enc)
            modeName += QString::fromUtf8(" +\u52a0\u5bc6");
        auto* modeItem = new QTableWidgetItem(modeName);
        modeItem->setTextAlignment(Qt::AlignCenter);
        modeItem->setData(Qt::UserRole, enc);
        m_roomTable->setItem(i, 3, modeItem);

        auto* mtuItem = new QTableWidgetItem(QString::number(normalizeRoomMtu(r.mtu)));
        mtuItem->setTextAlignment(Qt::AlignCenter);
        m_roomTable->setItem(i, 4, mtuItem);

        if (enc) {
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
    Q_UNUSED(rttMs);
    refreshDashboardState();
    updateTrayState();
}

void MainWindow::onTunSpeedUpdated(quint64 uploadBytesPerSec,
                                   quint64 downloadBytesPerSec) {
    if (!m_trafficLabel) return;
    m_trafficLabel->setText(QString::fromUtf8("↑ %1  ↓ %2")
                            .arg(formatSpeed(uploadBytesPerSec))
                            .arg(formatSpeed(downloadBytesPerSec)));
}

bool MainWindow::showGfwWarning() {
    QDialog dlg(this);
    dlg.setWindowTitle(QString::fromUtf8("\u26a0 \u52a0\u5bc6\u98ce\u9669\u58f0\u660e"));
    dlg.setMinimumWidth(480);

    QVBoxLayout* lay = new QVBoxLayout(&dlg);
    lay->setSpacing(14);

    QLabel* body = new QLabel(QString::fromUtf8(
        "\u542f\u7528\u52a0\u5bc6\u901a\u4fe1\u53ef\u80fd\u5e26\u6765\u4ee5\u4e0b\u98ce\u9669\uff1a\n\n"
        "1. \u672c\u529f\u80fd\u4ec5\u52a0\u5bc6\u4f20\u8f93\u5c42\u53ca\u4ee5\u4e0a\u7684\u8f7d\u8377\u6570\u636e\uff08\u5982\u6e38\u620f\u6570\u636e\u5305\u5185\u5bb9\uff09\uff0c\n"
        "   IP \u5934\u90e8\u4fe1\u606f\uff08\u865a\u62df\u5730\u5740\u3001\u534f\u8bae\u7c7b\u578b\u7b49\uff09\u4ecd\u4ee5\u660e\u6587\u4f20\u8f93\uff0c\n"
        "   \u4e0d\u63d0\u4f9b\u5b8c\u6574\u7684\u901a\u4fe1\u9690\u533f\u4fdd\u62a4\u3002\u4fdd\u7559 IP \u5934\u660e\u6587\u662f\u4e3a\u4e86\u964d\u4f4e\n"
        "   \u6d41\u91cf\u88ab\u5ba1\u67e5\u7cfb\u7edf\u8bef\u5224\u4e3a\u7ffb\u5899\u534f\u8bae\u7684\u98ce\u9669\uff0c\u540c\u65f6\u4ecd\u80fd\u4fdd\u62a4\n"
        "   \u5b9e\u9645\u901a\u4fe1\u5185\u5bb9\u4e0d\u88ab\u7b2c\u4e09\u65b9\u7aa5\u63a2\u3002\n\n"
        "2. \u52a0\u5bc6\u6d41\u91cf\u53ef\u80fd\u88ab\u7f51\u7edc\u5ba1\u67e5\u7cfb\u7edf\uff08GFW/\u8fd0\u8425\u5546\uff09\u6807\u8bb0\u4e3a\u53ef\u7591\u6d41\u91cf\uff0c\n"
        "   \u5bfc\u81f4\u670d\u52a1\u5668 IP \u88ab\u5c01\u7981\u6216\u8fde\u63a5\u4e2d\u65ad\u3002\n\n"
        "3. \u672c\u8f6f\u4ef6\u4ec5\u7528\u4e8e\u865a\u62df\u5c40\u57df\u7f51\u7ec4\u5efa\u548c\u6e38\u620f\u8054\u673a\uff0c\u4e0d\u63d0\u4f9b\u4efb\u4f55\u7ffb\u5899\u6216\n"
        "   \u79d1\u5b66\u4e0a\u7f51\u529f\u80fd\u3002\u7528\u6237\u4e0d\u5f97\u5c06\u672c\u8f6f\u4ef6\u7528\u4e8e\u975e\u6cd5\u7528\u9014\u3002\n\n"
        "4. \u56e0\u542f\u7528\u52a0\u5bc6\u5bfc\u81f4\u7684\u4efb\u4f55\u8fde\u63a5\u95ee\u9898\u6216\u6cd5\u5f8b\u98ce\u9669\uff0c\u7531\u7528\u6237\u81ea\u884c\u627f\u62c5\u3002\n\n"
        "\u8bf7\u786e\u8ba4\u60a8\u5df2\u9605\u8bfb\u5e76\u7406\u89e3\u4ee5\u4e0a\u58f0\u660e\u3002"
    ));
    body->setWordWrap(true);
    lay->addWidget(body);

    QDialogButtonBox* buttons = new QDialogButtonBox();
    QPushButton* agreeBtn = buttons->addButton(
        QString::fromUtf8("\u540c\u610f"), QDialogButtonBox::AcceptRole);
    QPushButton* disagreeBtn = buttons->addButton(
        QString::fromUtf8("\u4e0d\u540c\u610f"), QDialogButtonBox::RejectRole);
    agreeBtn->setEnabled(false);
    lay->addWidget(buttons);

    QTimer::singleShot(3000, agreeBtn, [agreeBtn]() { agreeBtn->setEnabled(true); });

    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    return dlg.exec() == QDialog::Accepted;
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
    m_transportModeBox->setEnabled(canEditRoom);
    m_fecModeBox->setEnabled(canEditRoom);
    m_mtuModeBox->setEnabled(canEditRoom);
    m_encryptCheck->setEnabled(canEditRoom);
    m_passwordEdit->setEnabled(canEditRoom && m_encryptCheck->isChecked());

    if (canEditRoom) {
        onTransportModeChanged(m_transportModeBox->currentIndex());
    } else {
        m_fecModeBox->setEnabled(false);
    }
}

void MainWindow::updateConnectButton(bool connected) {
    m_connectBtn->setText(connected
        ? QString::fromUtf8("\u65ad\u5f00")
        : QString::fromUtf8("\u8fde\u63a5"));
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

    if (m_roomMgr->inRoom()) {
        m_roomMgr->leaveRoom();
    }
    if (m_roomMgr->signalClient()->isConnected() ||
        m_roomMgr->signalClient()->isConnecting()) {
        m_roomMgr->disconnectFromServer();
    }
    event->accept();
}

} // namespace VLan

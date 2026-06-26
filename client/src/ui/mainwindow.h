#ifndef VLAN_MAINWINDOW_H
#define VLAN_MAINWINDOW_H

#include <QMainWindow>
#include <QTableWidget>
#include <QPushButton>
#include <QLineEdit>
#include <QLabel>
#include <QSpinBox>
#include <QTextEdit>
#include <QComboBox>
#include <QCheckBox>
#include <QCloseEvent>
#include <QListWidget>
#include <QStackedWidget>
#include <QSystemTrayIcon>
#include "protocol.h"

namespace VLan {

class RoomManager;
class RoomWidget;
class ModernTrayMenu;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow();

private slots:
    void onConnectClicked();
    void onCreateRoomClicked();
    void onJoinRoomClicked();
    void onLeaveRoomClicked();
    void onRefreshClicked();
    void onServerModeChanged(int index);
    void onTransportModeChanged(int index);
    void changePage(int index);

    void onConnectionStatusChanged(bool connected);
    void onLoggedIn(uint32_t peerId);
    void onRoomCreated(uint32_t roomId);
    void onRoomJoined(uint32_t roomId);
    void onRoomLeft();
    void onRoomListUpdated(QList<VLan::RoomListItem> rooms);
    void onStatusMessage(QString msg);
    void onErrorOccurred(QString msg);
    void onConnectFailed(QString reason);
    void onServerRttUpdated(int rttMs);
    void onTunSpeedUpdated(quint64 uploadBytesPerSec, quint64 downloadBytesPerSec);
    void onLogMessage(QString formattedHtml, int level);
    void onDetailLogToggled(bool checked);
    void onTrayIconActivated(QSystemTrayIcon::ActivationReason reason);
    void onTrayMenuItemClicked(const QString& id);
    void showFromTray();
    void quitProgram();

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    enum PageIndex {
        PageLogin = 0,
        PageLobby = 1,
        PageCreateRoom = 2,
        PageMembers = 3,
        PageLog = 4
    };

    void setupUI();
    QWidget* createLoginPage();
    QWidget* createLobbyPage();
    QWidget* createCreateRoomPage();
    QWidget* createMembersPage();
    QWidget* createLogPage();
    void initTray();
    void updateTrayState();
    void refreshDashboardState();
    void setRoomControlsEnabled(bool inRoom);
    void updateConnectButton(bool connected);
    bool showGfwWarning();

    RoomManager* m_roomMgr;
    RoomWidget*  m_roomWidget;
    bool         m_quitRequested;

    /* Shell */
    QListWidget*    m_navBar;
    QStackedWidget* m_mainStack;
    QLabel*         m_pageTitleLabel;
    QLabel*         m_pageSubtitleLabel;
    QLabel*         m_dashConnectionLabel;
    QLabel*         m_dashRoomLabel;
    QLabel*         m_dashPeerLabel;
    QLabel*         m_trafficLabel;

    /* Tray */
    QSystemTrayIcon* m_trayIcon;
    ModernTrayMenu*  m_trayMenu;

    /* Connection panel */
    QComboBox*   m_serverModeBox;
    QLineEdit*   m_serverEdit;
    QLineEdit*   m_nameEdit;
    QPushButton* m_connectBtn;
    QLabel*      m_connStatusLabel;

    /* Room list */
    QTableWidget* m_roomTable;
    QPushButton*  m_refreshBtn;

    /* Room creation */
    QLineEdit*   m_roomNameEdit;
    QSpinBox*    m_maxPlayersBox;
    QComboBox*   m_transportModeBox;
    QComboBox*   m_fecModeBox;
    QComboBox*   m_mtuModeBox;
    QCheckBox*   m_encryptCheck;
    QLineEdit*   m_passwordEdit;
    QPushButton* m_createBtn;
    QPushButton* m_joinBtn;
    QPushButton* m_leaveBtn;

    /* Log */
    QTextEdit*   m_logEdit;
    QCheckBox*   m_detailLogCheck;
    bool         m_showDetailLog;
};

} // namespace VLan
#endif // VLAN_MAINWINDOW_H

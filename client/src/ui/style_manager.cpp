#include "style_manager.h"

namespace VLan {

QString StyleManager::getStyleSheet()
{
    return QString::fromUtf8(R"(
/* ==================== Base ==================== */
* {
    font-family: 'Microsoft YaHei', 'Segoe UI';
    outline: none;
}

QMainWindow,
QWidget#CentralWidget,
QWidget#ContentPane,
QWidget#PageScrollContent,
QWidget#LobbyPage,
QWidget#MembersPage,
QWidget#LogPage {
    background: #f7fbff;
    color: #243041;
}

QWidget {
    color: #243041;
    font-size: 13px;
}

QScrollArea#PageScrollArea {
    background: transparent;
    border: none;
}

QScrollArea#PageScrollArea > QWidget > QWidget {
    background: #f7fbff;
}

/* ==================== Shell ==================== */
QWidget#Sidebar {
    background: #ffffff;
    border-right: 1px solid #dce9f6;
}

QWidget#BrandPanel {
    background: qlineargradient(x1:0, y1:0, x2:1, y2:1, stop:0 #ffffff, stop:0.55 #f0fcfa, stop:1 #f4f8ff);
    border: 1px solid #dce9f6;
    border-radius: 8px;
}

QLabel#BrandMark {
    background: #2ec4b6;
    color: #ffffff;
    border-radius: 8px;
    font-size: 18px;
    font-weight: 900;
}

QLabel#BrandTitle {
    color: #243041;
    font-size: 18px;
    font-weight: 900;
}

QLabel#BrandSubtitle {
    color: #7b8794;
    font-size: 11px;
    font-weight: 700;
}

QWidget#SidebarTraffic {
    color: #21897f;
    background: #effcf9;
    border: 1px solid #c9f2ea;
    border-radius: 8px;
}

QLabel#SidebarTrafficTitle {
    color: #5f6f7c;
    font-size: 11px;
    font-weight: 800;
}

QLabel#SidebarTrafficValue {
    color: #147a72;
    font-size: 12px;
    font-weight: 900;
}

QListWidget#NavList {
    background: transparent;
    border: none;
    color: #536274;
    font-size: 14px;
    padding: 0;
}

QListWidget#NavList::item {
    height: 40px;
    margin: 3px 0;
    padding-left: 14px;
    border-radius: 8px;
    font-weight: 800;
}

QListWidget#NavList::item:hover {
    background: #f4f8ff;
    color: #243041;
}

QListWidget#NavList::item:selected {
    background: #eafff9;
    color: #147a72;
    border-left: 4px solid #43d0bd;
}

QWidget#PageHeader {
    background: transparent;
}

QLabel#PageTitle {
    color: #243041;
    font-size: 25px;
    font-weight: 900;
}

QLabel#PageSubtitle {
    color: #6d7782;
    font-size: 12px;
    font-weight: 700;
}

QLabel#ShellBadge {
    color: #21897f;
    background: #effcf9;
    border: 1px solid #c9f2ea;
    border-radius: 8px;
    padding: 6px 11px;
    font-size: 11px;
    font-weight: 900;
}

/* ==================== Surfaces ==================== */
QWidget#Card,
QWidget#CardPanel,
QWidget#MetricTile {
    background: #ffffff;
    border: 1px solid #dce9f6;
    border-radius: 8px;
}

QWidget#MetricTile:hover,
QWidget#Card:hover {
    border-color: #c9f2ea;
}

QLabel#CardTitle,
QLabel#SectionTitle {
    color: #243041;
    font-size: 16px;
    font-weight: 900;
    padding: 2px 0;
}

QLabel#MetricKey,
QLabel#FieldLabel {
    color: #7b8794;
    font-size: 12px;
    font-weight: 800;
}

QLabel#MetricValue,
QLabel#MetricValueMuted,
QLabel#MetricValueGood,
QLabel#MetricValueBad,
QLabel#MetricValuePending {
    font-size: 18px;
    font-weight: 900;
}

QLabel#MetricValue {
    color: #243041;
}

QLabel#MetricValueMuted {
    color: #9aa5b1;
}

QLabel#MetricValueGood {
    color: #21897f;
}

QLabel#MetricValueBad {
    color: #ef4f7d;
}

QLabel#MetricValuePending {
    color: #c98200;
}

QLabel#MutedText,
QLabel#NatLabel {
    color: #7b8794;
    font-size: 12px;
    font-weight: 600;
}

QLabel#InfoLabel {
    color: #21897f;
    font-size: 14px;
    font-weight: 900;
}

/* ==================== Status ==================== */
QLabel#ConnStatusConnected,
QLabel#ConnStatusDisconnected,
QLabel#QualityExcellent,
QLabel#QualityGood,
QLabel#QualityFair,
QLabel#QualityPoor {
    border-radius: 8px;
    padding: 5px 10px;
    font-size: 12px;
    font-weight: 900;
}

QLabel#ConnStatusConnected,
QLabel#QualityExcellent {
    color: #147a72;
    background: #eafff9;
    border: 1px solid #c9f2ea;
}

QLabel#ConnStatusDisconnected,
QLabel#QualityPoor {
    color: #c24134;
    background: #fff0ed;
    border: 1px solid #ffd2cb;
}

QLabel#QualityGood {
    color: #2f6cad;
    background: #eef6ff;
    border: 1px solid #cce3ff;
}

QLabel#QualityFair {
    color: #a16a00;
    background: #fff8e8;
    border: 1px solid #ffe6aa;
}

/* ==================== Buttons ==================== */
QPushButton {
    background: #2ec4b6;
    color: #ffffff;
    border: none;
    border-radius: 8px;
    padding: 8px 16px;
    min-height: 20px;
    font-size: 13px;
    font-weight: 800;
}

QPushButton:hover {
    background: #21ad9c;
}

QPushButton:pressed {
    background: #188f82;
}

QPushButton:disabled {
    background: #e0e6ec;
    color: #9aa5b1;
}

QPushButton#PrimaryBtn,
QPushButton#ConnectBtn {
    background: #2ec4b6;
    color: #ffffff;
}

QPushButton#PrimaryBtn:hover,
QPushButton#ConnectBtn:hover {
    background: #21ad9c;
}

QPushButton#SecondaryBtn {
    background: #ffffff;
    color: #394456;
    border: 1px solid #dce9f6;
}

QPushButton#SecondaryBtn:hover {
    background: #f4f8ff;
    border-color: #bcdcf9;
}

QPushButton#DisconnectBtn {
    background: #ff6b8f;
}

QPushButton#DisconnectBtn:hover {
    background: #ef4f7d;
}

QPushButton#LeaveBtn {
    background: #f5ad42;
    color: #ffffff;
}

QPushButton#LeaveBtn:hover {
    background: #df9630;
}

/* ==================== Inputs ==================== */
QLineEdit,
QComboBox,
QSpinBox {
    background: #fbfdff;
    border: 1px solid #cfddeb;
    border-radius: 8px;
    padding: 7px 10px;
    min-height: 20px;
    color: #243041;
    font-size: 13px;
}

QLineEdit:focus,
QComboBox:focus,
QSpinBox:focus {
    border-color: #43d0bd;
    background: #ffffff;
}

QLineEdit:disabled,
QComboBox:disabled,
QSpinBox:disabled {
    background: #eef3f7;
    border-color: #dfe8f1;
    color: #9aa5b1;
}

QComboBox::drop-down {
    border: none;
    width: 26px;
}

QComboBox QAbstractItemView {
    background: #ffffff;
    border: 1px solid #dce9f6;
    selection-background-color: #eafff9;
    selection-color: #147a72;
}

QCheckBox {
    color: #536274;
    font-size: 13px;
    font-weight: 700;
    spacing: 6px;
}

QCheckBox::indicator {
    width: 15px;
    height: 15px;
    border-radius: 4px;
    border: 1px solid #9fb0c0;
    background: #ffffff;
}

QCheckBox::indicator:hover {
    border-color: #43d0bd;
}

QCheckBox::indicator:checked {
    background: #2ec4b6;
    border-color: #2ec4b6;
}

/* ==================== Tables ==================== */
QTableWidget {
    background: #ffffff;
    border: 1px solid #dce9f6;
    border-radius: 8px;
    color: #394456;
    gridline-color: #eef3f7;
    font-size: 12px;
    alternate-background-color: #fbfdff;
}

QTableWidget::item {
    padding: 7px 8px;
    border-bottom: 1px solid #eef3f7;
}

QTableWidget::item:selected {
    background: #eafff9;
    color: #147a72;
}

QTableWidget::item:hover {
    background: #f4f8ff;
}

QHeaderView::section {
    background: #f4f8ff;
    color: #647282;
    border: none;
    border-bottom: 1px solid #dce9f6;
    padding: 8px;
    font-size: 12px;
    font-weight: 900;
}

/* ==================== Log ==================== */
QTextEdit#LogConsole {
    background: #17202b;
    border: 1px solid #263445;
    border-radius: 8px;
    color: #c6d3e1;
    font-family: 'Consolas', 'Courier New';
    font-size: 12px;
    padding: 10px;
    selection-background-color: #355066;
    selection-color: #ffffff;
}

/* ==================== Scrollbar ==================== */
QScrollBar:vertical {
    background: transparent;
    width: 8px;
    margin: 0;
}

QScrollBar::handle:vertical {
    background: #c9d7e6;
    border-radius: 4px;
    min-height: 30px;
}

QScrollBar::handle:vertical:hover {
    background: #9fb0c0;
}

QScrollBar::add-line:vertical,
QScrollBar::sub-line:vertical {
    height: 0;
}

QScrollBar::add-page:vertical,
QScrollBar::sub-page:vertical {
    background: transparent;
}

QScrollBar:horizontal {
    background: transparent;
    height: 8px;
    margin: 0;
}

QScrollBar::handle:horizontal {
    background: #c9d7e6;
    border-radius: 4px;
    min-width: 30px;
}

QScrollBar::handle:horizontal:hover {
    background: #9fb0c0;
}

QScrollBar::add-line:horizontal,
QScrollBar::sub-line:horizontal {
    width: 0;
}

QScrollBar::add-page:horizontal,
QScrollBar::sub-page:horizontal {
    background: transparent;
}

QToolTip {
    background: #243041;
    color: #ffffff;
    border: 1px solid #394456;
    border-radius: 6px;
    padding: 6px 10px;
}
)");
}

} // namespace VLan

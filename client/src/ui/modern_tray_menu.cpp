#include "modern_tray_menu.h"

#include <QApplication>
#include <QCursor>
#include <QFont>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>

namespace VLan {

namespace {

QFont trayFont(int pixelSize, int weight = QFont::Medium)
{
    QFont font(QStringLiteral("Microsoft YaHei"));
    font.setPixelSize(pixelSize);
    font.setWeight(weight);
    return font;
}

} // namespace

ModernTrayMenu::ModernTrayMenu(QWidget* parent)
    : QWidget(parent), m_hoveredIndex(-1), m_pressedIndex(-1)
{
    setWindowFlags(Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setMouseTracking(true);
}

void ModernTrayMenu::clear()
{
    m_items.clear();
    m_hoveredIndex = -1;
    m_pressedIndex = -1;
}

void ModernTrayMenu::addHeader(const QString& title, const QString& subtitle)
{
    MenuItem item;
    item.type = MenuItem::Header;
    item.text = title;
    item.subtitle = subtitle;
    m_items.append(item);
}

void ModernTrayMenu::addAction(const QString& id, const QString& text)
{
    MenuItem item;
    item.type = MenuItem::Action;
    item.id = id;
    item.text = text;
    m_items.append(item);
}

void ModernTrayMenu::addSeparator()
{
    MenuItem item;
    item.type = MenuItem::Separator;
    item.enabled = false;
    m_items.append(item);
}

void ModernTrayMenu::setHeaderSubtitle(const QString& subtitle)
{
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items[i].type == MenuItem::Header) {
            m_items[i].subtitle = subtitle;
            update();
            return;
        }
    }
}

void ModernTrayMenu::setItemText(const QString& id, const QString& text)
{
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items[i].id == id) {
            m_items[i].text = text;
            update();
            return;
        }
    }
}

void ModernTrayMenu::setItemEnabled(const QString& id, bool enabled)
{
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items[i].id == id) {
            m_items[i].enabled = enabled;
            update();
            return;
        }
    }
}

QRectF ModernTrayMenu::contentRect() const
{
    return QRectF(ShadowMargin, ShadowMargin,
                  width() - ShadowMargin * 2,
                  height() - ShadowMargin * 2);
}

int ModernTrayMenu::totalHeight() const
{
    int height = 0;
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items[i].type == MenuItem::Header) {
            height += HeaderHeight;
        } else if (m_items[i].type == MenuItem::Separator) {
            height += SeparatorHeight;
        } else {
            height += ActionHeight;
        }
    }
    return height;
}

QRect ModernTrayMenu::itemRect(int index) const
{
    QRectF cr = contentRect();
    int y = ShadowMargin + 6;
    for (int i = 0; i < m_items.size(); ++i) {
        int height = ActionHeight;
        if (m_items[i].type == MenuItem::Header) {
            height = HeaderHeight;
        } else if (m_items[i].type == MenuItem::Separator) {
            height = SeparatorHeight;
        }

        if (i == index) {
            return QRect(static_cast<int>(cr.x()) + 6, y,
                         static_cast<int>(cr.width()) - 12, height);
        }
        y += height;
    }
    return QRect();
}

int ModernTrayMenu::itemAtPos(const QPoint& pos) const
{
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items[i].type != MenuItem::Action || !m_items[i].enabled) {
            continue;
        }
        if (itemRect(i).contains(pos)) {
            return i;
        }
    }
    return -1;
}

void ModernTrayMenu::updateMenuSize()
{
    setFixedSize(MenuWidth + ShadowMargin * 2,
                 totalHeight() + ShadowMargin * 2 + 12);
}

void ModernTrayMenu::showAtPosition(const QPoint& pos)
{
    updateMenuSize();

    QScreen* screen = QGuiApplication::primaryScreen();
    QRect screenRect = screen ? screen->availableGeometry() : QRect(0, 0, 1024, 768);

    int x = pos.x() - width() / 2;
    int y = pos.y() - height() - 6;

    if (x < screenRect.left()) x = screenRect.left();
    if (x + width() > screenRect.right()) x = screenRect.right() - width();
    if (y < screenRect.top()) y = pos.y() + 6;
    if (y + height() > screenRect.bottom()) y = screenRect.bottom() - height();

    move(x, y);
    show();
    raise();
    update();
}

void ModernTrayMenu::showAtCursor()
{
    showAtPosition(QCursor::pos());
}

void ModernTrayMenu::drawShadow(QPainter& painter, const QRectF& rect)
{
    for (int i = 0; i < 8; ++i) {
        QColor shadow(24, 48, 72, 18 - i * 2);
        QPainterPath path;
        path.addRoundedRect(rect.adjusted(-i, -i, i, i),
                            CornerRadius + i, CornerRadius + i);
        painter.fillPath(path, shadow);
    }
}

void ModernTrayMenu::drawHeader(QPainter& painter, const QRect& rect, const MenuItem& item)
{
    QRect badge(rect.left() + 14, rect.top() + 14, 34, 34);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor("#2ec4b6"));
    painter.drawRoundedRect(badge, 9, 9);

    painter.setPen(QColor("#ffffff"));
    painter.setFont(trayFont(18, QFont::Black));
    painter.drawText(badge, Qt::AlignCenter, QStringLiteral("V"));

    QRect textRect(rect.left() + 58, rect.top() + 12, rect.width() - 72, 20);
    painter.setPen(QColor("#243041"));
    painter.setFont(trayFont(14, QFont::Bold));
    painter.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, item.text);

    QRect subRect(rect.left() + 58, rect.top() + 34, rect.width() - 72, 18);
    painter.setPen(QColor("#6d7782"));
    painter.setFont(trayFont(11, QFont::Medium));
    painter.drawText(subRect, Qt::AlignLeft | Qt::AlignVCenter, item.subtitle);
}

void ModernTrayMenu::drawAction(QPainter& painter, const QRect& rect, const MenuItem& item,
                                bool hovered, bool pressed)
{
    QColor bg = Qt::transparent;
    if (pressed) {
        bg = QColor("#eafff9");
    } else if (hovered) {
        bg = QColor("#f4f8ff");
    }
    if (bg.alpha() > 0) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(bg);
        painter.drawRoundedRect(rect.adjusted(2, 2, -2, -2), 8, 8);
    }

    QColor textColor = item.enabled ? QColor("#394456") : QColor("#aab4bf");
    painter.setPen(textColor);
    painter.setFont(trayFont(13, QFont::DemiBold));
    painter.drawText(rect.adjusted(16, 0, -16, 0),
                     Qt::AlignLeft | Qt::AlignVCenter, item.text);
}

void ModernTrayMenu::drawSeparator(QPainter& painter, const QRect& rect)
{
    painter.setPen(QColor("#e5eef7"));
    int y = rect.center().y();
    painter.drawLine(rect.left() + 14, y, rect.right() - 14, y);
}

void ModernTrayMenu::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    QRectF cr = contentRect();
    drawShadow(painter, cr);

    QPainterPath surface;
    surface.addRoundedRect(cr, CornerRadius, CornerRadius);
    painter.fillPath(surface, QColor("#ffffff"));
    painter.setPen(QColor("#dce9f6"));
    painter.drawPath(surface);

    for (int i = 0; i < m_items.size(); ++i) {
        QRect rect = itemRect(i);
        const MenuItem& item = m_items[i];
        if (item.type == MenuItem::Header) {
            drawHeader(painter, rect, item);
        } else if (item.type == MenuItem::Separator) {
            drawSeparator(painter, rect);
        } else {
            drawAction(painter, rect, item,
                       i == m_hoveredIndex, i == m_pressedIndex);
        }
    }
}

void ModernTrayMenu::mouseMoveEvent(QMouseEvent* event)
{
    int index = itemAtPos(event->pos());
    if (index != m_hoveredIndex) {
        m_hoveredIndex = index;
        update();
    }
}

void ModernTrayMenu::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        m_pressedIndex = itemAtPos(event->pos());
        update();
    }
}

void ModernTrayMenu::mouseReleaseEvent(QMouseEvent* event)
{
    int released = itemAtPos(event->pos());
    int pressed = m_pressedIndex;
    m_pressedIndex = -1;
    update();

    if (released >= 0 && released == pressed) {
        QString id = m_items[released].id;
        hide();
        emit itemClicked(id);
    }
}

void ModernTrayMenu::leaveEvent(QEvent* event)
{
    Q_UNUSED(event);
    m_hoveredIndex = -1;
    m_pressedIndex = -1;
    update();
}

} // namespace VLan

#ifndef VLAN_MODERN_TRAY_MENU_H
#define VLAN_MODERN_TRAY_MENU_H

#include <QRect>
#include <QString>
#include <QVector>
#include <QWidget>

class QEvent;
class QMouseEvent;
class QPaintEvent;
class QPainter;

namespace VLan {

class ModernTrayMenu : public QWidget {
    Q_OBJECT
public:
    struct MenuItem {
        enum Type {
            Header,
            Action,
            Separator
        };

        Type type;
        QString id;
        QString text;
        QString subtitle;
        bool enabled;

        MenuItem() : type(Action), enabled(true) {}
    };

    explicit ModernTrayMenu(QWidget* parent = nullptr);

    void clear();
    void addHeader(const QString& title, const QString& subtitle = QString());
    void addAction(const QString& id, const QString& text);
    void addSeparator();

    void setHeaderSubtitle(const QString& subtitle);
    void setItemText(const QString& id, const QString& text);
    void setItemEnabled(const QString& id, bool enabled);

    void showAtCursor();

signals:
    void itemClicked(const QString& id);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    QRectF contentRect() const;
    QRect itemRect(int index) const;
    int itemAtPos(const QPoint& pos) const;
    int totalHeight() const;
    void updateMenuSize();
    void showAtPosition(const QPoint& pos);

    void drawShadow(QPainter& painter, const QRectF& rect);
    void drawHeader(QPainter& painter, const QRect& rect, const MenuItem& item);
    void drawAction(QPainter& painter, const QRect& rect, const MenuItem& item,
                    bool hovered, bool pressed);
    void drawSeparator(QPainter& painter, const QRect& rect);

    QVector<MenuItem> m_items;
    int m_hoveredIndex;
    int m_pressedIndex;

    static const int ShadowMargin = 12;
    static const int MenuWidth = 272;
    static const int HeaderHeight = 60;
    static const int ActionHeight = 40;
    static const int SeparatorHeight = 15;
    static const int CornerRadius = 8;
};

} // namespace VLan

#endif // VLAN_MODERN_TRAY_MENU_H

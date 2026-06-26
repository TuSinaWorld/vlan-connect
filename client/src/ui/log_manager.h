#ifndef VLAN_LOG_MANAGER_H
#define VLAN_LOG_MANAGER_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QList>
#include <QMutex>

namespace VLan {

enum LogLevel {
    LOG_LEVEL_NORMAL = 0,
    LOG_LEVEL_DETAIL = 1
};

enum LogColor {
    LOG_COLOR_INFO  = 0,
    LOG_COLOR_ERROR = 1,
    LOG_COLOR_DEBUG = 2
};

struct LogEntry {
    QString  timestamp;
    QString  message;
    LogLevel level;
    LogColor color;
};

class LogManager : public QObject {
    Q_OBJECT
public:
    static LogManager& instance();

    void installHandler();

    void logNormal(const QString& msg);
    void logError(const QString& msg);
    void logDetail(const QString& msg);

    void addMaskedKeyword(const QString& keyword);
    void clearMaskedKeywords();

    const QList<LogEntry>& allEntries() const { return m_entries; }
    static QString formatHtml(const LogEntry& e);

signals:
    void logMessage(QString formattedHtml, int level);

private:
    explicit LogManager(QObject* parent = nullptr);

    void appendEntry(const QString& msg, LogLevel level, LogColor color);
    QString maskSensitive(const QString& msg) const;
    static void messageHandler(QtMsgType type, const QMessageLogContext& ctx,
                               const QString& msg);

    QList<LogEntry> m_entries;
    QStringList     m_maskedKeywords;
    QMutex          m_mutex;

    static const int MAX_ENTRIES = 2000;
};

} // namespace VLan
#endif // VLAN_LOG_MANAGER_H

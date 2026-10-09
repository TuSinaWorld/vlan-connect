#include "log_manager.h"
#include <QDateTime>
#include <QMutexLocker>
#include <QDir>
#include <QFile>
#include <QStandardPaths>

namespace VLan {

LogManager& LogManager::instance() {
    static LogManager s;
    return s;
}

LogManager::LogManager(QObject* parent) : QObject(parent) {}

void LogManager::installHandler() {
    qInstallMessageHandler(LogManager::messageHandler);
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    const QString directory = base + "/logs";
    if (!base.isEmpty() && QDir().mkpath(directory)) {
        m_logPath = directory + "/client.log";
        QFile file(m_logPath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Append))
            m_logPath.clear();
    }
    if (m_logPath.isEmpty())
        logError(QStringLiteral("[log] Cannot save diagnostic logs; check local application data permissions"));
    else
        logNormal(QString("[log] Diagnostic log: %1").arg(m_logPath));
}

void LogManager::messageHandler(QtMsgType type, const QMessageLogContext&,
                                const QString& msg)
{
    LogManager& lm = instance();
    switch (type) {
    case QtDebugMsg:
        lm.appendEntry(msg, LOG_LEVEL_DETAIL, LOG_COLOR_DEBUG);
        break;
    case QtWarningMsg:
        lm.appendEntry(msg, LOG_LEVEL_NORMAL, LOG_COLOR_ERROR);
        break;
    case QtCriticalMsg:
    case QtFatalMsg:
        lm.appendEntry(msg, LOG_LEVEL_NORMAL, LOG_COLOR_ERROR);
        break;
    default:
        lm.appendEntry(msg, LOG_LEVEL_DETAIL, LOG_COLOR_DEBUG);
        break;
    }
}

void LogManager::logNormal(const QString& msg) {
    appendEntry(msg, LOG_LEVEL_NORMAL, LOG_COLOR_INFO);
}

void LogManager::logError(const QString& msg) {
    appendEntry(msg, LOG_LEVEL_NORMAL, LOG_COLOR_ERROR);
}

void LogManager::logDetail(const QString& msg) {
    appendEntry(msg, LOG_LEVEL_DETAIL, LOG_COLOR_DEBUG);
}

void LogManager::addMaskedKeyword(const QString& keyword) {
    if (!keyword.isEmpty() && !m_maskedKeywords.contains(keyword))
        m_maskedKeywords.append(keyword);
}

void LogManager::clearMaskedKeywords() {
    m_maskedKeywords.clear();
}

QString LogManager::maskSensitive(const QString& msg) const {
    if (m_maskedKeywords.isEmpty()) return msg;
    QString result = msg;
    static const QString mask = QStringLiteral("[*]");
    for (const QString& kw : m_maskedKeywords) {
        result.replace(kw, mask, Qt::CaseInsensitive);
    }
    return result;
}

void LogManager::appendEntry(const QString& msg, LogLevel level, LogColor color) {
    LogEntry e;
    e.timestamp = QDateTime::currentDateTime().toString("HH:mm:ss.zzz");
    e.message   = maskSensitive(msg);
    e.level     = level;
    e.color     = color;

    {
        QMutexLocker lock(&m_mutex);
        m_entries.append(e);
        if (m_entries.size() > MAX_ENTRIES)
            m_entries.removeFirst();
        persistEntry(e);
    }

    emit logMessage(formatHtml(e), static_cast<int>(level));
}

void LogManager::persistEntry(const LogEntry& entry) {
    // Called with m_mutex held. Store already-masked normal/error messages;
    // packet-by-packet detail stays in the in-memory verbose view.
    if (m_logPath.isEmpty() || entry.level != LOG_LEVEL_NORMAL) return;
    QFile file(m_logPath);
    if (file.size() >= 512 * 1024) {
        QFile::remove(m_logPath + ".2");
        if (QFile::exists(m_logPath + ".1") &&
            !QFile::rename(m_logPath + ".1", m_logPath + ".2"))
            return;
        if (!QFile::rename(m_logPath, m_logPath + ".1")) return;
    }
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append)) return;
    const QString line = QString("[%1] %2\n")
        .arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs))
        .arg(entry.message);
    file.write(line.toUtf8());
}

QString LogManager::formatHtml(const LogEntry& e) {
    QString tsColor, msgColor;
    switch (e.color) {
    case LOG_COLOR_INFO:
        tsColor  = "#89b4fa";
        msgColor = "#a6adc8";
        break;
    case LOG_COLOR_ERROR:
        tsColor  = "#f38ba8";
        msgColor = "#f38ba8";
        break;
    case LOG_COLOR_DEBUG:
        tsColor  = "#6c7086";
        msgColor = "#7f849c";
        break;
    }

    return QString("<span style='color:%1'>[%2]</span> "
                   "<span style='color:%3'>%4</span>")
        .arg(tsColor).arg(e.timestamp).arg(msgColor).arg(e.message.toHtmlEscaped());
}

} // namespace VLan

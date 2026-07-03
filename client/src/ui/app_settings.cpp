#include "app_settings.h"
#include "protocol.h"
#include <QScopedPointer>
#include <QSettings>

namespace VLan {
namespace {

QSettings* createSettings()
{
    return new QSettings(QStringLiteral("VLan"), QStringLiteral("VLan"));
}

} // namespace

AppLanguage AppSettings::language()
{
    QScopedPointer<QSettings> s(createSettings());
    return UiStrings::languageFromCode(
        s->value(QStringLiteral("ui/language"), QStringLiteral("en")).toString());
}

void AppSettings::setLanguage(AppLanguage language)
{
    QScopedPointer<QSettings> s(createSettings());
    s->setValue(QStringLiteral("ui/language"), UiStrings::languageCode(language));
}

QString AppSettings::defaultServerHost()
{
    QScopedPointer<QSettings> s(createSettings());
    return s->value(QStringLiteral("connection/serverHost"), QString()).toString();
}

void AppSettings::setDefaultServerHost(const QString& host)
{
    QScopedPointer<QSettings> s(createSettings());
    s->setValue(QStringLiteral("connection/serverHost"), host.trimmed());
}

quint16 AppSettings::defaultServerPort()
{
    QScopedPointer<QSettings> s(createSettings());
    bool ok = false;
    int port = s->value(QStringLiteral("connection/serverPort"), DEFAULT_PORT).toInt(&ok);
    if (!ok || port <= 0 || port > 65535)
        return DEFAULT_PORT;
    return static_cast<quint16>(port);
}

void AppSettings::setDefaultServerPort(quint16 port)
{
    QScopedPointer<QSettings> s(createSettings());
    s->setValue(QStringLiteral("connection/serverPort"), static_cast<int>(port == 0 ? DEFAULT_PORT : port));
}

QString AppSettings::defaultPlayerName()
{
    QScopedPointer<QSettings> s(createSettings());
    return s->value(QStringLiteral("connection/playerName"), QString()).toString();
}

void AppSettings::setDefaultPlayerName(const QString& name)
{
    QScopedPointer<QSettings> s(createSettings());
    s->setValue(QStringLiteral("connection/playerName"), name.trimmed());
}

bool AppSettings::verboseLogsDefault()
{
    QScopedPointer<QSettings> s(createSettings());
    return s->value(QStringLiteral("logs/verboseDefault"), false).toBool();
}

void AppSettings::setVerboseLogsDefault(bool enabled)
{
    QScopedPointer<QSettings> s(createSettings());
    s->setValue(QStringLiteral("logs/verboseDefault"), enabled);
}

} // namespace VLan

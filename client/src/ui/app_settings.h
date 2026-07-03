#ifndef VLAN_APP_SETTINGS_H
#define VLAN_APP_SETTINGS_H

#include "ui_strings.h"
#include <QString>
#include <QtGlobal>

namespace VLan {

class AppSettings {
public:
    static AppLanguage language();
    static void setLanguage(AppLanguage language);

    static QString defaultServerHost();
    static void setDefaultServerHost(const QString& host);

    static quint16 defaultServerPort();
    static void setDefaultServerPort(quint16 port);

    static QString defaultPlayerName();
    static void setDefaultPlayerName(const QString& name);

    static bool verboseLogsDefault();
    static void setVerboseLogsDefault(bool enabled);
};

} // namespace VLan

#endif // VLAN_APP_SETTINGS_H

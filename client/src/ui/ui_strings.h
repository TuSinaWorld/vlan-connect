#ifndef VLAN_UI_STRINGS_H
#define VLAN_UI_STRINGS_H

#include <QString>

namespace VLan {

enum class AppLanguage {
    English = 0,
    Chinese = 1
};

class UiStrings {
public:
    static void setLanguage(AppLanguage language);
    static AppLanguage language();
    static QString text(const char* key);
    static QString languageCode(AppLanguage language);
    static AppLanguage languageFromCode(const QString& code);
};

} // namespace VLan

#endif // VLAN_UI_STRINGS_H

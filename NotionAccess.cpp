#include "NotionAccess.h"
#include "AppSettings.h"
#include "SecretStore.h"

#include <QSettings>

namespace {

constexpr char kSecretName[] = "notion-token";
constexpr char kParentIdKey[] = "notion/lastParentId";
constexpr char kParentTitleKey[] = "notion/lastParentTitle";

}

QString NotionAccess::token() {
    return SecretStore::read(QLatin1String(kSecretName));
}

bool NotionAccess::saveToken(const QString &token, QString *error) {
    return SecretStore::write(QLatin1String(kSecretName), token, error);
}

void NotionAccess::clear() {
    SecretStore::remove(QLatin1String(kSecretName));
    QSettings file(AppSettings::filePath(), QSettings::IniFormat);
    file.remove(QLatin1String(kParentIdKey));
    file.remove(QLatin1String(kParentTitleKey));
    file.sync();
}

NotionAccess::Parent NotionAccess::lastParent() {
    const QSettings file(AppSettings::filePath(), QSettings::IniFormat);
    return {file.value(QLatin1String(kParentIdKey)).toString(), file.value(QLatin1String(kParentTitleKey)).toString()};
}

void NotionAccess::saveLastParent(const Parent &parent) {
    QSettings file(AppSettings::filePath(), QSettings::IniFormat);
    file.setValue(QLatin1String(kParentIdKey), parent.id);
    file.setValue(QLatin1String(kParentTitleKey), parent.title);
    file.sync();
}

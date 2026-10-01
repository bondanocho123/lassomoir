#include "AgentAccess.h"
#include "AppSettings.h"
#include "SecretStore.h"

#include <QSettings>

namespace {

// Tidak ada = login Claude Code
constexpr char kMethodKey[] = "agent/access";
constexpr char kApiKeyMethod[] = "apiKey";
constexpr char kSecretName[] = "anthropic-api-key";

}

AgentAccess::Settings AgentAccess::load() {
    Settings settings;
    const QSettings file(AppSettings::filePath(), QSettings::IniFormat);
    if (file.value(QLatin1String(kMethodKey)).toString() == QLatin1String(kApiKeyMethod)) {
        settings.method = Method::ApiKey;
        settings.apiKey = SecretStore::read(QLatin1String(kSecretName));
    }
    return settings;
}

bool AgentAccess::save(const Settings &settings, QString *error) {
    QSettings file(AppSettings::filePath(), QSettings::IniFormat);
    if (settings.method == Method::Login) {
        file.remove(QLatin1String(kMethodKey));
        file.sync();
        SecretStore::remove(QLatin1String(kSecretName));
        return true;
    }

    // Key dulu, baru metodenya: bila key gagal disimpan, pilihan lama tetap berlaku
    if (!SecretStore::write(QLatin1String(kSecretName), settings.apiKey, error)) {
        return false;
    }
    file.setValue(QLatin1String(kMethodKey), QLatin1String(kApiKeyMethod));
    file.sync();
    return true;
}

QString AgentAccess::maskedKey(const QString &apiKey) {
    // Key yang terlalu pendek tidak ditampilkan sama sekali: empat karakter sudah sebagian besarnya
    return apiKey.size() > 8 ? QStringLiteral("…") + apiKey.right(4) : QStringLiteral("…");
}

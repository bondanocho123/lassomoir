#include "AppFonts.h"

#include <QDir>
#include <QDirIterator>
#include <QFontDatabase>
#include <QSettings>
#include <QStandardPaths>
#include <QtDebug>

#include <algorithm>

namespace {

constexpr char kFontKey[] = "ui/fontFamily";

// File INI di folder konfigurasi aplikasi (bukan registry), jadi test mode QStandardPaths ikut terpisah
QString settingsPath() {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(dir);
    return QDir(dir).filePath(QStringLiteral("settings.ini"));
}

}

QStringList AppFonts::registerBundled() {
    static QStringList families;
    static bool registered = false;
    if (registered) {
        return families;
    }
    registered = true;

    QDirIterator it(QStringLiteral(":/fonts"), {QStringLiteral("*.ttf")}, QDir::Files);
    while (it.hasNext()) {
        const QString path = it.next();
        const int id = QFontDatabase::addApplicationFont(path);
        if (id < 0) {
            qWarning() << "Font gagal dimuat:" << path;
            continue;
        }
        for (const QString &family : QFontDatabase::applicationFontFamilies(id)) {
            if (!families.contains(family)) {
                families.append(family);
            }
        }
    }
    std::sort(families.begin(), families.end(), [](const QString &a, const QString &b) {
        return a.compare(b, Qt::CaseInsensitive) < 0;
    });
    return families;
}

QString AppFonts::defaultLabel() {
    return QStringLiteral("Bawaan sistem (Garamond)");
}

QString AppFonts::saved() {
    const QSettings settings(settingsPath(), QSettings::IniFormat);
    return settings.value(QLatin1String(kFontKey)).toString();
}

void AppFonts::save(const QString &family) {
    QSettings settings(settingsPath(), QSettings::IniFormat);
    if (family.isEmpty()) {
        settings.remove(QLatin1String(kFontKey));
    } else {
        settings.setValue(QLatin1String(kFontKey), family);
    }
    settings.sync();
}

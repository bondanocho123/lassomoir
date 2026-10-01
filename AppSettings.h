#ifndef APPSETTINGS_H
#define APPSETTINGS_H

#pragma once

#include <QDir>
#include <QStandardPaths>
#include <QString>

// Pengaturan aplikasi di <AppConfigLocation>/settings.ini. File INI, bukan registry, jadi test mode
// QStandardPaths ikut memisahkannya dari pengaturan asli.
namespace AppSettings {

inline QString filePath() {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(dir);
    return QDir(dir).filePath(QStringLiteral("settings.ini"));
}

}

#endif // APPSETTINGS_H

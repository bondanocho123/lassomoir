#ifndef GITSANDBOX_H
#define GITSANDBOX_H

#pragma once

#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

// Konfigurasi git terisolasi selama objek hidup, juga untuk git yang dijalankan aplikasi (TaskGit):
// identitas tetap, tanpa signing, dan konfigurasi sistem/global mesin (core.autocrlf, gpgsign,
// credential helper) tidak ikut berpengaruh
class GitSandbox {
public:
    GitSandbox() {
        const QString config = m_home.filePath(QStringLiteral("gitconfig"));
        QFile file(config);
        if (file.open(QIODevice::WriteOnly)) {
            file.write("[user]\n\tname = Lassomoir Test\n\temail = test@lassomoir.local\n"
                       "[commit]\n\tgpgsign = false\n");
        }
        qputenv("GIT_CONFIG_GLOBAL", config.toUtf8());
        qputenv("GIT_CONFIG_NOSYSTEM", "1");
    }

    ~GitSandbox() {
        qunsetenv("GIT_CONFIG_GLOBAL");
        qunsetenv("GIT_CONFIG_NOSYSTEM");
    }

    GitSandbox(const GitSandbox &) = delete;
    GitSandbox &operator=(const GitSandbox &) = delete;

    // stdout git yang dipangkas; kosong bila git gagal
    static QString output(const QString &directory, const QStringList &arguments) {
        QProcess process;
        process.setWorkingDirectory(directory);
        process.start(QStandardPaths::findExecutable(QStringLiteral("git")), arguments);
        if (!process.waitForFinished(20000) || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
            return QString();
        }
        return QString::fromUtf8(process.readAllStandardOutput()).trimmed();
    }

private:
    QTemporaryDir m_home;
};

#endif // GITSANDBOX_H

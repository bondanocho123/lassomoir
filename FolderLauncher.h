#ifndef FOLDERLAUNCHER_H
#define FOLDERLAUNCHER_H

#pragma once

#include <QString>
#include <QStringList>

#include <functional>

// Membuka folder di luar aplikasi: pengelola file sistem (File Explorer) dan terminal
namespace FolderLauncher {

struct Command {
    QString program;
    QStringList arguments;

    bool operator==(const Command &other) const {
        return program == other.program && arguments == other.arguments;
    }
};

// Perintah yang membuka terminal baru di folder itu. Windows: Windows Terminal bila terpasang
// (memakai profil bawaan pengguna), selain itu PowerShell, lalu cmd.
Command terminalCommand(const QString &directory);

// Buka folder di File Explorer; false bila sistem menolak membukanya
bool showInExplorer(const QString &directory);

// Jalankan terminalCommand() dengan folder itu sebagai folder kerjanya; false bila gagal dijalankan
bool showInTerminal(const QString &directory);

// Hanya untuk test: pengganti peluncur proses terminal, supaya tidak ada jendela yang benar-benar
// terbuka. Kosong = kembali ke QProcess::startDetached. (File Explorer dibuka lewat
// QDesktopServices, yang bisa dicegat test dengan QDesktopServices::setUrlHandler.)
using ProcessStarter = std::function<bool(const Command &command, const QString &workingDirectory)>;
void setProcessStarter(ProcessStarter starter);

}

#endif // FOLDERLAUNCHER_H

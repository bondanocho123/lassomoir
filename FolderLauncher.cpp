#include "FolderLauncher.h"

#include <QDesktopServices>
#include <QDir>
#include <QProcess>
#include <QStandardPaths>
#include <QUrl>

namespace {

FolderLauncher::ProcessStarter g_starter;

}

FolderLauncher::Command FolderLauncher::terminalCommand(const QString &directory) {
    const QString native = QDir::toNativeSeparators(QDir::cleanPath(directory));
#if defined(Q_OS_WIN)
    const QString windowsTerminal = QStandardPaths::findExecutable(QStringLiteral("wt"));
    if (!windowsTerminal.isEmpty()) {
        return {windowsTerminal, {QStringLiteral("-d"), native}};
    }
    // Tanpa argumen folder: shell mulai di folder kerja prosesnya (lihat showInTerminal)
    const QString powershell = QStandardPaths::findExecutable(QStringLiteral("powershell"));
    if (!powershell.isEmpty()) {
        return {powershell, {}};
    }
    return {qEnvironmentVariable("COMSPEC", QStringLiteral("cmd.exe")), {}};
#elif defined(Q_OS_MACOS)
    return {QStringLiteral("open"), {QStringLiteral("-a"), QStringLiteral("Terminal"), native}};
#else
    Q_UNUSED(native);
    return {QStringLiteral("x-terminal-emulator"), {}};
#endif
}

bool FolderLauncher::showInExplorer(const QString &directory) {
    return QDesktopServices::openUrl(QUrl::fromLocalFile(QDir::cleanPath(directory)));
}

bool FolderLauncher::showInTerminal(const QString &directory) {
    const Command command = terminalCommand(directory);
    if (g_starter) {
        return g_starter(command, directory);
    }
    // startDetached memberi proses konsol jendela konsolnya sendiri, lepas dari aplikasi ini
    return QProcess::startDetached(command.program, command.arguments, directory);
}

void FolderLauncher::setProcessStarter(ProcessStarter starter) {
    g_starter = std::move(starter);
}

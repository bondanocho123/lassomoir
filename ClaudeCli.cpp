#include "ClaudeCli.h"

#include <QDir>
#include <QStandardPaths>

QString ClaudeCli::findExecutable() {
    // findExecutable memakai PATHEXT di Windows, jadi claude.exe ikut ketemu
    const QString onPath = QStandardPaths::findExecutable(QStringLiteral("claude"));
    if (!onPath.isEmpty()) {
        return onPath;
    }
    return QStandardPaths::findExecutable(QStringLiteral("claude"),
                                          {QDir::home().filePath(QStringLiteral(".local/bin"))});
}

QStringList ClaudeCli::arguments(const AgentDefinition &agent) {
    QStringList args = {
        QStringLiteral("-p"),
        QStringLiteral("--output-format"), QStringLiteral("stream-json"),
        // Tanpa --verbose, CLI menolak stream-json di mode -p
        QStringLiteral("--verbose"),
        QStringLiteral("--tools"), agent.tools.join(','),
    };
    if (!agent.allowedTools.isEmpty()) {
        args << QStringLiteral("--allowedTools") << agent.allowedTools.join(',');
    }

    // Edit file diterima otomatis; aksi lain yang butuh izin langsung ditolak
    // (tidak ada yang bisa menjawab prompt izin, jadi proses tidak boleh menunggu)
    args << QStringLiteral("--permission-mode") << QStringLiteral("acceptEdits")
         << QStringLiteral("--permission-prompts") << QStringLiteral("none")
         // Server MCP milik pengguna tidak ikut dimuat di run agent
         << QStringLiteral("--strict-mcp-config");

    if (!agent.effort.isEmpty()) {
        args << QStringLiteral("--effort") << agent.effort;
    }
    if (!agent.rolePrompt.isEmpty()) {
        args << QStringLiteral("--append-system-prompt") << agent.rolePrompt;
    }
    return args;
}

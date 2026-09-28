#include "ClaudeCli.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

namespace {

QJsonObject textBlock(const QString &text) {
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("text")}, {QStringLiteral("text"), text}};
}

// Media type yang diterima Claude untuk blok gambar; kosong bila bukan salah satunya
QString imageMediaType(const QString &path) {
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QLatin1String("png")) return QStringLiteral("image/png");
    if (suffix == QLatin1String("jpg") || suffix == QLatin1String("jpeg")) return QStringLiteral("image/jpeg");
    if (suffix == QLatin1String("gif")) return QStringLiteral("image/gif");
    if (suffix == QLatin1String("webp")) return QStringLiteral("image/webp");
    return QString();
}

}

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

    if (!agent.model.isEmpty()) {
        args << QStringLiteral("--model") << agent.model;
    }
    if (!agent.effort.isEmpty()) {
        args << QStringLiteral("--effort") << agent.effort;
    }
    if (!agent.rolePrompt.isEmpty()) {
        args << QStringLiteral("--append-system-prompt") << agent.rolePrompt;
    }
    return args;
}

QStringList ClaudeCli::arguments(const AgentLaunch &launch) {
    AgentDefinition agent = launch.agent;
    for (const QString &directory : launch.readableDirectories) {
        agent.allowedTools.append(readRule(directory));
    }
    QStringList args = arguments(agent);
    if (!launch.imagePaths.isEmpty()) {
        // Foto hanya bisa ikut sebagai blok gambar di pesan stream-json (lihat userMessage)
        args << QStringLiteral("--input-format") << QStringLiteral("stream-json");
    }
    return args;
}

QString ClaudeCli::readRule(const QString &directory) {
    // Pola aturan izin memakai path gaya POSIX: drive Windows "E:" menjadi "/e"
    QString path = QDir::cleanPath(QDir::fromNativeSeparators(QDir(directory).absolutePath()));
    if (path.size() >= 2 && path.at(1) == QLatin1Char(':')) {
        path = QLatin1Char('/') + path.at(0).toLower() + path.mid(2);
    }
    if (path.endsWith(QLatin1Char('/'))) {
        path.chop(1);   // akar drive: "/e/" -> "/e"
    }
    // "//" di depan = path absolut (satu "/" berarti relatif terhadap folder project)
    return QStringLiteral("Read(/%1/**)").arg(path);
}

QByteArray ClaudeCli::userMessage(const QString &prompt, const QStringList &imagePaths) {
    QJsonArray content;
    for (const QString &path : imagePaths) {
        const QString name = QFileInfo(path).fileName();
        const QString mediaType = imageMediaType(path);
        QFile file(path);
        if (mediaType.isEmpty() || !file.open(QIODevice::ReadOnly)) {
            content.append(textBlock(QStringLiteral("(Foto %1 tidak bisa dibaca)").arg(name)));
            continue;
        }
        // Label nama sebelum tiap gambar, supaya prompt bisa menyebut foto tertentu
        content.append(textBlock(QStringLiteral("Foto: %1").arg(name)));
        const QJsonObject source{
            {QStringLiteral("type"), QStringLiteral("base64")},
            {QStringLiteral("media_type"), mediaType},
            {QStringLiteral("data"), QString::fromLatin1(file.readAll().toBase64())},
        };
        content.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("image")}, {QStringLiteral("source"), source}});
    }
    content.append(textBlock(prompt));

    const QJsonObject message{{QStringLiteral("role"), QStringLiteral("user")}, {QStringLiteral("content"), content}};
    const QJsonObject line{{QStringLiteral("type"), QStringLiteral("user")}, {QStringLiteral("message"), message}};
    return QJsonDocument(line).toJson(QJsonDocument::Compact) + '\n';
}

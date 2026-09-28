#include "RunLogFormatter.h"

#include <QDir>

namespace {

constexpr int kMaxDetailLength = 100;

QString taskLabel(const TaskItem &task) {
    return QStringLiteral("%1/%2").arg(task.projectId, task.title);
}

QString agentSource(const TaskItem &task) {
    return QStringLiteral("[AGENT:%1]").arg(task.stage);
}

// Path absolut di dalam folder kerja dibuat relatif supaya baris log tetap pendek
QString displayDetail(const QString &detail, const QString &workingDirectory) {
    QString text = detail;
    text.replace(QLatin1Char('\n'), QLatin1Char(' '));

    if (!workingDirectory.isEmpty() && QDir::isAbsolutePath(text)) {
        const QString relative = QDir(workingDirectory).relativeFilePath(QDir::fromNativeSeparators(text));
        if (!relative.startsWith(QLatin1String(".."))) {
            text = relative;
        }
    }
    if (text.size() > kMaxDetailLength) {
        text = text.left(kMaxDetailLength - 1) + QChar(0x2026);   // …
    }
    return text;
}

}

QStringList RunLogFormatter::startLines(const TaskItem &task, const AgentLaunch &launch) {
    QStringList lines = {QStringLiteral("[RUN] %1 · %2 · %3")
                             .arg(taskLabel(task), task.stage, launch.workingDirectory)};

    // Bagian "# Task" dicetak utuh; bagian serah-terima (spesifikasi, hasil stage sebelumnya)
    // bisa ratusan baris, jadi cukup judul + jumlah barisnya
    QString heading;
    int headingLines = 0;
    auto flushHeading = [&]() {
        if (!heading.isEmpty()) {
            lines.append(QStringLiteral("      + %1 (%2 baris)").arg(heading).arg(headingLines));
        }
    };
    // Baris "# ..." di dalam blok kode (mis. judul dokumen Word lampiran) bukan judul bagian.
    // Blok ditutup pagar backtick yang sama panjang atau lebih, jadi ``` di dalam pagar ```` tidak menutupnya.
    qsizetype fenceLength = 0;   // > 0 selama di dalam blok kode
    const QStringList promptLines = launch.prompt.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &line : promptLines) {
        qsizetype ticks = 0;
        while (ticks < line.size() && line.at(ticks) == QLatin1Char('`')) {
            ++ticks;
        }
        if (fenceLength == 0 && ticks >= 3) {
            fenceLength = ticks;
        } else if (fenceLength > 0 && ticks >= fenceLength && line.trimmed().size() == ticks) {
            fenceLength = 0;
        }
        const bool isHeading = fenceLength == 0 && line.startsWith(QLatin1String("# "));
        if (isHeading && line != QLatin1String("# Task")) {
            flushHeading();
            heading = line.mid(2);
            headingLines = 0;
        } else if (!heading.isEmpty()) {
            ++headingLines;
        } else {
            lines.append(QStringLiteral("      %1").arg(line));
        }
    }
    flushHeading();
    return lines;
}

QString RunLogFormatter::gateLine(const TaskItem &task, const QString &text) {
    return QStringLiteral("[GATE] %1 %2").arg(taskLabel(task), text);
}

QString RunLogFormatter::gitLine(const TaskItem &task, const QString &text) {
    return QStringLiteral("[GIT] %1 %2").arg(taskLabel(task), text);
}

QString RunLogFormatter::toolLabel(const AgentEvent &event, const QString &workingDirectory) {
    return QStringLiteral("→ %1 %2")
        .arg(event.toolName, displayDetail(event.toolDetail, workingDirectory))
        .trimmed();
}

QString RunLogFormatter::queuedLine(const TaskItem &task) {
    return QStringLiteral("[RUN] %1 · %2 antre: menunggu slot kosong atau folder kerja yang sedang ditulis agent lain")
        .arg(taskLabel(task), task.stage);
}

QString RunLogFormatter::rejectedLine(const TaskItem &task, const QString &reason) {
    return QStringLiteral("[RUN] %1 ditolak: %2").arg(taskLabel(task), reason);
}

QString RunLogFormatter::warningLine(const TaskItem &task, const QString &text) {
    return QStringLiteral("[RUN] %1 peringatan: %2").arg(taskLabel(task), text);
}

QString RunLogFormatter::eventLine(const TaskItem &task, const AgentEvent &event, const QString &workingDirectory) {
    switch (event.kind) {
    case AgentEvent::Kind::Text:
        return QStringLiteral("%1 %2").arg(agentSource(task), event.text);
    case AgentEvent::Kind::ToolUse:
        return QStringLiteral("%1 %2").arg(agentSource(task), toolLabel(event, workingDirectory));
    case AgentEvent::Kind::Stderr:
        return QStringLiteral("%1 stderr: %2").arg(agentSource(task), event.text);
    case AgentEvent::Kind::Result:
        break;
    }
    return QString();
}

QString RunLogFormatter::finishLine(const TaskItem &task, const AgentResult &result) {
    if (!result.success) {
        QString line = QStringLiteral("%1 ✗ %2")
                           .arg(agentSource(task),
                                result.outcome.isEmpty() ? QStringLiteral("gagal") : result.outcome);
        if (!result.message.isEmpty()) {
            line += QStringLiteral(": ") + result.message;
        }
        return line;
    }

    QStringList parts = {
        QStringLiteral("✓ selesai"),
        formatDuration(result.durationMs),
        QStringLiteral("%1 tok").arg(formatTokens(result.totalTokens)),
        QStringLiteral("$%1").arg(result.costUsd, 0, 'f', 2),
    };
    if (!result.sessionId.isEmpty()) {
        // ID lengkap supaya bisa dipakai untuk `claude --resume <id>`
        parts.append(QStringLiteral("sesi %1").arg(result.sessionId));
    }
    if (!result.deniedTools.isEmpty()) {
        QStringList tools = result.deniedTools;
        tools.removeDuplicates();
        parts.append(QStringLiteral("%1 aksi ditolak (%2)")
                         .arg(result.deniedTools.size())
                         .arg(tools.join(QStringLiteral(", "))));
    }
    return QStringLiteral("%1 %2").arg(agentSource(task), parts.join(QStringLiteral(" · ")));
}

QString RunLogFormatter::formatDuration(qint64 ms) {
    if (ms < 60000) {
        return QStringLiteral("%1s").arg(ms / 1000.0, 0, 'f', 1);
    }
    const qint64 totalSeconds = ms / 1000;
    return QStringLiteral("%1m %2s").arg(totalSeconds / 60).arg(totalSeconds % 60);
}

QString RunLogFormatter::formatTokens(qint64 tokens) {
    // Satu desimal di bawah 10 (2.9k), bulat di atasnya (107k)
    auto scaled = [](double value, QLatin1String suffix) {
        return QString::number(value, 'f', value < 10 ? 1 : 0) + suffix;
    };
    if (tokens < 1000) {
        return QString::number(tokens);
    }
    if (tokens < 1000000) {
        return scaled(tokens / 1000.0, QLatin1String("k"));
    }
    return scaled(tokens / 1000000.0, QLatin1String("M"));
}

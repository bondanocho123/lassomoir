#ifndef RUNLOGFORMATTER_H
#define RUNLOGFORMATTER_H

#pragma once

#include "AgentTypes.h"
#include "TaskItem.h"

#include <QString>
#include <QStringList>

// Teks baris konsol untuk semua kejadian run agent.
namespace RunLogFormatter {

// "[RUN] TTT/Login · CODER · E:/repo", lalu bagian "# Task" dari prompt; bagian serah-terima
// hanya diringkas: "+ Spesifikasi yang disetujui (SPECIFIER) (142 baris)"
QStringList startLines(const TaskItem &task, const AgentLaunch &launch);

// "[GATE] TTT/Login <teks>" untuk keputusan review dan perpindahan yang ditolak gate
QString gateLine(const TaskItem &task, const QString &text);

// "[GIT] TTT/Login <teks>" untuk branch, worktree, commit, push, dan merge task
QString gitLine(const TaskItem &task, const QString &text);

// "→ Write src/a.cpp": nama tool + detail (path relatif terhadap folder kerja, maks. 100 karakter)
QString toolLabel(const AgentEvent &event, const QString &workingDirectory);

// "[RUN] TTT/Login · CODER antre: ..."
QString queuedLine(const TaskItem &task);

// "[RUN] TTT/Login ditolak: <alasan>"
QString rejectedLine(const TaskItem &task, const QString &reason);

// "[RUN] TTT/Login peringatan: <teks>", mis. lampiran atau folder referensi yang dilewati
QString warningLine(const TaskItem &task, const QString &text);

// "[AGENT:CODER] <teks>", "[AGENT:CODER] → Write src/a.cpp", "[AGENT:CODER] stderr: ...".
// Path di dalam folder kerja ditampilkan relatif.
QString eventLine(const TaskItem &task, const AgentEvent &event, const QString &workingDirectory);

// "[AGENT:CODER] ✓ selesai · 5.8s · 2.9k tok · $0.03 · sesi <id>" atau "[AGENT:CODER] ✗ timeout: ..."
QString finishLine(const TaskItem &task, const AgentResult &result);

// 5800 -> "5.8s", 72000 -> "1m 12s"
QString formatDuration(qint64 ms);

// 950 -> "950", 2910 -> "2.9k", 107000 -> "107k", 1200000 -> "1.2M"
QString formatTokens(qint64 tokens);

}

#endif // RUNLOGFORMATTER_H

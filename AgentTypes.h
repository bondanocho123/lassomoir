#ifndef AGENTTYPES_H
#define AGENTTYPES_H

#pragma once

#include "AgentDefinition.h"

#include <QString>
#include <QStringList>

// Status run satu task; dipakai kartu dan SwarmCoordinator
enum class RunState { Idle, Queued, Running };

// Ringkasan akhir satu run agent
struct AgentResult {
    bool success = false;        // event result: subtype "success" && !is_error
    QString outcome;             // "success", subtype error CLI, "cancelled", "timeout", "no_result", "failed_to_start"
    QString message;             // jawaban akhir agent, atau alasan gagal
    QString sessionId;           // untuk `claude --resume <id>`
    qint64 durationMs = 0;
    qint64 totalTokens = 0;      // input + cache_creation + cache_read + output
    double costUsd = 0.0;
    QStringList deniedTools;     // tool yang ditolak karena butuh izin

    static AgentResult failure(const QString &outcome, const QString &message) {
        AgentResult result;
        result.outcome = outcome;
        result.message = message;
        return result;
    }
};

// Satu kejadian selama run
struct AgentEvent {
    enum class Kind { Text, ToolUse, Stderr, Result };

    Kind kind = Kind::Text;
    QString text;                // Text: potongan jawaban; Stderr: satu baris
    QString toolName;            // ToolUse: "Read", "Bash", ...
    QString toolDetail;          // ToolUse: file_path / path / pattern / command, apa adanya
    AgentResult result;          // Result: hanya dipakai di dalam session
};

// Semua yang dibutuhkan runtime untuk menjalankan satu run
struct AgentLaunch {
    AgentDefinition agent;       // salinan definisi stage
    QString prompt;              // isi task, dikirim lewat stdin
    QString workingDirectory;    // folder kerja project = cwd proses agent
};

#endif // AGENTTYPES_H

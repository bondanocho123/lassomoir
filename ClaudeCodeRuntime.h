#ifndef CLAUDECODERUNTIME_H
#define CLAUDECODERUNTIME_H

#pragma once

#include "AgentRuntime.h"
#include "ClaudeCli.h"

#include <QByteArray>
#include <QProcess>
#include <QTimer>
#include <optional>

// Runtime yang menjalankan agent sebagai proses `claude -p` (Claude Code CLI)
class ClaudeCodeRuntime final : public AgentRuntime {
public:
    // Default: claude dicari sekali saat aplikasi mulai. Test bisa memberi exe palsu.
    explicit ClaudeCodeRuntime(QString program = ClaudeCli::findExecutable());

    bool isAvailable(QString *reason) const override;
    AgentSession *createSession(const AgentLaunch &launch, QObject *parent) override;

    // Terpasang -> `claude --version` >= ClaudeCli::minimumVersion() -> `claude auth status`
    RuntimeCheck check() const override;
    // failed_to_start -> Missing; pesan gagal bernada auth -> LoggedOut
    RuntimeCheck diagnose(const AgentResult &result) const override;

private:
    // check() / diagnose() tanpa URL panduan
    RuntimeCheck probe() const;
    RuntimeCheck classify(const AgentResult &result) const;

    QString m_program;
};

// Satu proses `claude -p`: prompt masuk lewat stdin, event dibaca per baris dari stdout (stream-json)
class ClaudeCodeSession final : public AgentSession {
    Q_OBJECT

public:
    ClaudeCodeSession(QString program, AgentLaunch launch, QObject *parent = nullptr);
    ~ClaudeCodeSession() override;

    void start() override;
    void cancel() override;
    bool isRunning() const override;

private:
    void onStarted();
    void onReadyReadStdout();
    void onReadyReadStderr();
    void onProcessFinished(int exitCode, QProcess::ExitStatus status);
    void onProcessError(QProcess::ProcessError error);
    void onTimeout();
    void handleLine(const QByteArray &line);
    void handleStderrLine(const QByteArray &line);
    void finish(const AgentResult &result);

    QString m_program;
    AgentLaunch m_launch;
    QProcess *m_process = nullptr;
    QTimer m_timeout;
    QByteArray m_stdoutBuffer;              // potongan baris stdout yang belum diakhiri '\n'
    QByteArray m_stderrBuffer;              // idem untuk stderr
    QString m_lastStderr;                   // alasan gagal bila tidak ada event result
    std::optional<AgentResult> m_result;    // dari event result
    bool m_cancelled = false;
    bool m_timedOut = false;
    bool m_done = false;                    // finished sudah dipancarkan
};

#endif // CLAUDECODERUNTIME_H

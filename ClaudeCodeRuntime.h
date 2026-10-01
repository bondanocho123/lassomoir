#ifndef CLAUDECODERUNTIME_H
#define CLAUDECODERUNTIME_H

#pragma once

#include "AgentAccess.h"
#include "AgentRuntime.h"
#include "ClaudeCli.h"

#include <QByteArray>
#include <QProcess>
#include <QTimer>
#include <functional>
#include <optional>

// Runtime yang menjalankan agent sebagai proses `claude -p` (Claude Code CLI)
class ClaudeCodeRuntime final : public AgentRuntime {
public:
    // Pilihan File > Integrations. Dibaca tiap kali dibutuhkan (juga dari thread pemeriksaan),
    // jadi perubahan langsung berlaku untuk run berikutnya.
    using AccessSource = std::function<AgentAccess::Settings()>;

    // Default: claude dicari sekali saat aplikasi mulai. Test bisa memberi exe dan akses palsu.
    explicit ClaudeCodeRuntime(QString program = ClaudeCli::findExecutable(),
                               AccessSource access = &AgentAccess::load);

    bool isAvailable(QString *reason) const override;
    AgentSession *createSession(const AgentLaunch &launch, QObject *parent) override;
    AccountLogin *createAccountLogin(QObject *parent) override;

    // Terpasang -> `claude --version` >= ClaudeCli::minimumVersion() -> API key terisi (metode
    // ApiKey) atau `claude auth status` (metode Login)
    RuntimeCheck check() const override;
    // failed_to_start -> Missing; API key kosong/ditolak -> BadApiKey; pesan gagal bernada auth ->
    // BadApiKey di metode ApiKey, selain itu LoggedOut
    RuntimeCheck diagnose(const AgentResult &result) const override;

private:
    // check() / diagnose() tanpa URL panduan
    RuntimeCheck probe() const;
    RuntimeCheck classify(const AgentResult &result) const;

    QString m_program;
    AccessSource m_access;
};

// Satu proses `claude -p`: prompt masuk lewat stdin, event dibaca per baris dari stdout (stream-json)
class ClaudeCodeSession final : public AgentSession {
    Q_OBJECT

public:
    // access: pilihan akses saat run dibuat; run yang sudah berjalan tidak ikut berubah
    ClaudeCodeSession(QString program, AgentLaunch launch, AgentAccess::Settings access,
                      QObject *parent = nullptr);
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
    AgentAccess::Settings m_access;
    QProcess *m_process = nullptr;
    QTimer m_timeout;
    QByteArray m_stdoutBuffer;              // potongan baris stdout yang belum diakhiri '\n'
    QByteArray m_stderrBuffer;              // idem untuk stderr
    QString m_lastStderr;                   // alasan gagal bila tidak ada event result
    std::optional<AgentResult> m_result;    // dari event result
    bool m_cancelled = false;
    bool m_timedOut = false;
    bool m_keyRejected = false;             // server menolak API key (401): proses dihentikan
    bool m_done = false;                    // finished sudah dipancarkan
};

#endif // CLAUDECODERUNTIME_H

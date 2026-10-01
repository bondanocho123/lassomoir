#ifndef CLAUDECODELOGIN_H
#define CLAUDECODELOGIN_H

#pragma once

#include "AgentRuntime.h"

#include <QProcess>

// Login Claude Code lewat CLI-nya: `claude auth status` untuk status, `claude auth login` untuk
// login OAuth (CLI membuka browser dan menyimpan tokennya sendiri). Environment proses tidak
// diubah, jadi yang dilaporkan adalah login CLI apa adanya, bukan API key milik aplikasi.
class ClaudeCodeLogin final : public AccountLogin {
    Q_OBJECT

public:
    explicit ClaudeCodeLogin(QString program, QObject *parent = nullptr);
    ~ClaudeCodeLogin() override;

    void refresh() override;
    void start() override;
    void cancel() override;
    bool isRunning() const override;

private:
    QProcess *spawn(const QStringList &arguments);
    void stop(QProcess *process);
    void onStatusFinished();
    void onLoginFinished(int exitCode, QProcess::ExitStatus status);

    QString m_program;
    QProcess *m_statusProcess = nullptr;    // `claude auth status` yang sedang ditunggu
    QProcess *m_loginProcess = nullptr;     // `claude auth login` yang sedang ditunggu
    bool m_cancelled = false;
};

#endif // CLAUDECODELOGIN_H

#include "ClaudeCodeLogin.h"
#include "ClaudeCli.h"

#include <QFileInfo>
#include <QTimer>

namespace {

constexpr int kStatusTimeoutMs = 10000;

}

ClaudeCodeLogin::ClaudeCodeLogin(QString program, QObject *parent)
    : AccountLogin(parent),
      m_program(std::move(program)) {
}

ClaudeCodeLogin::~ClaudeCodeLogin() {
    // Pemiliknya (dialog) ditutup: matikan proses tanpa memancarkan sinyal
    stop(m_statusProcess);
    stop(m_loginProcess);
}

void ClaudeCodeLogin::refresh() {
    // Jawaban yang masih ditunggu bisa berasal dari sebelum login: buang, tanya lagi
    stop(m_statusProcess);
    m_statusProcess = nullptr;

    if (m_program.isEmpty() || !QFileInfo::exists(m_program)) {
        AccountStatus status;
        status.state = AccountStatus::State::Unavailable;
        emit statusChanged(status);
        return;
    }

    m_statusProcess = spawn({QStringLiteral("auth"), QStringLiteral("status")});
    connect(m_statusProcess, &QProcess::finished, this, &ClaudeCodeLogin::onStatusFinished);
    connect(m_statusProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            onStatusFinished();   // tidak disusul finished()
        }
    });
    QTimer::singleShot(kStatusTimeoutMs, m_statusProcess, &QProcess::kill);
    m_statusProcess->start();
}

void ClaudeCodeLogin::start() {
    if (m_loginProcess) {
        return;
    }
    m_cancelled = false;
    m_loginProcess = spawn({QStringLiteral("auth"), QStringLiteral("login")});
    connect(m_loginProcess, &QProcess::finished, this, &ClaudeCodeLogin::onLoginFinished);
    connect(m_loginProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            onLoginFinished(-1, QProcess::CrashExit);
        }
    });
    m_loginProcess->start();
}

void ClaudeCodeLogin::cancel() {
    if (m_loginProcess) {
        m_cancelled = true;
        // kill(), bukan terminate(): di Windows terminate() tidak menghentikan aplikasi konsol
        m_loginProcess->kill();
    }
}

bool ClaudeCodeLogin::isRunning() const {
    return m_loginProcess != nullptr;
}

QProcess *ClaudeCodeLogin::spawn(const QStringList &arguments) {
    auto *process = new QProcess(this);
    process->setProgram(m_program);
    process->setArguments(arguments);
    // Tidak ada yang mengetik ke proses ini; stdin kosong supaya ia tidak menunggu masukan
    process->setStandardInputFile(QProcess::nullDevice());
    return process;
}

void ClaudeCodeLogin::stop(QProcess *process) {
    if (!process) {
        return;
    }
    process->disconnect(this);
    if (process->state() != QProcess::NotRunning) {
        process->kill();
        process->waitForFinished(2000);
    }
    process->deleteLater();
}

void ClaudeCodeLogin::onStatusFinished() {
    QProcess *process = m_statusProcess;
    m_statusProcess = nullptr;
    process->deleteLater();
    // Exit code tidak dipakai: `claude auth status` keluar 1 saat belum login, JSON-nya tetap ada
    emit statusChanged(ClaudeCli::parseAccountStatus(process->readAllStandardOutput()));
}

void ClaudeCodeLogin::onLoginFinished(int exitCode, QProcess::ExitStatus status) {
    QProcess *process = m_loginProcess;
    m_loginProcess = nullptr;
    process->deleteLater();

    const bool success = !m_cancelled && status == QProcess::NormalExit && exitCode == 0;
    QString message;
    if (!success && !m_cancelled) {
        // CLI menjelaskan sebabnya di stderr, mis. "Login failed: ..."
        message = QString::fromUtf8(process->readAllStandardError()).simplified();
        if (message.isEmpty()) {
            message = QStringLiteral("claude auth login gagal (exit code %1).").arg(exitCode);
        }
    }
    emit finished(success, message);
    refresh();
}

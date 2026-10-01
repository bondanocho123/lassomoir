#include "ClaudeCodeRuntime.h"
#include "StreamJsonParser.h"

#include <QFileInfo>

namespace {

constexpr int kCheckTimeoutMs = 10000;

struct CommandOutput {
    bool finished = false;   // berjalan dan keluar sendiri sebelum batas waktu
    int exitCode = -1;
    QByteArray stdoutData;
};

// Satu perintah singkat (`claude --version`, `claude auth status`) secara sinkron
CommandOutput runCommand(const QString &program, const QStringList &arguments) {
    QProcess process;
    process.start(program, arguments);
    CommandOutput output;
    if (!process.waitForStarted(kCheckTimeoutMs)) {
        return output;
    }
    process.closeWriteChannel();   // tidak ada input; jangan biarkan proses menunggu stdin
    if (!process.waitForFinished(kCheckTimeoutMs)) {
        process.kill();
        process.waitForFinished(2000);
        return output;
    }
    output.finished = process.exitStatus() == QProcess::NormalExit;
    output.exitCode = process.exitCode();
    output.stdoutData = process.readAllStandardOutput();
    return output;
}

// Panduan yang sesuai dengan masalahnya
RuntimeCheck withHelp(RuntimeCheck check) {
    switch (check.status) {
    case RuntimeCheck::Status::Missing:
        check.helpUrl = ClaudeCli::installUrl();
        break;
    case RuntimeCheck::Status::Outdated:
        check.helpUrl = ClaudeCli::installUrl() + QStringLiteral("#update-claude-code");
        break;
    case RuntimeCheck::Status::LoggedOut:
        check.helpUrl = ClaudeCli::authUrl();
        break;
    case RuntimeCheck::Status::Ok:
        break;
    }
    return check;
}

}

ClaudeCodeRuntime::ClaudeCodeRuntime(QString program)
    : m_program(std::move(program)) {
}

RuntimeCheck ClaudeCodeRuntime::check() const {
    return withHelp(probe());
}

RuntimeCheck ClaudeCodeRuntime::diagnose(const AgentResult &result) const {
    return withHelp(classify(result));
}

RuntimeCheck ClaudeCodeRuntime::probe() const {
    using Status = RuntimeCheck::Status;
    QString reason;
    if (!isAvailable(&reason)) {
        return RuntimeCheck::of(Status::Missing, reason);
    }

    const CommandOutput versionOutput = runCommand(m_program, {QStringLiteral("--version")});
    const QVersionNumber version = ClaudeCli::parseVersion(versionOutput.stdoutData);
    const QString versionText = version.isNull() ? QString() : version.toString();
    if (!version.isNull() && version < ClaudeCli::minimumVersion()) {
        return RuntimeCheck::of(Status::Outdated,
                                QStringLiteral("Versi terpasang %1, butuh minimal %2.")
                                    .arg(versionText, ClaudeCli::minimumVersion().toString()),
                                versionText);
    }

    // Status login yang tidak terbaca (mis. versi tanpa subcommand auth) tidak dilaporkan sebagai
    // masalah; run yang gagal karena belum login tetap ditangkap diagnose()
    // Kredensial lewat environment (API key, Bedrock, Vertex, Foundry) tidak tercatat sebagai login
    // CLI; status auth-nya tidak dipakai supaya pengguna seperti itu tidak diminta login tiap kali buka
    static const char *const kEnvCredentials[] = {
        "ANTHROPIC_API_KEY", "ANTHROPIC_AUTH_TOKEN", "CLAUDE_CODE_USE_BEDROCK",
        "CLAUDE_CODE_USE_VERTEX", "CLAUDE_CODE_USE_FOUNDRY",
    };
    for (const char *name : kEnvCredentials) {
        if (!qEnvironmentVariableIsEmpty(name)) {
            return RuntimeCheck::of(Status::Ok, QString(), versionText);
        }
    }
    const CommandOutput authOutput = runCommand(m_program, {QStringLiteral("auth"), QStringLiteral("status")});
    if (ClaudeCli::parseLoggedIn(authOutput.stdoutData) == std::optional<bool>(false)) {
        return RuntimeCheck::of(Status::LoggedOut, QStringLiteral("Claude Code belum login."), versionText);
    }
    return RuntimeCheck::of(Status::Ok, QString(), versionText);
}

RuntimeCheck ClaudeCodeRuntime::classify(const AgentResult &result) const {
    using Status = RuntimeCheck::Status;
    if (result.success || result.outcome == QLatin1String("cancelled") || result.outcome == QLatin1String("timeout")) {
        return RuntimeCheck();
    }
    if (result.outcome == QLatin1String("failed_to_start")) {
        return RuntimeCheck::of(Status::Missing, result.message);
    }
    if (ClaudeCli::looksLikeAuthError(result.message)) {
        return RuntimeCheck::of(Status::LoggedOut, result.message);
    }
    return RuntimeCheck();
}

bool ClaudeCodeRuntime::isAvailable(QString *reason) const {
    if (m_program.isEmpty() || !QFileInfo::exists(m_program)) {
        if (reason) {
            *reason = QStringLiteral("claude tidak ditemukan di PATH maupun ~/.local/bin");
        }
        return false;
    }
    return true;
}

AgentSession *ClaudeCodeRuntime::createSession(const AgentLaunch &launch, QObject *parent) {
    return new ClaudeCodeSession(m_program, launch, parent);
}

ClaudeCodeSession::ClaudeCodeSession(QString program, AgentLaunch launch, QObject *parent)
    : AgentSession(parent),
      m_program(std::move(program)),
      m_launch(std::move(launch)) {
    m_timeout.setSingleShot(true);
    connect(&m_timeout, &QTimer::timeout, this, &ClaudeCodeSession::onTimeout);
}

ClaudeCodeSession::~ClaudeCodeSession() {
    // Pemilik session sedang dibongkar (mis. aplikasi ditutup): matikan proses tanpa memancarkan sinyal
    if (m_process && m_process->state() != QProcess::NotRunning) {
        m_process->disconnect(this);
        m_process->kill();
        m_process->waitForFinished(2000);
    }
}

void ClaudeCodeSession::start() {
    if (m_process || m_done) {
        return;   // start() hanya berlaku sekali
    }
    if (m_program.isEmpty()) {
        finish(AgentResult::failure(QStringLiteral("failed_to_start"),
                                    QStringLiteral("claude tidak ditemukan")));
        return;
    }

    m_process = new QProcess(this);
    m_process->setProgram(m_program);
    m_process->setArguments(ClaudeCli::arguments(m_launch));
    m_process->setWorkingDirectory(m_launch.workingDirectory);

    connect(m_process, &QProcess::started, this, &ClaudeCodeSession::onStarted);
    connect(m_process, &QProcess::readyReadStandardOutput, this, &ClaudeCodeSession::onReadyReadStdout);
    connect(m_process, &QProcess::readyReadStandardError, this, &ClaudeCodeSession::onReadyReadStderr);
    connect(m_process, &QProcess::finished, this, &ClaudeCodeSession::onProcessFinished);
    connect(m_process, &QProcess::errorOccurred, this, &ClaudeCodeSession::onProcessError);

    m_timeout.start(m_launch.agent.timeoutMs);
    m_process->start();
}

void ClaudeCodeSession::cancel() {
    if (m_done) {
        return;
    }
    m_cancelled = true;
    if (m_process && m_process->state() != QProcess::NotRunning) {
        // kill(), bukan terminate(): di Windows terminate() tidak menghentikan aplikasi konsol.
        // finished("cancelled") menyusul dari onProcessFinished().
        m_process->kill();
    } else {
        finish(AgentResult::failure(QStringLiteral("cancelled"), QStringLiteral("dibatalkan")));
    }
}

bool ClaudeCodeSession::isRunning() const {
    return m_process && !m_done;
}

void ClaudeCodeSession::onStarted() {
    // Prompt lewat stdin: aman dari masalah quoting dan batas panjang command line.
    // Task berfoto dikirim sebagai satu pesan stream-json supaya fotonya ikut sebagai blok gambar.
    m_process->write(m_launch.imagePaths.isEmpty() ? m_launch.prompt.toUtf8()
                                                   : ClaudeCli::userMessage(m_launch.prompt, m_launch.imagePaths));
    m_process->closeWriteChannel();
}

void ClaudeCodeSession::onReadyReadStdout() {
    m_stdoutBuffer += m_process->readAllStandardOutput();
    qsizetype newline;
    while ((newline = m_stdoutBuffer.indexOf('\n')) >= 0) {
        const QByteArray line = m_stdoutBuffer.left(newline);
        m_stdoutBuffer.remove(0, newline + 1);
        handleLine(line);
    }
}

void ClaudeCodeSession::onReadyReadStderr() {
    m_stderrBuffer += m_process->readAllStandardError();
    qsizetype newline;
    while ((newline = m_stderrBuffer.indexOf('\n')) >= 0) {
        const QByteArray line = m_stderrBuffer.left(newline);
        m_stderrBuffer.remove(0, newline + 1);
        handleStderrLine(line);
    }
}

void ClaudeCodeSession::onProcessFinished(int exitCode, QProcess::ExitStatus status) {
    Q_UNUSED(status);

    // Ambil sisa output; baris terakhir bisa saja tidak diakhiri '\n'
    onReadyReadStdout();
    onReadyReadStderr();
    if (!m_stdoutBuffer.isEmpty()) {
        handleLine(m_stdoutBuffer);
        m_stdoutBuffer.clear();
    }
    if (!m_stderrBuffer.isEmpty()) {
        handleStderrLine(m_stderrBuffer);
        m_stderrBuffer.clear();
    }

    if (m_cancelled) {
        finish(AgentResult::failure(QStringLiteral("cancelled"), QStringLiteral("dibatalkan")));
    } else if (m_timedOut) {
        const int timeoutMs = m_launch.agent.timeoutMs;
        const QString limit = timeoutMs >= 60000
                                  ? QStringLiteral("%1 menit").arg(timeoutMs / 60000)
                                  : QStringLiteral("%1 detik").arg(timeoutMs / 1000.0, 0, 'f', 1);
        finish(AgentResult::failure(QStringLiteral("timeout"), QStringLiteral("melewati batas %1").arg(limit)));
    } else if (m_result) {
        finish(*m_result);
    } else {
        // Tanpa event result, pesan stderr terakhir biasanya menjelaskan sebabnya (mis. belum login)
        const QString message = m_lastStderr.isEmpty()
                                    ? QStringLiteral("claude berhenti dengan exit code %1").arg(exitCode)
                                    : m_lastStderr;
        finish(AgentResult::failure(QStringLiteral("no_result"), message));
    }
}

void ClaudeCodeSession::onProcessError(QProcess::ProcessError error) {
    // Error lain (mis. Crashed setelah kill) selalu disusul finished(); hanya gagal start yang tidak
    if (error == QProcess::FailedToStart) {
        finish(AgentResult::failure(QStringLiteral("failed_to_start"),
                                    QStringLiteral("claude gagal dijalankan: %1").arg(m_process->errorString())));
    }
}

void ClaudeCodeSession::onTimeout() {
    if (m_done || !m_process) {
        return;
    }
    m_timedOut = true;
    m_process->kill();
}

void ClaudeCodeSession::handleLine(const QByteArray &line) {
    const QList<AgentEvent> events = StreamJsonParser::parseLine(line);
    for (const AgentEvent &event : events) {
        if (event.kind == AgentEvent::Kind::Result) {
            m_result = event.result;
        } else if (!m_done) {
            emit eventReceived(event);
        }
    }
}

void ClaudeCodeSession::handleStderrLine(const QByteArray &line) {
    const QString text = QString::fromUtf8(line).trimmed();
    if (text.isEmpty() || m_done) {
        return;
    }
    m_lastStderr = text;

    AgentEvent event;
    event.kind = AgentEvent::Kind::Stderr;
    event.text = text;
    emit eventReceived(event);
}

void ClaudeCodeSession::finish(const AgentResult &result) {
    if (m_done) {
        return;
    }
    m_done = true;
    m_timeout.stop();
    emit finished(result);
}

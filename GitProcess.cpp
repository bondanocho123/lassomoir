#include "GitProcess.h"

#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>

QString GitProcess::executable() {
    return QStandardPaths::findExecutable(QStringLiteral("git"));
}

GitOutput GitProcess::run(const QString &git, const QString &directory, const QStringList &arguments,
                          const QByteArray &input, int timeoutMs) {
    GitOutput output;
    QProcess process;
    process.setProgram(git);
    process.setArguments(arguments);
    process.setWorkingDirectory(directory);

    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    // Perintah baca tidak menulis ulang index, supaya tidak berebut index.lock
    // dengan git lain (agent atau pengguna) yang sedang jalan di folder yang sama
    environment.insert(QStringLiteral("GIT_OPTIONAL_LOCKS"), QStringLiteral("0"));
    // Pesan git berbahasa Inggris agar penyebab gagal bisa dikenali
    environment.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    // Tidak ada terminal yang bisa menjawab: push yang butuh kredensial gagal, bukan menunggu
    environment.insert(QStringLiteral("GIT_TERMINAL_PROMPT"), QStringLiteral("0"));
    process.setProcessEnvironment(environment);

    process.start(input.isEmpty() ? QIODevice::ReadOnly : QIODevice::ReadWrite);
    if (!process.waitForStarted(timeoutMs)) {
        output.err = QStringLiteral("git gagal dijalankan: %1").arg(process.errorString());
        return output;
    }
    if (!input.isEmpty()) {
        process.write(input);
        process.closeWriteChannel();
    }
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(2000);
        output.err = QStringLiteral("git tidak selesai dalam %1 detik").arg(timeoutMs / 1000);
        return output;
    }
    output.finished = process.exitStatus() == QProcess::NormalExit;
    output.exitCode = process.exitCode();
    output.out = process.readAllStandardOutput();
    output.err = QString::fromUtf8(process.readAllStandardError()).trimmed();
    return output;
}

QString GitProcess::describeFailure(const GitOutput &output) {
    if (output.err.contains(QLatin1String("not a git repository"))) {
        return QStringLiteral("Folder kerja bukan repository git, jadi perubahan kode tidak bisa dilacak. "
                              "Jalankan git init lalu buat commit awal di folder kerja supaya perubahan "
                              "dari agent bisa ditinjau di sini.");
    }
    if (!output.err.isEmpty()) {
        return output.err;
    }
    return QStringLiteral("git berhenti dengan exit code %1").arg(output.exitCode);
}

#ifndef GITPROCESS_H
#define GITPROCESS_H

#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

// Hasil satu proses git
struct GitOutput {
    bool finished = false;   // proses berjalan dan berhenti sendiri sebelum batas waktu
    int exitCode = -1;
    QByteArray out;
    QString err;             // stderr git, atau alasan proses tidak selesai

    bool ok() const { return finished && exitCode == 0; }
    // stdout tanpa spasi/baris baru di ujungnya, mis. hasil rev-parse
    QString text() const { return QString::fromUtf8(out).trimmed(); }
};

// Menjalankan git sebagai proses anak. Memblokir sampai git selesai: panggil di luar thread GUI.
namespace GitProcess {

constexpr int kDefaultTimeoutMs = 20000;

// Path git di PATH; kosong bila tidak ada
QString executable();

GitOutput run(const QString &git, const QString &directory, const QStringList &arguments,
              const QByteArray &input = QByteArray(), int timeoutMs = kDefaultTimeoutMs);

// Alasan gagal yang bisa dibaca pengguna: folder bukan repository, stderr git, atau exit code
QString describeFailure(const GitOutput &output);

}

#endif // GITPROCESS_H

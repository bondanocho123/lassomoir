#ifndef WORKSPACEDIFF_H
#define WORKSPACEDIFF_H

#pragma once

#include "CodeMetrics.h"

#include <QHash>
#include <QList>
#include <QString>

#include <optional>

// Satu baris tampilan diff. Nomor baris 0 = baris itu tidak ada di sisi tersebut.
struct DiffLine {
    enum class Kind { Hunk, Context, Added, Removed, Note };

    Kind kind = Kind::Context;
    QString text;          // tanpa penanda +/-/spasi; Hunk: baris "@@ ... @@" utuh
    int oldLine = 0;
    int newLine = 0;
};

// Perubahan satu file dibanding commit dasar
struct FileDiff {
    enum class Status { Modified, Added, Deleted, Renamed };

    Status status = Status::Modified;
    QString path;          // relatif terhadap folder kerja; path baru bila Renamed
    QString oldPath;       // hanya untuk Renamed
    bool untracked = false;  // file baru yang belum pernah di-git add
    QString note;          // alasan isi tidak ditampilkan (biner, terlalu besar, hanya mode, ...)
    int added = 0;
    int removed = 0;
    QList<DiffLine> lines;

    // Metrik kode di commit dasar dan di folder kerja; kosong bila sisi itu tidak ada
    // (file baru / dihapus) atau file bukan kode sumber yang bisa diukur
    std::optional<CodeMetrics> before;
    std::optional<CodeMetrics> after;
};

// Perubahan folder kerja (tracked + file baru) dibanding commit dasar: HEAD, atau titik cabang
// branch task sehingga putaran yang sudah di-commit ikut terlihat. Juga dipakai untuk perubahan
// antara dua commit (GitDiff::between), yang sisi "sesudah"-nya commit, bukan folder kerja.
struct WorkspaceDiff {
    QString error;             // kosong = berhasil dibaca
    QString baseCommit;        // hash pendek commit dasar; kosong bila repository belum punya commit
                               // (between: kosong = dibanding pohon kosong, yaitu commit pertama)
    QString targetCommit;      // hash pendek commit sisi sesudah; kosong = folder kerja
    QList<FileDiff> files;     // urut path
    int omittedUntracked = 0;  // file baru di luar batas daftar
    // C#: nama tipe yang dideklarasikan di project → kelas dasarnya (DIT, tipe konteks diagram kelas)
    QHash<QString, QString> csharpTypes;

    int added() const;
    int removed() const;

    // Maintainability Index file terukur, rata-rata tertimbang baris kode; kosong bila tidak ada
    std::optional<int> maintainabilityBefore() const;
    std::optional<int> maintainabilityAfter() const;

    static WorkspaceDiff failure(const QString &error);
};

namespace GitDiff {

// Unified diff keluaran `git diff` -> daftar file. Tidak menjalankan proses apa pun.
QList<FileDiff> parse(const QString &patch);

// Membaca perubahan lewat git (memblokir sampai git selesai): panggil di luar thread GUI.
// withMetrics = false melewati pengukuran kode (Maintainability, tipe C# seluruh project)
// bila yang ditampilkan hanya diff-nya. baseRevision kosong = dibanding HEAD.
WorkspaceDiff collect(const QString &workingDirectory, bool withMetrics = true,
                      const QString &baseRevision = QString());

// Perubahan dari revisi from ke revisi to (tanpa folder kerja dan tanpa metrik), dibatasi ke isi
// folder kerja seperti collect(). from kosong = dibanding pohon kosong (commit pertama repository).
// fromMergeBase = true membandingkan to dengan titik cabang from & to, seperti `git diff from...to`:
// hanya yang dikerjakan di to, bukan yang sudah berubah di from sesudah bercabang.
WorkspaceDiff between(const QString &workingDirectory, const QString &from, const QString &to,
                      bool fromMergeBase = false);

}

#endif // WORKSPACEDIFF_H

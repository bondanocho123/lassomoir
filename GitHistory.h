#ifndef GITHISTORY_H
#define GITHISTORY_H

#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>

// Keadaan HEAD folder kerja project, untuk tombol branch di header swimlane
struct GitHead {
    bool repository = false;   // folder kerja berada di dalam repository git
    QString branch;            // branch aktif; kosong bila detached HEAD
    QString commit;            // hash pendek HEAD; kosong bila branch-nya belum punya commit
};

// Satu branch lokal atau remote-tracking
struct GitBranch {
    QString ref;          // nama lengkap untuk perintah git: "refs/heads/main", "refs/remotes/origin/main"
    QString name;         // nama tampil: "main", "origin/main"
    bool remote = false;
    bool current = false;     // branch yang sedang aktif di folder kerja project
    QString commit;       // hash lengkap ujung branch
    QString subject;      // judul commit ujung branch
    QDateTime date;       // waktu commit ujung branch
    QString upstream;     // branch lokal: branch remote yang diikutinya ("origin/main")
    QString track;        // selisih dengan upstream: "ahead 1, behind 2", "gone", atau kosong (sama)
    QString worktree;     // branch lokal yang sedang di-checkout di sebuah worktree: path-nya
};

// Branch repository tempat folder kerja berada
struct GitBranchList {
    QString error;              // kosong = berhasil dibaca
    QString root;               // akar repository
    QString subdir;             // letak folder kerja di dalam repository ("" = akarnya)
    GitHead head;
    QList<GitBranch> branches;  // lokal (yang aktif dulu, lalu yang terbaru), baru remote (terbaru dulu)

    // nullptr bila tidak ada branch dengan nama tampil itu
    const GitBranch *find(const QString &name) const;
};

struct GitCommit {
    QString hash;
    QStringList parents;   // hash lengkap; kosong = commit pertama repository
    QString author;
    QString email;
    QDateTime date;        // waktu penulisan (author date)
    QStringList refs;      // branch & tag yang menunjuk commit ini: "main", "origin/main", "tag: v1"
    QString subject;
    QString body;          // pesan sesudah judul; kosong bila tidak ada

    QString shortHash() const { return hash.left(7); }
};

// Satu halaman riwayat commit, terbaru dulu
struct GitLog {
    QString error;
    QList<GitCommit> commits;
    bool hasMore = false;   // masih ada commit lebih lama di luar halaman ini
    int total = -1;         // jumlah seluruh commit revisi itu; -1 bila tidak dihitung (halaman lanjutan)
};

// Branch source dibanding branch target, seperti merge request source → target
struct GitComparison {
    QString error;
    QString mergeBase;         // hash lengkap titik cabang bersama; kosong bila tidak ada
    int ahead = 0;             // commit di source yang belum ada di target
    int behind = 0;            // commit di target yang belum ada di source
    QList<GitCommit> commits;  // halaman pertama commit "ahead", terbaru dulu
    bool hasMore = false;
};

// Riwayat git folder kerja project: branch, commit, dan perbandingan branch. Hanya membaca
// repository (tanpa checkout/commit). Bila folder kerja adalah subfolder repository, riwayatnya
// dibatasi ke commit yang mengubah subfolder itu, sama seperti tab "Perubahan kode" yang hanya
// menampilkan isi folder kerja. Fungsi yang menjalankan git memblokir: panggil di luar thread GUI.
namespace GitHistory {

constexpr int kPageSize = 200;

// Keluaran git -> data, tanpa menjalankan proses (format dari branches() dan log())
QList<GitBranch> parseBranches(const QByteArray &output);
QList<GitCommit> parseLog(const QByteArray &output);
// Dekorasi %D ("HEAD -> main, origin/main, tag: v1") -> ["main", "origin/main", "tag: v1"];
// HEAD dan penunjuk HEAD remote dibuang
QStringList parseRefs(const QString &decoration);

// "baru saja", "5 menit lalu", "kemarin", ..., lalu tanggal saja untuk yang lebih dari seminggu
QString relativeTime(const QDateTime &when, const QDateTime &now = QDateTime::currentDateTime());

GitHead head(const QString &workingDirectory);
GitBranchList branches(const QString &workingDirectory);
// revision: ref, hash, atau rentang "A..B"; skip = commit yang dilewati (halaman lanjutan)
GitLog log(const QString &workingDirectory, const QString &revision, int skip = 0, int limit = kPageSize);
// target & source: ref atau hash
GitComparison compare(const QString &workingDirectory, const QString &target, const QString &source,
                      int limit = kPageSize);

}

#endif // GITHISTORY_H

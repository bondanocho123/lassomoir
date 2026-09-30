#ifndef TASKGIT_H
#define TASKGIT_H

#pragma once

#include "TaskItem.h"

#include <QString>
#include <QStringList>

class StageCatalog;

// Alur git per task: branch + worktree sendiri, lalu serah-terima ke QA (commit + push) sebelum
// agent QA jalan. Menyetujui QA tidak menyentuh branch dasar (bisa diproteksi, mis. wajib PR);
// worktree-nya dibuang dan branch-nya dibiarkan siap di-PR manual. Fungsi yang menjalankan git
// memblokir: panggil lewat QtConcurrent, lalu terapkan Result-nya di thread GUI.
namespace TaskGit {

struct Result {
    QString error;          // kosong = berhasil
    bool skipped = false;   // folder kerja bukan repository / belum punya commit: task jalan tanpa branch
    QStringList log;        // yang sudah dikerjakan, untuk konsol
    QStringList warnings;   // gagal yang tidak menghentikan alur (mis. push)
    TaskBranch branch;      // keadaan branch task sesudah operasi
};

// Stage tempat kode diserahkan: commit + push sebelum agent-nya jalan
bool isHandoffStage(const QString &stageKey);

// "lassomoir/<id>-<judul>" dari huruf/angka ASCII, jadi selalu sah sebagai nama branch
QString branchName(const TaskItem &task);

// Ada .git di folder ini atau salah satu induknya (tanpa menjalankan git)
bool looksLikeRepository(const QString &directory);

// Task baru mendapat branch pada run pertamanya di stage yang agent-nya menulis folder kerja;
// task yang branch dasarnya dipilih saat dibuat (branch.base) sudah pada run agent pertamanya,
// supaya stage baca (SPECIFIER) pun membaca branch itu, bukan branch yang kebetulan aktif di
// folder kerja. Task yang sudah pernah ditulis langsung di folder kerja project tetap di sana.
bool startsBranch(const TaskItem &task, const StageCatalog &catalog);

// Pastikan task punya worktree di path: branch baru dari branch.base (atau branch yang aktif di
// projectDir), atau branch yang sudah ada dipasang lagi (mis. worktree-nya sempat dibuang)
Result ensureWorktree(const QString &projectDir, const QString &path, const TaskItem &task);

// Perbarui branch remote-tracking dari origin (fetch --prune), mis. supaya pilihan branch di form
// task ikut memuat branch yang baru di-push rekan kerja. Tanpa remote origin tidak ada yang diambil.
Result fetchOrigin(const QString &directory);

// Pull branch dari origin: fetch, lalu branch lokalnya dimajukan (fast-forward saja, tidak pernah
// merge/rebase). Branch yang sedang di-checkout di sebuah worktree ikut memperbarui isi foldernya;
// branch yang baru ada di origin dibuat di lokal. directory: folder mana pun di repository.
// Tanpa remote origin, atau branch-nya belum ada di origin, tidak ada yang perlu di-pull.
// error: fetch gagal, branch lokal & origin sudah bercabang, atau perubahan lokal bentrok.
Result pull(const QString &directory, const QString &branch);

// Persiapan git sebelum agent jalan, selalu diawali pull supaya agent bekerja di kode terbaru:
// - branched false (task bekerja di folder kerja project): pull branch yang aktif di sana;
// - task yang baru bercabang: pull branch dasarnya, lalu branch + worktree dibuat dari situ;
// - task yang sudah punya branch: worktree dipasang lagi bila perlu, lalu branch task di-pull.
// handoff (QA): sesudahnya commit + push. Berhenti di langkah pertama yang gagal.
Result prepareRun(const QString &projectDir, const QString &worktreePath, const TaskItem &task,
                  bool branched, bool handoff);

// Serah-terima ke QA: commit semua perubahan worktree (bila ada), lalu push branch ke origin
Result handoff(const TaskItem &task);

// Buang worktree task (dihapus, atau sudah disetujui QA); branch-nya dibiarkan di lokal & remote
// supaya bisa di-PR manual — main/branch dasar lain mungkin diproteksi, jadi aplikasi ini tidak
// pernah merge sendiri.
Result removeWorktree(const QString &projectDir, const TaskBranch &branch);

// Bersihkan catatan worktree yang foldernya sudah tidak ada (mis. project dihapus)
void prune(const QString &projectDir);

}

#endif // TASKGIT_H

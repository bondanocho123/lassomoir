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

// Task baru mendapat branch pada run pertamanya di stage yang agent-nya menulis folder kerja.
// Task yang sudah pernah ditulis langsung di folder kerja project tetap di sana.
bool startsBranch(const TaskItem &task, const StageCatalog &catalog);

// Pastikan task punya worktree di path: branch baru dari branch yang aktif di projectDir, atau
// branch yang sudah ada dipasang lagi (mis. worktree-nya sempat dibuang)
Result ensureWorktree(const QString &projectDir, const QString &path, const TaskItem &task);

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

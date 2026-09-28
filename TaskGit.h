#ifndef TASKGIT_H
#define TASKGIT_H

#pragma once

#include "TaskItem.h"

#include <QString>
#include <QStringList>

class StageCatalog;

// Alur git per task: branch + worktree sendiri, serah-terima ke QA (commit + push), lalu merge ke
// branch dasar saat QA disetujui. Fungsi yang menjalankan git memblokir: panggil lewat QtConcurrent,
// lalu terapkan Result-nya di thread GUI.
namespace TaskGit {

struct Result {
    QString error;          // kosong = berhasil
    bool skipped = false;   // folder kerja bukan repository / belum punya commit: task jalan tanpa branch
    QStringList log;        // yang sudah dikerjakan, untuk konsol
    QStringList warnings;   // gagal yang tidak menghentikan alur (mis. push)
    TaskBranch branch;      // keadaan branch task sesudah operasi
};

// Stage tempat kode diserahkan (commit + push sebelum agent-nya jalan) dan di-merge saat disetujui
bool isHandoffStage(const QString &stageKey);

// "lassomoir/<id>-<judul>" dari huruf/angka ASCII, jadi selalu sah sebagai nama branch
QString branchName(const TaskItem &task);

// Ada .git di folder ini atau salah satu induknya (tanpa menjalankan git)
bool looksLikeRepository(const QString &directory);

// Task baru mendapat branch pada run pertamanya di stage yang agent-nya menulis folder kerja.
// Task yang sudah pernah ditulis langsung di folder kerja project tetap di sana.
bool startsBranch(const TaskItem &task, const StageCatalog &catalog);

// Pastikan task punya worktree di path: branch baru dari branch yang aktif di projectDir, atau
// branch yang sudah ada dipasang lagi (worktree hilang, atau task dibuka lagi setelah merge)
Result ensureWorktree(const QString &projectDir, const QString &path, const TaskItem &task);

// Serah-terima ke QA: commit semua perubahan worktree (bila ada), lalu push branch ke origin
Result handoff(const TaskItem &task);

// Merge branch task ke base di projectDir, push base, lalu buang worktree dan branch lokalnya
Result merge(const QString &projectDir, const TaskItem &task);

// Buang worktree task yang dihapus; branch-nya dibiarkan
Result removeWorktree(const QString &projectDir, const TaskBranch &branch);

// Bersihkan catatan worktree yang foldernya sudah tidak ada (mis. project dihapus)
void prune(const QString &projectDir);

}

#endif // TASKGIT_H

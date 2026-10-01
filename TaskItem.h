#ifndef TASKITEM_H
#define TASKITEM_H

#pragma once

#include "AgentTypes.h"

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

// Status task di stage-nya (disimpan di session.json). Run yang sedang antre/berjalan
// adalah status runtime terpisah (RunState) dan tidak disimpan.
enum class TaskState { Idle, AwaitingReview, Failed };

inline QString taskStateKey(TaskState state) {
    switch (state) {
    case TaskState::AwaitingReview: return QStringLiteral("review");
    case TaskState::Failed: return QStringLiteral("failed");
    case TaskState::Idle: break;
    }
    return QStringLiteral("idle");
}

inline TaskState taskStateFromKey(const QString &key) {
    if (key == QLatin1String("review")) return TaskState::AwaitingReview;
    if (key == QLatin1String("failed")) return TaskState::Failed;
    return TaskState::Idle;
}

// Keputusan pengguna atas hasil run di stage ber-gate
namespace ReviewDecision {
inline const QString Approved = QStringLiteral("approved");
inline const QString Revise = QStringLiteral("revise");
inline const QString SentBack = QStringLiteral("sent_back");
}

// Satu run agent yang sudah selesai, beserta keputusan review-nya
struct StageRun {
    QString stage;          // stage saat run dijalankan
    QDateTime finishedAt;   // UTC
    AgentResult result;     // result.message = dokumen/ringkasan dari agent
    QString decision;       // kosong, atau salah satu ReviewDecision
    QString reviewNote;     // catatan pengguna saat keputusan diambil

    static StageRun finished(const QString &stage, const AgentResult &result) {
        StageRun run;
        run.stage = stage;
        run.finishedAt = QDateTime::currentDateTimeUtc();
        run.result = result;
        return run;
    }
};

// Branch git milik task (lihat TaskGit): dibuat pada run pertamanya di stage yang menulis kode.
// Kosong = task bekerja langsung di folder kerja project (bukan repository, atau task lama).
struct TaskBranch {
    QString name;          // "lassomoir/<id>-<judul>"
    QString base;          // tujuan merge: branch yang aktif di folder kerja saat branch dibuat
    QString baseCommit;    // titik cabang; pembanding tab "Perubahan kode"
    QString worktree;      // akar git worktree task; kosong setelah di-merge
    QString subdir;        // letak folder kerja project di dalam repository ("" = akarnya)
    QString mergedCommit;  // commit merge di base; kosong selama belum di-merge

    bool isEmpty() const { return name.isEmpty(); }
    bool hasWorktree() const { return !worktree.isEmpty(); }
    // Folder kerja agent: posisi yang sama dengan folder kerja project, tapi di dalam worktree
    QString directory() const { return subdir.isEmpty() ? worktree : worktree + QLatin1Char('/') + subdir; }

    bool operator==(const TaskBranch &other) const {
        return name == other.name && base == other.base && baseCommit == other.baseCommit
               && worktree == other.worktree && subdir == other.subdir && mergedCommit == other.mergedCommit;
    }
    bool operator!=(const TaskBranch &other) const { return !(*this == other); }
};

struct TaskItem {
    QString id;
    QString projectId; // "TTT", "spacewar"
    QString stage; // "WAITING", "CODER", "DONE", dll.
    QString category; // "component", "utility"
    QString title;
    QString subtext;    // catatan / prompt untuk agent, boleh multi-baris
    // Nama file lampiran (foto & dokumen) di folder lampiran task, lihat FileManager::attachmentDirectory
    QStringList attachments;
    TaskState state = TaskState::Idle;
    QList<StageRun> runs;   // kronologis, run terlama dulu

    // Model/effort pilihan pengguna per key stage; stage yang tidak ada di sini memakai
    // bawaan StageCatalog
    QHash<QString, AgentTuning> tuning;

    TaskBranch branch;

    // Run terakhir di stage tertentu; nullptr bila belum pernah ada
    const StageRun *latestRun(const QString &stageKey) const {
        for (auto it = runs.crbegin(); it != runs.crend(); ++it) {
            if (it->stage == stageKey) {
                return &*it;
            }
        }
        return nullptr;
    }

    // Jumlah stage yang run terakhirnya disetujui: angka pada badge "✓ N"
    int approvedGates() const {
        QSet<QString> stages;
        for (const StageRun &run : runs) {
            stages.insert(run.stage);
        }
        int count = 0;
        for (const QString &stageKey : std::as_const(stages)) {
            if (latestRun(stageKey)->decision == ReviewDecision::Approved) {
                ++count;
            }
        }
        return count;
    }

    // Stage yang pekerjaannya baru saja selesai, untuk penanda "✓ Selesai" di kartu:
    // "DONE" di ujung pipeline, atau stage run terakhir bila run itu berhasil dan tidak dikembalikan.
    // Kosong di WAITING, saat menunggu review / gagal (punya penanda sendiri), atau belum ada run.
    QString completedStage() const {
        if (stage == QLatin1String("DONE")) {
            return stage;
        }
        if (stage == QLatin1String("WAITING") || state != TaskState::Idle || runs.isEmpty()) {
            return QString();
        }
        const StageRun &last = runs.constLast();
        if (!last.result.success || last.decision == ReviewDecision::SentBack
            || last.decision == ReviewDecision::Revise) {
            return QString();
        }
        return last.stage;
    }

    // Berapa kali task dikembalikan dari gate ke stage sebelumnya: angka pada badge "↺ N"
    int sentBackCount() const {
        int count = 0;
        for (const StageRun &run : runs) {
            if (run.decision == ReviewDecision::SentBack) {
                ++count;
            }
        }
        return count;
    }
};

#endif // TASKITEM_H

#ifndef TASKITEM_H
#define TASKITEM_H

#pragma once

#include "AgentTypes.h"

#include <QDateTime>
#include <QList>
#include <QSet>
#include <QString>

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

struct TaskItem {
    QString id;
    QString projectId; // "TTT", "spacewar"
    QString stage; // "WAITING", "CODER", "DONE", dll.
    QString category; // "component", "utility"
    QString title;
    QString subtext;
    TaskState state = TaskState::Idle;
    QList<StageRun> runs;   // kronologis, run terlama dulu

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
};

#endif // TASKITEM_H

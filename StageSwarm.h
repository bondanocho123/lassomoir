#ifndef STAGESWARM_H
#define STAGESWARM_H

#pragma once

#include "AgentDefinition.h"
#include "AgentRuntime.h"
#include "AgentTypes.h"
#include "TaskItem.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>

class WorkspaceGuard;

// Gerombolan agent milik satu stage: paling banyak maxConcurrent run berjalan
// bersamaan, sisanya antre FIFO. Tiap run = satu AgentSession baru (konteks bersih).
class StageSwarm : public QObject {
    Q_OBJECT

public:
    StageSwarm(QString stageKey, AgentDefinition agent, AgentRuntime &runtime,
               WorkspaceGuard &guard, QObject *parent = nullptr);

    QString stageKey() const { return m_stageKey; }

    // Masukkan ke antrean lalu pump(); runQueued hanya bila run itu masih tertahan
    void submit(const TaskItem &task, const AgentLaunch &launch);

    // Antre -> dibuang + runFinished("cancelled"); berjalan -> session dihentikan.
    // false bila task tidak ada di gerombolan ini.
    bool cancel(const QString &taskId);

    // Pembatalan per project dibagi dua supaya SwarmCoordinator bisa membuang antrean
    // project itu di semua stage lebih dulu: run yang berhenti melepas folder, dan
    // antrean yang tersisa tidak boleh sempat jalan
    void cancelQueued(const QString &projectId);
    void cancelRunning(const QString &projectId);

    // Jalankan kepala antrean selama slot tersedia dan folder kerjanya boleh dipakai
    void pump();

    RunState state(const QString &taskId) const;
    int activeCount() const { return m_active.size(); }
    int queuedCount() const { return m_queue.size(); }

signals:
    void runQueued(const TaskItem &task);
    void runStarted(const TaskItem &task, const AgentLaunch &launch);
    void runEvent(const TaskItem &task, const AgentEvent &event);
    void runFinished(const TaskItem &task, const AgentResult &result);

private:
    struct Pending {
        TaskItem task;
        AgentLaunch launch;
    };
    struct Active {
        TaskItem task;
        AgentLaunch launch;
        AgentSession *session = nullptr;
    };

    void startRun(Pending pending);
    void onSessionFinished(const QString &taskId, const AgentResult &result);

    QString m_stageKey;
    AgentDefinition m_agent;             // maxConcurrent & writesWorkspace()
    AgentRuntime &m_runtime;             // pabrik session (Claude atau palsu untuk test)
    WorkspaceGuard &m_guard;             // milik SwarmCoordinator, dipakai bersama semua stage
    QList<Pending> m_queue;              // antrean FIFO
    QHash<QString, Active> m_active;     // taskId -> run yang sedang berjalan
};

#endif // STAGESWARM_H

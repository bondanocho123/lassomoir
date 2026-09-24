#include "StageSwarm.h"
#include "WorkspaceGuard.h"

#include <QStringList>

StageSwarm::StageSwarm(QString stageKey, AgentDefinition agent, AgentRuntime &runtime,
                       WorkspaceGuard &guard, QObject *parent)
    : QObject(parent),
      m_stageKey(std::move(stageKey)),
      m_agent(std::move(agent)),
      m_runtime(runtime),
      m_guard(guard) {
}

void StageSwarm::submit(const TaskItem &task, const AgentLaunch &launch) {
    m_queue.append({task, launch});
    pump();

    // Lapor antre hanya bila benar-benar tertahan (slot penuh atau folder sedang ditulis)
    if (state(task.id) == RunState::Queued) {
        emit runQueued(task);
    }
}

bool StageSwarm::cancel(const QString &taskId) {
    for (qsizetype i = 0; i < m_queue.size(); ++i) {
        if (m_queue.at(i).task.id == taskId) {
            const TaskItem task = m_queue.takeAt(i).task;
            emit runFinished(task, AgentResult::failure(QStringLiteral("cancelled"),
                                                        QStringLiteral("dibatalkan sebelum mulai")));
            return true;
        }
    }

    const auto it = m_active.constFind(taskId);
    if (it == m_active.constEnd()) {
        return false;
    }
    // finished("cancelled") menyusul lewat onSessionFinished(), bisa langsung di dalam cancel()
    AgentSession *session = it->session;
    session->cancel();
    return true;
}

void StageSwarm::cancelQueued(const QString &projectId) {
    QStringList taskIds;
    for (const Pending &pending : std::as_const(m_queue)) {
        if (pending.task.projectId == projectId) {
            taskIds.append(pending.task.id);
        }
    }
    for (const QString &taskId : std::as_const(taskIds)) {
        cancel(taskId);
    }
}

void StageSwarm::cancelRunning(const QString &projectId) {
    // Id dikumpulkan dulu: cancel() bisa langsung mengubah m_active
    QStringList taskIds;
    for (const Active &active : std::as_const(m_active)) {
        if (active.task.projectId == projectId) {
            taskIds.append(active.task.id);
        }
    }
    for (const QString &taskId : std::as_const(taskIds)) {
        cancel(taskId);
    }
}

void StageSwarm::pump() {
    while (m_active.size() < m_agent.maxConcurrent && !m_queue.isEmpty()) {
        // FIFO: kepala antrean menunggu bila folder kerjanya sedang dipakai penulis lain
        if (!m_guard.tryAcquire(m_queue.first().launch.workingDirectory, m_agent.writesWorkspace())) {
            return;
        }
        startRun(m_queue.takeFirst());
    }
}

RunState StageSwarm::state(const QString &taskId) const {
    if (m_active.contains(taskId)) {
        return RunState::Running;
    }
    for (const Pending &pending : m_queue) {
        if (pending.task.id == taskId) {
            return RunState::Queued;
        }
    }
    return RunState::Idle;
}

void StageSwarm::startRun(Pending pending) {
    AgentSession *session = m_runtime.createSession(pending.launch, this);
    const TaskItem task = pending.task;

    // Didaftarkan sebelum start(): session boleh selesai sinkron di dalam start()
    m_active.insert(task.id, {pending.task, pending.launch, session});

    connect(session, &AgentSession::eventReceived, this, [this, task](const AgentEvent &event) {
        emit runEvent(task, event);
    });
    connect(session, &AgentSession::finished, this, [this, taskId = task.id](const AgentResult &result) {
        onSessionFinished(taskId, result);
    });

    emit runStarted(task, pending.launch);
    session->start();
}

void StageSwarm::onSessionFinished(const QString &taskId, const AgentResult &result) {
    const Active active = m_active.take(taskId);
    if (!active.session) {
        return;
    }
    m_guard.release(active.launch.workingDirectory, m_agent.writesWorkspace());
    active.session->deleteLater();

    // SwarmCoordinator lalu memanggil pump() semua gerombolan: folder yang baru lepas
    // bisa membuka antrean stage lain
    emit runFinished(active.task, result);
}

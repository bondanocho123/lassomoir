#include "SwarmCoordinator.h"
#include "PromptComposer.h"
#include "StageCatalog.h"
#include "StageSwarm.h"

#include <QDir>

SwarmCoordinator::SwarmCoordinator(const StageCatalog &catalog, AgentRuntime &runtime,
                                   const PromptComposer &composer, QObject *parent)
    : QObject(parent),
      m_catalog(catalog),
      m_runtime(runtime),
      m_composer(composer) {
    const QList<const StageProfile *> agentStages = m_catalog.agentStages();
    for (const StageProfile *profile : agentStages) {
        auto *swarm = new StageSwarm(profile->key(), *profile->agent(), m_runtime, m_guard, this);

        connect(swarm, &StageSwarm::runQueued, this, &SwarmCoordinator::runQueued);
        connect(swarm, &StageSwarm::runStarted, this, &SwarmCoordinator::runStarted);
        connect(swarm, &StageSwarm::runEvent, this, &SwarmCoordinator::runEvent);
        connect(swarm, &StageSwarm::runFinished, this,
                [this](const TaskItem &task, const AgentResult &result) {
            emit runFinished(task, result);
            // Folder yang baru lepas bisa membuka antrean stage lain
            pumpAll();
        });

        m_swarms.append(swarm);
    }
}

bool SwarmCoordinator::run(const TaskItem &task, const QString &workingDirectory, QString *reason) {
    auto reject = [reason](const QString &why) {
        if (reason) {
            *reason = why;
        }
        return false;
    };

    const StageProfile *profile = m_catalog.profile(task.stage);
    StageSwarm *swarm = swarmFor(task.stage);
    if (!profile || !profile->agent() || !swarm) {
        return reject(QStringLiteral("stage %1 tidak menjalankan agent").arg(task.stage));
    }
    if (state(task.id) != RunState::Idle) {
        return reject(QStringLiteral("task ini sudah berjalan atau sedang antre"));
    }
    if (!m_runtime.isAvailable(reason)) {
        return false;
    }
    if (workingDirectory.isEmpty() || !QDir(workingDirectory).exists()) {
        return reject(QStringLiteral("folder kerja tidak ada: %1").arg(workingDirectory));
    }

    AgentLaunch launch;
    launch.agent = *profile->agent();
    launch.agent.applyTuning(task.tuning.value(task.stage));
    launch.prompt = m_composer.compose(task);
    launch.workingDirectory = workingDirectory;
    swarm->submit(task, launch);
    return true;
}

void SwarmCoordinator::cancel(const QString &taskId) {
    if (StageSwarm *swarm = swarmHolding(taskId)) {
        swarm->cancel(taskId);
    }
}

void SwarmCoordinator::cancelProject(const QString &projectId) {
    // Antrean dulu di semua stage: run yang berhenti akan melepas folder dan memicu pumpAll(),
    // dan saat itu tidak boleh ada task project ini yang masih menunggu giliran
    for (StageSwarm *swarm : std::as_const(m_swarms)) {
        swarm->cancelQueued(projectId);
    }
    for (StageSwarm *swarm : std::as_const(m_swarms)) {
        swarm->cancelRunning(projectId);
    }
}

RunState SwarmCoordinator::state(const QString &taskId) const {
    if (StageSwarm *swarm = swarmHolding(taskId)) {
        return swarm->state(taskId);
    }
    return RunState::Idle;
}

StageSwarm *SwarmCoordinator::swarmFor(const QString &stageKey) const {
    for (StageSwarm *swarm : m_swarms) {
        if (swarm->stageKey() == stageKey) {
            return swarm;
        }
    }
    return nullptr;
}

StageSwarm *SwarmCoordinator::swarmHolding(const QString &taskId) const {
    for (StageSwarm *swarm : m_swarms) {
        if (swarm->state(taskId) != RunState::Idle) {
            return swarm;
        }
    }
    return nullptr;
}

void SwarmCoordinator::pumpAll() {
    // Urutan pipeline: stage lebih awal mendapat giliran lebih dulu
    for (StageSwarm *swarm : std::as_const(m_swarms)) {
        swarm->pump();
    }
}

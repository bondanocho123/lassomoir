#include "StageCatalog.h"
#include "TaskManager.h"

namespace {

bool fail(QString *reason, const QString &why) {
    if (reason) {
        *reason = why;
    }
    return false;
}

}

TaskManager::TaskManager(const StageCatalog &catalog, QObject *parent)
    : QObject(parent), m_catalog(catalog) {
}

std::optional<TaskItem> TaskManager::task(const QString &taskId) const {
    const auto it = m_tasks.constFind(taskId);
    if (it == m_tasks.constEnd()) {
        return std::nullopt;
    }
    return *it;
}

QList<TaskItem> TaskManager::tasksForProject(const QString &projectId) const {
    QList<TaskItem> result;
    for (const TaskItem &task : m_tasks) {
        if (task.projectId == projectId) {
            result.append(task);
        }
    }
    return result;
}

void TaskManager::addTask(const TaskItem &item) {
    if (item.id.isEmpty()) {
        return;
    }

    m_tasks.insert(item.id, item);
    emit taskAdded(item);
}

bool TaskManager::updateDetails(const QString &taskId, const QString &title, const QString &category,
                                const QString &subtext, const QHash<QString, AgentTuning> &tuning) {
    auto it = m_tasks.find(taskId);
    if (it == m_tasks.end()) {
        return false;
    }
    it->title = title;
    it->category = category;
    it->subtext = subtext;
    it->tuning = tuning;
    emit taskChanged(*it);
    return true;
}

bool TaskManager::moveTask(const QString &taskId, const QString &toStage, QString *reason) {
    auto it = m_tasks.find(taskId);
    if (it == m_tasks.end()) {
        return fail(reason, QStringLiteral("task tidak ditemukan"));
    }
    TaskItem &task = *it;
    const QString fromStage = task.stage;
    if (fromStage == toStage) {
        return true;   // urutan di dalam kolom hanya urusan tampilan
    }

    QString why;
    if (!m_catalog.profile(toStage)) {
        why = QStringLiteral("stage %1 tidak dikenal").arg(toStage);
    } else if (task.state == TaskState::AwaitingReview) {
        why = QStringLiteral("task menunggu review di %1; putuskan dulu lewat drawer").arg(fromStage);
    } else if (stageIndex(toStage) > stageIndex(fromStage)) {
        // Gate hanya menahan perpindahan maju; mundur selalu boleh
        const StageProfile *profile = m_catalog.profile(fromStage);
        if (profile) {
            profile->exitPolicy().canLeave(task, &why);
        }
    }

    if (!why.isEmpty()) {
        emit taskMoveRejected(taskId, fromStage, why);
        return fail(reason, why);
    }

    moveTo(task, toStage);
    emit taskChanged(task);
    return true;
}

void TaskManager::removeProject(const QString &projectId) {
    for (auto it = m_tasks.begin(); it != m_tasks.end();) {
        if (it->projectId == projectId) {
            it = m_tasks.erase(it);
        } else {
            ++it;
        }
    }
}

void TaskManager::recordRun(const QString &taskId, const StageRun &run) {
    auto it = m_tasks.find(taskId);
    if (it == m_tasks.end()) {
        return;
    }
    TaskItem &task = *it;
    task.runs.append(run);

    // Run yang selesai saat task sudah dipindah tidak lagi menentukan status stage sekarang
    if (run.stage == task.stage) {
        const StageProfile *profile = m_catalog.profile(task.stage);
        if (run.result.outcome == QLatin1String("cancelled")) {
            task.state = TaskState::Idle;
        } else if (!run.result.success) {
            task.state = TaskState::Failed;
        } else if (profile && profile->exitPolicy().isGated()) {
            task.state = TaskState::AwaitingReview;
        } else {
            const QString next = nextStage(task.stage);
            if (!next.isEmpty()) {
                moveTo(task, next);
            } else {
                task.state = TaskState::Idle;
            }
        }
    }
    emit taskChanged(task);
}

bool TaskManager::approve(const QString &taskId, const QString &note, QString *reason) {
    auto it = m_tasks.find(taskId);
    if (it == m_tasks.end()) {
        return fail(reason, QStringLiteral("task tidak ditemukan"));
    }
    TaskItem &task = *it;
    StageRun *run = reviewedRun(task, reason);
    if (!run) {
        return false;
    }
    run->decision = ReviewDecision::Approved;
    run->reviewNote = note.trimmed();

    const QString next = nextStage(task.stage);
    if (next.isEmpty()) {
        task.state = TaskState::Idle;
    } else {
        moveTo(task, next);
    }
    emit taskChanged(task);
    return true;
}

bool TaskManager::requestRevision(const QString &taskId, const QString &note, QString *reason) {
    if (note.trimmed().isEmpty()) {
        return fail(reason, QStringLiteral("catatan revisi wajib diisi"));
    }
    auto it = m_tasks.find(taskId);
    if (it == m_tasks.end()) {
        return fail(reason, QStringLiteral("task tidak ditemukan"));
    }
    TaskItem &task = *it;
    StageRun *run = reviewedRun(task, reason);
    if (!run) {
        return false;
    }
    run->decision = ReviewDecision::Revise;
    run->reviewNote = note.trimmed();
    task.state = TaskState::Idle;
    emit taskChanged(task);
    return true;
}

bool TaskManager::sendBack(const QString &taskId, const QString &toStage, const QString &note, QString *reason) {
    if (note.trimmed().isEmpty()) {
        return fail(reason, QStringLiteral("catatan wajib diisi supaya agent tahu apa yang harus diperbaiki"));
    }
    auto it = m_tasks.find(taskId);
    if (it == m_tasks.end()) {
        return fail(reason, QStringLiteral("task tidak ditemukan"));
    }
    TaskItem &task = *it;
    if (!sendBackTargets(task.stage).contains(toStage)) {
        return fail(reason, QStringLiteral("%1 bukan stage agent sebelum %2").arg(toStage, task.stage));
    }
    StageRun *run = reviewedRun(task, reason);
    if (!run) {
        return false;
    }
    run->decision = ReviewDecision::SentBack;
    run->reviewNote = note.trimmed();
    moveTo(task, toStage);
    emit taskChanged(task);
    return true;
}

QString TaskManager::nextStage(const QString &stageKey) const {
    const QStringList keys = m_catalog.keys();
    const qsizetype index = keys.indexOf(stageKey);
    return index >= 0 && index + 1 < keys.size() ? keys.at(index + 1) : QString();
}

QStringList TaskManager::sendBackTargets(const QString &stageKey) const {
    QStringList targets;
    const QStringList keys = m_catalog.keys();
    const qsizetype index = keys.indexOf(stageKey);
    for (qsizetype i = 0; i < index; ++i) {
        const StageProfile *profile = m_catalog.profile(keys.at(i));
        if (profile && profile->agent()) {
            targets.append(keys.at(i));
        }
    }
    return targets;
}

int TaskManager::stageIndex(const QString &stageKey) const {
    return int(m_catalog.keys().indexOf(stageKey));
}

StageRun *TaskManager::reviewedRun(TaskItem &task, QString *reason) {
    if (task.state != TaskState::AwaitingReview) {
        fail(reason, QStringLiteral("task tidak sedang menunggu review"));
        return nullptr;
    }
    for (auto it = task.runs.rbegin(); it != task.runs.rend(); ++it) {
        if (it->stage == task.stage) {
            return &*it;
        }
    }
    fail(reason, QStringLiteral("belum ada hasil run di %1").arg(task.stage));
    return nullptr;
}

void TaskManager::moveTo(TaskItem &task, const QString &toStage) {
    const QString fromStage = task.stage;
    task.stage = toStage;
    task.state = TaskState::Idle;
    emit taskMoved(task, fromStage);
}

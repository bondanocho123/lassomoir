#include "TaskManager.h"

TaskManager::TaskManager(QObject *parent)
    : QObject(parent) {
}

void TaskManager::addTask(const TaskItem &item) {
    if (item.id.isEmpty()) {
        return;
    }

    m_tasks.insert(item.id, item);
    emit taskAdded(item);
}

void TaskManager::updateTaskStage(const QString &taskId, const QString &newStage, int targetIndex) {
    if (!m_tasks.contains(taskId)) {
        return;
    }

    m_tasks[taskId].stage = newStage;
    emit taskStageChanged(taskId, newStage, targetIndex);
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

TaskItem TaskManager::task(const QString &taskId) const {
    return m_tasks.value(taskId, TaskItem{});
}
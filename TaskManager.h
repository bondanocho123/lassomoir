#ifndef TASKMANAGER_H
#define TASKMANAGER_H

#pragma once

#include <QObject>
#include <QString>
#include <QMap>
#include <QList>
#include "TaskItem.h"

class TaskManager : public QObject {
    Q_OBJECT

public:
    explicit TaskManager(QObject *parent = nullptr);
    ~TaskManager() override = default;

    // Menambahkan item tugas baru ke dalam database/memori
    void addTask(const TaskItem &item);

    // Memperbarui stage/kolom dan urutan kartu saat dipindahkan
    void updateTaskStage(const QString &taskId, const QString &newStage, int targetIndex);

    // Mengambil daftar tugas berdasarkan ID proyek (misal "TTT" atau "spacewar")
    QList<TaskItem> tasksForProject(const QString &projectId) const;

    // Mengambil satu data spesifik task berdasarkan taskId
    TaskItem task(const QString &taskId) const;

signals:
    // Sinyal yang dipancarkan ketika status stage task berubah
    void taskStageChanged(const QString &taskId, const QString &newStage, int targetIndex);

    // Sinyal saat ada task baru yang ditambahkan
    void taskAdded(const TaskItem &item);

private:
    // Penyimpanan internal state kartu (Key: taskId)
    QMap<QString, TaskItem> m_tasks;
};

#endif // TASKMANAGER_H
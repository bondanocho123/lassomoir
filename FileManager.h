#ifndef FILEMANAGER_H
#define FILEMANAGER_H

#pragma once
#include "TaskItem.h"

#include <QString>
#include <QList>
#include <QMap>
#include <QSet>
#include <QJsonObject>
#include <QTimer>
#include <QByteArray>

class FileManager : public QObject {
    Q_OBJECT;

public:
    explicit FileManager(QObject *parent = nullptr);
    ~FileManager() override = default;

    QString projectFilePath(const QString &projectId);

    QList<TaskItem> loadTasks(const QString &projectId, QString *error);

    bool saveTasks(const QString &projectId, const QMap<QString, TaskItem> tasks, QString *error);

    void scheduleSave(const QString &projectId, QMap<QString, TaskItem> &tasks);

    void flushPendingSaves();

signals:
    void saveFailed(const QString &projectId, QString *reason);

    void loadFailed(const QString &projectId, QString *reason);

private:
    QString m_basePath;
    int m_schemaVersion;
    QTimer* m_saveTimer;
    QSet<QString> m_pendingProjects;
    QMap<QString, QMap<QString, TaskItem>> m_pendingSaves;
    QJsonObject taskToJson(TaskItem task);

    TaskItem taskFromJson(QJsonObject obj, QString *error = nullptr);

    bool writeAtomic(const QString &path, const QByteArray &data);
};

#endif // FILEMANAGER_H

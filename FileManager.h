#ifndef FILEMANAGER_H
#define FILEMANAGER_H

#pragma once
#include "TaskItem.h"

#include <QString>
#include <QStringList>
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

    // Daftar projectId = nama subfolder di <AppData>/projects yang punya session.json
    QStringList listProjectIds();

    QList<TaskItem> loadTasks(const QString &projectId, QString *error);

    bool saveTasks(const QString &projectId, const QList<TaskItem> &tasks, QString *error);

    void scheduleSave(const QString &projectId, const QList<TaskItem> &tasks);

    // Hapus permanen folder <AppData>/projects/<projectId> beserta session.json-nya.
    // Save yang masih tertunda untuk project itu ikut dibatalkan supaya folder
    // tidak ditulis ulang sesaat setelah dihapus.
    bool deleteProject(const QString &projectId, QString *error);

    // Tulis sekarang semua save yang masih menunggu debounce (dipakai timer dan saat aplikasi ditutup)
    void flushPendingSaves();

    // Folder tempat agent project ini bekerja; kosong bila belum dipilih.
    // Ikut tersimpan di session.json (field root "workingDirectory") pada save berikutnya,
    // jadi pemanggil setWorkingDirectory() perlu memanggil scheduleSave() sesudahnya.
    QString workingDirectory(const QString &projectId) const;
    void setWorkingDirectory(const QString &projectId, const QString &dir);

signals:
    void saveFailed(const QString &projectId, QString *reason);

    void loadFailed(const QString &projectId, QString *reason);

private:
    QString m_basePath;
    int m_schemaVersion;
    QTimer* m_saveTimer;
    QSet<QString> m_pendingProjects;
    QMap<QString, QList<TaskItem>> m_pendingSaves;
    QMap<QString, QString> m_workingDirs;   // projectId -> folder kerja agent
    QJsonObject taskToJson(TaskItem task);

    TaskItem taskFromJson(QJsonObject obj, QString *error = nullptr);

    bool writeAtomic(const QString &path, const QByteArray &data);
};

#endif // FILEMANAGER_H

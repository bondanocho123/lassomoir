#include "FileManager.h"


#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>
#include <QStandardPaths>

namespace {
constexpr int kSaveDebounceMs = 500;
}

FileManager::FileManager(QObject *parent) :
    QObject(parent),
    m_basePath(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)),
    m_schemaVersion(1),
    m_saveTimer(new QTimer(this))
{
    connect(m_saveTimer, &QTimer::timeout, this, [this]{
        for (auto it = m_pendingSaves.constBegin(); it != m_pendingSaves.constEnd(); ++it) {
            saveTasks(it.key(), it.value(), nullptr);
        }

        m_pendingSaves.clear();
        m_pendingProjects.clear();
    });
}

QString FileManager::projectFilePath(const QString &projectId) {
    return QStringLiteral("%1/projects/%2/session.json").arg(m_basePath, projectId);
}

QList<TaskItem> FileManager::loadTasks(const QString &projectId, QString *error){
    QList<TaskItem> result;

    QFile file(projectFilePath(projectId));

    if (!file.exists()){
        //set error menampilkan kesalahan : proyek belum pernah dibuat.
        return result;
    }

    if (!file.open(QIODevice::ReadOnly)){
        if (error) *error = QStringLiteral("Tidak bisa membuka file sesi:%1").arg(file.errorString());
        return result;
    }

    //set error parse json
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError){
        if (error) *error = QStringLiteral("File sesi rusak: %1").arg(parseError.errorString());
        return result;
    }

    const QJsonObject root = doc.object();
    const int fileSchemaVersion = root.value(QStringLiteral("schemaVersion")).toInt(-1);
    if (fileSchemaVersion != m_schemaVersion){
        if (error) {
            *error = QStringLiteral("schemaVersion %1 tidak dikenal (diharapkan %2)")
            .arg(fileSchemaVersion)
                .arg(m_schemaVersion);
        }
        return result;
    }

    const QJsonArray tasksArray = root.value(QStringLiteral("tasks")).toArray();
    result.reserve(tasksArray.size());
    for(const QJsonValue &value : tasksArray){
        QString itemError;
        const TaskItem item = taskFromJson(value.toObject(), &itemError);
        if (!itemError.isEmpty() && error){
            *error = itemError;
        }

        if (!item.id.isEmpty()){
            result.append(item);
        }
    }

    return result;

}

bool FileManager::saveTasks(const QString &projectId, const QMap<QString, TaskItem> tasks, QString *error){
    QJsonArray taskArray;
    for(const TaskItem &task : tasks){
        taskArray.append(taskToJson(task));
    }
}

QJsonObject FileManager::taskToJson(TaskItem task){
    QJsonObject obj;
    obj[QStringLiteral("id")] = task.id;
    obj[QStringLiteral("projectId")] = task.projectId;
    obj[QStringLiteral("stage")] = task.stage;
    obj[QStringLiteral("category")] = task.category;
    obj[QStringLiteral("title")] = task.title;
    obj[QStringLiteral("subtext")] = task.subtext;
    obj[QStringLiteral("badge")] = task.badge;
}

TaskItem FileManager::taskFromJson(QJsonObject obj, QString *error){
    TaskItem result;

    const QString id = obj.value(QStringLiteral("id")).toString();
    if (id.isEmpty()){
        if (error) *error = QStringLiteral("Task tanpa id diabaikan");
        return result;
    }

    result.id = obj.value(QStringLiteral("id")).toString();
    result.projectId = obj.value(QStringLiteral("projectId")).toString();
    result.stage = obj.value(QStringLiteral("stage")).toString();
    result.category = obj.value(QStringLiteral("category")).toString();
    result.title = obj.value(QStringLiteral("title")).toString();
    result.subtext = obj.value(QStringLiteral("subtext")).toString();
    result.badge = obj.value(QStringLiteral("badge")).toString();

    return result;
}

bool FileManager::writeAtomic(const QString &path, const QByteArray &data){
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)){
        return false;
    }

    file.write(data);
    return file.commit();
}
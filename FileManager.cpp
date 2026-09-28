#include "FileManager.h"


#include <QDebug>
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

QJsonObject runToJson(const StageRun &run) {
    QJsonObject obj;
    obj[QStringLiteral("stage")] = run.stage;
    obj[QStringLiteral("finishedAt")] = run.finishedAt.toString(Qt::ISODate);
    obj[QStringLiteral("success")] = run.result.success;
    obj[QStringLiteral("outcome")] = run.result.outcome;
    obj[QStringLiteral("message")] = run.result.message;
    obj[QStringLiteral("sessionId")] = run.result.sessionId;
    obj[QStringLiteral("durationMs")] = run.result.durationMs;
    obj[QStringLiteral("totalTokens")] = run.result.totalTokens;
    obj[QStringLiteral("costUsd")] = run.result.costUsd;
    obj[QStringLiteral("deniedTools")] = QJsonArray::fromStringList(run.result.deniedTools);
    obj[QStringLiteral("decision")] = run.decision;
    obj[QStringLiteral("reviewNote")] = run.reviewNote;
    return obj;
}

StageRun runFromJson(const QJsonObject &obj) {
    StageRun run;
    run.stage = obj.value(QStringLiteral("stage")).toString();
    run.finishedAt = QDateTime::fromString(obj.value(QStringLiteral("finishedAt")).toString(), Qt::ISODate);
    run.result.success = obj.value(QStringLiteral("success")).toBool();
    run.result.outcome = obj.value(QStringLiteral("outcome")).toString();
    run.result.message = obj.value(QStringLiteral("message")).toString();
    run.result.sessionId = obj.value(QStringLiteral("sessionId")).toString();
    run.result.durationMs = obj.value(QStringLiteral("durationMs")).toInteger();
    run.result.totalTokens = obj.value(QStringLiteral("totalTokens")).toInteger();
    run.result.costUsd = obj.value(QStringLiteral("costUsd")).toDouble();
    const QJsonArray denied = obj.value(QStringLiteral("deniedTools")).toArray();
    for (const QJsonValue &tool : denied) {
        run.result.deniedTools.append(tool.toString());
    }
    run.decision = obj.value(QStringLiteral("decision")).toString();
    run.reviewNote = obj.value(QStringLiteral("reviewNote")).toString();
    return run;
}

QJsonObject branchToJson(const TaskBranch &branch) {
    QJsonObject obj;
    obj[QStringLiteral("name")] = branch.name;
    obj[QStringLiteral("base")] = branch.base;
    obj[QStringLiteral("baseCommit")] = branch.baseCommit;
    obj[QStringLiteral("worktree")] = branch.worktree;
    obj[QStringLiteral("subdir")] = branch.subdir;
    obj[QStringLiteral("mergedCommit")] = branch.mergedCommit;
    return obj;
}

TaskBranch branchFromJson(const QJsonObject &obj) {
    TaskBranch branch;
    branch.name = obj.value(QStringLiteral("name")).toString();
    branch.base = obj.value(QStringLiteral("base")).toString();
    branch.baseCommit = obj.value(QStringLiteral("baseCommit")).toString();
    branch.worktree = obj.value(QStringLiteral("worktree")).toString();
    branch.subdir = obj.value(QStringLiteral("subdir")).toString();
    branch.mergedCommit = obj.value(QStringLiteral("mergedCommit")).toString();
    return branch;
}
}

FileManager::FileManager(QObject *parent) :
    QObject(parent),
    m_basePath(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)),
    m_schemaVersion(1),
    m_saveTimer(new QTimer(this))
{
    m_saveTimer->setSingleShot(true);
    connect(m_saveTimer, &QTimer::timeout, this, &FileManager::flushPendingSaves);
}

void FileManager::flushPendingSaves() {
    m_saveTimer->stop();
    for (auto it = m_pendingSaves.constBegin(); it != m_pendingSaves.constEnd(); ++it) {
        QString error;
        if (!saveTasks(it.key(), it.value(), &error)) {
            qWarning() << "[FileManager] Gagal menyimpan proyek" << it.key() << ":" << error;
        }
    }

    m_pendingSaves.clear();
    m_pendingProjects.clear();
}

QString FileManager::projectFilePath(const QString &projectId) {
    return QStringLiteral("%1/projects/%2/session.json").arg(m_basePath, projectId);
}

QStringList FileManager::listProjectIds() {
    QStringList ids;
    const QDir projectsDir(QStringLiteral("%1/projects").arg(m_basePath));
    const QStringList dirs = projectsDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &dirName : dirs) {
        if (QFile::exists(projectFilePath(dirName))) {
            ids.append(dirName);
        }
    }
    return ids;
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

    const QString workingDirectory = root.value(QStringLiteral("workingDirectory")).toString();
    if (!workingDirectory.isEmpty()) {
        m_workingDirs.insert(projectId, workingDirectory);
    }
    QStringList referenceDirs;
    const QJsonArray references = root.value(QStringLiteral("referenceDirectories")).toArray();
    for (const QJsonValue &reference : references) {
        if (!reference.toString().isEmpty()) {
            referenceDirs.append(reference.toString());
        }
    }
    if (!referenceDirs.isEmpty()) {
        m_referenceDirs.insert(projectId, referenceDirs);
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

bool FileManager::saveTasks(const QString &projectId, const QList<TaskItem> &tasks, QString *error){
    QJsonArray taskArray;
    for(const TaskItem &task : tasks){
        taskArray.append(taskToJson(task));
    }

    QJsonObject root;
    root[QStringLiteral("schemaVersion")] = m_schemaVersion;
    root[QStringLiteral("projectId")] = projectId;
    // Field tambahan: file lama tanpa field ini tetap terbaca, jadi schemaVersion tidak naik
    const QString workingDirectory = m_workingDirs.value(projectId);
    if (!workingDirectory.isEmpty()) {
        root[QStringLiteral("workingDirectory")] = workingDirectory;
    }
    const QStringList referenceDirs = m_referenceDirs.value(projectId);
    if (!referenceDirs.isEmpty()) {
        root[QStringLiteral("referenceDirectories")] = QJsonArray::fromStringList(referenceDirs);
    }
    root[QStringLiteral("tasks")] = taskArray;

    const QString path = projectFilePath(projectId);

    // Pastikan folder projects/<projectId>/ ada sebelum menulis
    const QString dirPath = QFileInfo(path).absolutePath();
    if (!QDir().mkpath(dirPath)){
        if (error) *error = QStringLiteral("Tidak bisa membuat folder sesi: %1").arg(dirPath);
        return false;
    }

    if (!writeAtomic(path, QJsonDocument(root).toJson(QJsonDocument::Indented))){
        if (error) *error = QStringLiteral("Gagal menulis file sesi: %1").arg(path);
        return false;
    }

    return true;
}

QJsonObject FileManager::taskToJson(TaskItem task){
    QJsonObject obj;
    obj[QStringLiteral("id")] = task.id;
    obj[QStringLiteral("projectId")] = task.projectId;
    obj[QStringLiteral("stage")] = task.stage;
    obj[QStringLiteral("category")] = task.category;
    obj[QStringLiteral("title")] = task.title;
    obj[QStringLiteral("subtext")] = task.subtext;
    if (!task.attachments.isEmpty()) {
        obj[QStringLiteral("attachments")] = QJsonArray::fromStringList(task.attachments);
    }
    obj[QStringLiteral("state")] = taskStateKey(task.state);

    // Riwayat run: dokumen hasil agent + keputusan review, dipakai untuk prompt stage berikutnya
    QJsonArray runs;
    for (const StageRun &run : task.runs) {
        runs.append(runToJson(run));
    }
    obj[QStringLiteral("runs")] = runs;

    // Hanya pilihan yang benar-benar menimpa bawaan stage yang ditulis
    QJsonObject tuning;
    for (auto it = task.tuning.cbegin(); it != task.tuning.cend(); ++it) {
        if (it->isEmpty()) {
            continue;
        }
        QJsonObject entry;
        entry[QStringLiteral("model")] = it->model;
        entry[QStringLiteral("effort")] = it->effort;
        tuning[it.key()] = entry;
    }
    if (!tuning.isEmpty()) {
        obj[QStringLiteral("tuning")] = tuning;
    }
    if (!task.branch.isEmpty()) {
        obj[QStringLiteral("branch")] = branchToJson(task.branch);
    }

    return obj;
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
    const QJsonArray attachments = obj.value(QStringLiteral("attachments")).toArray();
    for (const QJsonValue &attachment : attachments) {
        if (!attachment.toString().isEmpty()) {
            result.attachments.append(attachment.toString());
        }
    }
    // File lama tanpa field di bawah tetap terbaca: status Idle, riwayat kosong.
    // Field "badge" lama diabaikan; badge sekarang dihitung dari riwayat run.
    result.state = taskStateFromKey(obj.value(QStringLiteral("state")).toString());
    const QJsonArray runs = obj.value(QStringLiteral("runs")).toArray();
    for (const QJsonValue &run : runs) {
        result.runs.append(runFromJson(run.toObject()));
    }
    const QJsonObject tuning = obj.value(QStringLiteral("tuning")).toObject();
    for (auto it = tuning.constBegin(); it != tuning.constEnd(); ++it) {
        const QJsonObject entry = it.value().toObject();
        const AgentTuning value{entry.value(QStringLiteral("model")).toString(),
                                entry.value(QStringLiteral("effort")).toString()};
        if (!value.isEmpty()) {
            result.tuning.insert(it.key(), value);
        }
    }
    result.branch = branchFromJson(obj.value(QStringLiteral("branch")).toObject());

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

void FileManager::scheduleSave(const QString &projectId, const QList<TaskItem> &tasks){
    m_pendingSaves[projectId] = tasks;
    m_pendingProjects.insert(projectId);
    m_saveTimer->start(kSaveDebounceMs);
}

bool FileManager::deleteProject(const QString &projectId, QString *error){
    // Batalkan dulu save tertunda: tanpa ini timer debounce bisa menulis ulang
    // session.json beberapa ratus milidetik setelah foldernya dihapus.
    m_pendingSaves.remove(projectId);
    m_pendingProjects.remove(projectId);
    m_workingDirs.remove(projectId);
    m_referenceDirs.remove(projectId);

    QDir projectDir(QStringLiteral("%1/projects/%2").arg(m_basePath, projectId));
    if (!projectDir.exists()) {
        // Project yang belum pernah tersimpan ke disk dianggap sudah bersih
        return true;
    }

    if (!projectDir.removeRecursively()) {
        if (error) {
            *error = QStringLiteral("Gagal menghapus folder %1").arg(projectDir.absolutePath());
        }
        return false;
    }

    return true;
}

QString FileManager::workingDirectory(const QString &projectId) const {
    return m_workingDirs.value(projectId);
}

void FileManager::setWorkingDirectory(const QString &projectId, const QString &dir) {
    m_workingDirs.insert(projectId, dir);
}

QStringList FileManager::referenceDirectories(const QString &projectId) const {
    return m_referenceDirs.value(projectId);
}

void FileManager::setReferenceDirectories(const QString &projectId, const QStringList &dirs) {
    if (dirs.isEmpty()) {
        m_referenceDirs.remove(projectId);
    } else {
        m_referenceDirs.insert(projectId, dirs);
    }
}

QString FileManager::attachmentDirectory(const QString &projectId, const QString &taskId) const {
    // Tanpa id, path-nya menunjuk folder induk yang dipakai bersama; kosong berarti "tidak ada"
    if (projectId.isEmpty() || taskId.isEmpty()) {
        return QString();
    }
    return QStringLiteral("%1/projects/%2/attachments/%3").arg(m_basePath, projectId, taskId);
}

bool FileManager::deleteAttachments(const QString &projectId, const QString &taskId, QString *error) {
    // Path kosong = QDir("") = folder kerja proses: jangan pernah dihapus
    const QString path = attachmentDirectory(projectId, taskId);
    if (path.isEmpty()) {
        return true;
    }
    QDir dir(path);
    if (!dir.exists() || dir.removeRecursively()) {
        return true;
    }
    if (error) {
        *error = QStringLiteral("Gagal menghapus folder lampiran %1").arg(dir.absolutePath());
    }
    return false;
}

QString FileManager::worktreeDirectory(const QString &projectId, const QString &taskId) const {
    if (projectId.isEmpty() || taskId.isEmpty()) {
        return QString();
    }
    return QStringLiteral("%1/projects/%2/worktrees/%3").arg(m_basePath, projectId, taskId);
}

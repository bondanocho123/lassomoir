#pragma once

#include "AgentTypes.h"
#include "CanvasBoard.h"
#include "TaskItem.h"

#include <QHash>
#include <QImage>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QPointF>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <functional>
#include <optional>

class AgentRuntime;
class CanvasAutomation;
class CanvasChat;
class CanvasModel;
class CanvasPage;
class FileManager;
class MermaidRenderer;
class StageCatalog;
class TaskManager;

// Kanvas brainstorm semua project: memuat dan menyimpan canvas.json, membuat halaman kanvas,
// menjaga isi kartu referensi tetap sama dengan task asalnya, menjalankan langkah AI dan chat tanya
// jawab, dan membuat task dari kanvas. TaskManager tetap satu-satunya pemilik data task: task baru masuk lewat addTask.
class CanvasWorkspace : public QObject {
    Q_OBJECT

public:
    // Folder kerja project yang masih ada; boleh bertanya ke pengguna. Kosong = dibatalkan.
    using DirectoryResolver = std::function<QString(const QString &projectId)>;

    CanvasWorkspace(const StageCatalog &catalog, FileManager &files, TaskManager &tasks, AgentRuntime &runtime,
                    MermaidRenderer *renderer, DirectoryResolver ensureWorkingDirectory, QObject *parent = nullptr);
    ~CanvasWorkspace() override;

    // Halaman kanvas project; dibuat (dan kanvasnya dimuat dari disk) saat pertama dibutuhkan
    CanvasPage *page(const QString &projectId);
    CanvasPage *existingPage(const QString &projectId) const;

    // Project ditutup atau dihapus: langkah AI-nya dihentikan, isinya dijadwalkan tersimpan, dan
    // halamannya dibuang. Pemanggil melepas halaman dari tata letaknya lebih dulu.
    void closeProject(const QString &projectId);

    // Data task berubah atau hilang: kartu referensi dan pustaka mengikuti
    void taskChanged(const TaskItem &task);
    void taskRemoved(const TaskItem &task);
    void tasksReloaded();

    // Task dari kanvas sudah dibuat: kartunya ditaruh di samping kartu asal dan disambungkan
    void placeTasks(const QString &projectId, const QString &fromNodeId, const QList<TaskItem> &tasks);

    // Posisi tampilan semua kanvas ikut tersimpan (dipanggil sebelum aplikasi ditutup)
    void storeViews();

signals:
    void boardRequested(const QString &projectId);
    // "Jadikan task…": MainWindow membuka form task baru yang sudah terisi
    void taskDraftRequested(const QString &projectId, const QString &fromNodeId, const QString &title,
                            const QString &brief);
    void openTaskRequested(const QString &taskId);
    void logLine(const QString &line);
    void taskLogged(const TaskItem &task, const QString &line);
    // Langkah AI atau chat gagal karena backend-nya (mis. belum login): MainWindow menampilkan notice
    void runFailed(const AgentResult &result);

private:
    struct Canvas {
        CanvasModel *model = nullptr;
        CanvasAutomation *automation = nullptr;
        CanvasChat *chat = nullptr;
        QPointer<CanvasPage> page;
        QHash<QString, QString> live;   // stepId -> keluaran agent yang sedang mengalir
    };

    Canvas *ensure(const QString &projectId);
    void wireAutomation(const QString &projectId, Canvas *canvas);
    void wireChat(const QString &projectId, Canvas *canvas);
    void save(const QString &projectId);
    // Isi canvas.json: kartu, garis, tampilan, dan percakapan chat
    static QJsonObject snapshot(const Canvas *canvas);
    void refreshReferences(Canvas *canvas, const QString &taskId = QString());
    std::optional<AgentLaunch> launchFor(const QString &projectId, const QString &stepId, QString *reason) const;
    // Folder kerja, folder baca, dan foto lampiran dari kartu-kartu itu; agent dan prompt belum diisi
    std::optional<AgentLaunch> environmentFor(const QString &projectId, const QStringList &imageNodeIds,
                                              QString *reason) const;
    void askChat(const QString &projectId, const QString &question, const QStringList &contextIds, const QString &model);
    void addSources(const QString &projectId, const QList<CanvasSource> &sources, const QPointF &scenePos, bool exact);
    void runSteps(const QString &projectId, const QStringList &stepIds, bool withUpstream);
    void runAll(const QString &projectId);
    void requestTask(const QString &projectId, const QStringList &nodeIds);
    void createProposedTasks(const QString &projectId, const QString &stepId);
    void scheduleLibraryUpdate();
    void updateLibraries();
    QImage thumbnail(const CanvasSource &source) const;
    QString uniqueTaskId() const;
    QString stepName(const Canvas *canvas, const QString &stepId) const;
    void message(const QString &projectId, const QString &text, bool error = false);

    const StageCatalog &m_catalog;
    FileManager &m_files;
    TaskManager &m_tasks;
    AgentRuntime &m_runtime;
    MermaidRenderer *m_renderer;
    DirectoryResolver m_ensureWorkingDirectory;
    QHash<QString, Canvas *> m_canvases;
    QTimer m_libraryTimer;
};

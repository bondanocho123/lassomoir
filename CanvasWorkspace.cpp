#include "CanvasWorkspace.h"
#include "AgentRuntime.h"
#include "CanvasAutomation.h"
#include "CanvasChat.h"
#include "CanvasModel.h"
#include "CanvasPage.h"
#include "CanvasView.h"
#include "CanvasWorkflow.h"
#include "FileManager.h"
#include "RunLogFormatter.h"
#include "StageCatalog.h"
#include "TaskAttachments.h"
#include "TaskManager.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonObject>

namespace {

constexpr int kThumbnailSide = 640;

// Nama lampiran dari canvas.json dipakai sebagai nama file: jangan sampai menunjuk ke luar folder lampiran
bool isPlainName(const QString &name) {
    return !name.isEmpty() && name != QLatin1String(".") && name != QLatin1String("..")
           && !name.contains(QLatin1Char('/')) && !name.contains(QLatin1Char('\\'));
}

QString elided(const QString &text, qsizetype max) {
    return text.size() > max ? text.left(max - 1).trimmed() + QChar(0x2026) : text;
}

}

CanvasWorkspace::CanvasWorkspace(const StageCatalog &catalog, FileManager &files, TaskManager &tasks,
                                 AgentRuntime &runtime, MermaidRenderer *renderer,
                                 DirectoryResolver ensureWorkingDirectory, QObject *parent)
    : QObject(parent),
      m_catalog(catalog),
      m_files(files),
      m_tasks(tasks),
      m_runtime(runtime),
      m_renderer(renderer),
      m_ensureWorkingDirectory(std::move(ensureWorkingDirectory)) {
    m_libraryTimer.setSingleShot(true);
    m_libraryTimer.setInterval(250);
    connect(&m_libraryTimer, &QTimer::timeout, this, &CanvasWorkspace::updateLibraries);
}

CanvasWorkspace::~CanvasWorkspace() {
    // Halaman memegang referensi ke model, automation, dan chat kanvasnya: dibongkar lebih dulu
    for (Canvas *canvas : std::as_const(m_canvases)) {
        delete canvas->page;
        delete canvas->automation;
        delete canvas->chat;
        delete canvas->model;
        delete canvas;
    }
}

CanvasWorkspace::Canvas *CanvasWorkspace::ensure(const QString &projectId) {
    if (Canvas *existing = m_canvases.value(projectId)) {
        return existing;
    }
    auto *canvas = new Canvas;
    canvas->model = new CanvasModel(projectId);

    QString error;
    const QJsonObject stored = m_files.loadCanvas(projectId, &error);
    CanvasBoard board;
    QList<CanvasChatMessage> chatMessages;
    if (error.isEmpty() && !stored.isEmpty()) {
        QStringList warnings;
        const std::optional<CanvasBoard> parsed = CanvasBoard::fromJson(stored, &error, &warnings);
        if (parsed) {
            board = *parsed;
            chatMessages = CanvasChat::fromJson(stored.value(QStringLiteral("chat")).toArray());
        }
        for (const QString &warning : std::as_const(warnings)) {
            emit logLine(QStringLiteral("[KANVAS WARN] %1: %2").arg(projectId, warning));
        }
    }
    if (!error.isEmpty()) {
        // File yang tidak terbaca tidak ditimpa: disisihkan, lalu kanvas mulai baru
        const QString aside = m_files.setAsideCanvas(projectId);
        emit logLine(aside.isEmpty()
                         ? QStringLiteral("[KANVAS WARN] %1: %2").arg(projectId, error)
                         : QStringLiteral("[KANVAS WARN] %1: %2. Isinya disimpan sebagai %3; kanvas dimulai baru.")
                               .arg(projectId, error, QDir::toNativeSeparators(aside)));
    }
    canvas->model->load(board);
    m_canvases.insert(projectId, canvas);
    refreshReferences(canvas);

    canvas->automation = new CanvasAutomation(
        *canvas->model, m_runtime,
        [this, projectId](const QString &stepId, QString *reason) { return launchFor(projectId, stepId, reason); });
    wireAutomation(projectId, canvas);
    // Foto lampiran di antara kartu bahan ikut sebagai blok gambar, sama seperti langkah AI
    auto chatLaunch = [this, projectId](const QStringList &contextIds, QString *reason) -> std::optional<AgentLaunch> {
        const Canvas *current = m_canvases.value(projectId);
        if (!current) {
            *reason = QStringLiteral("kanvas sudah ditutup");
            return std::nullopt;
        }
        return environmentFor(projectId, CanvasWorkflow::chatContext(current->model->board(), contextIds), reason);
    };
    canvas->chat = new CanvasChat(*canvas->model, m_runtime, chatLaunch);
    canvas->chat->load(chatMessages);
    wireChat(projectId, canvas);
    connect(canvas->model, &CanvasModel::changed, this, [this, projectId]() { save(projectId); });
    // Undo/redo bisa mengembalikan kartu referensi lama: isinya disegarkan dari task-nya
    connect(canvas->model, &CanvasModel::boardReset, this, [this, projectId]() {
        if (Canvas *current = m_canvases.value(projectId)) {
            refreshReferences(current);
        }
    });
    return canvas;
}

void CanvasWorkspace::wireAutomation(const QString &projectId, Canvas *canvas) {
    CanvasAutomation *automation = canvas->automation;
    connect(automation, &CanvasAutomation::stepStarted, this,
            [this, projectId](const QString &stepId, const AgentLaunch &launch) {
        Canvas *current = m_canvases.value(projectId);
        if (!current) {
            return;
        }
        current->live.insert(stepId, QString());
        emit logLine(QStringLiteral("[KANVAS] %1 · ▶ langkah \"%2\" · %3 · %4")
                         .arg(projectId, stepName(current, stepId), launch.agent.model, launch.agent.effort));
    });
    connect(automation, &CanvasAutomation::stepEvent, this,
            [this, projectId](const QString &stepId, const AgentEvent &event) {
        Canvas *current = m_canvases.value(projectId);
        if (!current) {
            return;
        }
        QString &live = current->live[stepId];
        if (event.kind == AgentEvent::Kind::Text) {
            live += event.text.trimmed() + QStringLiteral("\n\n");
        } else if (event.kind == AgentEvent::Kind::ToolUse) {
            live += QStringLiteral("`%1`\n\n").arg(RunLogFormatter::toolLabel(event, m_files.workingDirectory(projectId)));
        } else {
            return;
        }
        if (current->page) {
            current->page->setLiveOutput(stepId, live);
        }
    });
    connect(automation, &CanvasAutomation::stepFinished, this,
            [this, projectId](const QString &stepId, const AgentResult &result) {
        Canvas *current = m_canvases.value(projectId);
        if (!current) {
            return;
        }
        current->live.remove(stepId);
        if (current->page) {
            current->page->setLiveOutput(stepId, QString());
        }
        const QString name = stepName(current, stepId);
        if (result.success) {
            QStringList facts = {RunLogFormatter::formatDuration(result.durationMs),
                                 QStringLiteral("%1 tok").arg(RunLogFormatter::formatTokens(result.totalTokens))};
            if (result.costUsd > 0) {
                facts.append(QStringLiteral("$%1").arg(result.costUsd, 0, 'f', 2));
            }
            emit logLine(QStringLiteral("[KANVAS] %1 · ✓ langkah \"%2\" selesai · %3")
                             .arg(projectId, name, facts.join(QStringLiteral(" · "))));
            message(projectId, QStringLiteral("Langkah \"%1\" selesai").arg(name));
            return;
        }
        if (result.outcome == QLatin1String("skipped") || result.outcome == QLatin1String("cancelled")) {
            emit logLine(QStringLiteral("[KANVAS] %1 · langkah \"%2\" %3").arg(projectId, name, result.message));
            return;
        }
        emit logLine(QStringLiteral("[KANVAS] %1 · ✗ langkah \"%2\" %3: %4")
                         .arg(projectId, name,
                              result.outcome == QLatin1String("rejected") ? QStringLiteral("tidak dijalankan")
                                                                          : QStringLiteral("gagal (%1)").arg(result.outcome),
                              result.message));
        message(projectId, QStringLiteral("Langkah \"%1\": %2").arg(name, elided(result.message, 160)), true);
        if (result.outcome != QLatin1String("rejected")) {
            emit runFailed(result);
        }
    });
    connect(automation, &CanvasAutomation::batchFinished, this,
            [this, projectId](int succeeded, int failed, int skipped) {
        if (succeeded + failed + skipped < 2) {
            return;   // satu langkah sudah dilaporkan sendiri
        }
        const QString summary = QStringLiteral("Alur selesai: %1 berhasil · %2 gagal · %3 dilewati")
                                    .arg(succeeded).arg(failed).arg(skipped);
        emit logLine(QStringLiteral("[KANVAS] %1 · %2").arg(projectId, summary));
        message(projectId, summary, failed > 0);
    });
}

void CanvasWorkspace::wireChat(const QString &projectId, Canvas *canvas) {
    CanvasChat *chat = canvas->chat;
    connect(chat, &CanvasChat::changed, this, [this, projectId]() { save(projectId); });
    connect(chat, &CanvasChat::started, this, [this, projectId](const AgentLaunch &launch) {
        emit logLine(QStringLiteral("[KANVAS] %1 · chat ▶ %2 · %3").arg(projectId, launch.agent.model, launch.agent.effort));
    });
    connect(chat, &CanvasChat::finished, this,
            [this, projectId](const CanvasChatMessage &answer, const AgentResult &result) {
        if (answer.succeeded()) {
            QStringList facts = {RunLogFormatter::formatDuration(result.durationMs),
                                 QStringLiteral("%1 tok").arg(RunLogFormatter::formatTokens(result.totalTokens))};
            if (result.costUsd > 0) {
                facts.append(QStringLiteral("$%1").arg(result.costUsd, 0, 'f', 2));
            }
            emit logLine(QStringLiteral("[KANVAS] %1 · ✓ chat dijawab · %2").arg(projectId, facts.join(QStringLiteral(" · "))));
            return;
        }
        if (answer.outcome == QLatin1String("cancelled")) {
            emit logLine(QStringLiteral("[KANVAS] %1 · chat dihentikan").arg(projectId));
            return;
        }
        emit logLine(QStringLiteral("[KANVAS] %1 · ✗ chat gagal (%2): %3").arg(projectId, answer.outcome, result.message));
        message(projectId, QStringLiteral("Chat gagal: %1").arg(elided(result.message, 160)), true);
        emit runFailed(result);
    });
}

void CanvasWorkspace::askChat(const QString &projectId, const QString &question, const QStringList &contextIds,
                              const QString &model) {
    Canvas *canvas = m_canvases.value(projectId);
    if (!canvas) {
        return;
    }
    QString reason;
    // Agent membaca kode project: folder kerja dipilih dulu bila belum ada
    if (m_ensureWorkingDirectory && m_ensureWorkingDirectory(projectId).isEmpty()) {
        reason = QStringLiteral("chat butuh folder kerja project: pilih foldernya dulu");
    } else if (canvas->chat->ask(question, contextIds, model, &reason)) {
        return;
    }
    if (canvas->page) {
        canvas->page->showChatError(reason);
    }
    QString missing;
    if (!m_runtime.isAvailable(&missing)) {
        emit runFailed(AgentResult::failure(QStringLiteral("failed_to_start"), missing));
    }
}

CanvasPage *CanvasWorkspace::page(const QString &projectId) {
    Canvas *canvas = ensure(projectId);
    if (canvas->page) {
        return canvas->page;
    }
    auto *page = new CanvasPage(*canvas->model, *canvas->automation, *canvas->chat, m_renderer);
    page->view()->setThumbnailProvider([this](const CanvasSource &source) { return thumbnail(source); });
    connect(page, &CanvasPage::boardRequested, this, [this, projectId]() { emit boardRequested(projectId); });
    connect(page, &CanvasPage::sourcesDropped, this,
            [this, projectId](const QList<CanvasSource> &sources, const QPointF &scenePos, bool exact) {
        addSources(projectId, sources, scenePos, exact);
    });
    connect(page, &CanvasPage::runRequested, this, [this, projectId](const QStringList &stepIds, bool withUpstream) {
        runSteps(projectId, stepIds, withUpstream);
    });
    connect(page, &CanvasPage::runAllRequested, this, [this, projectId]() { runAll(projectId); });
    connect(page, &CanvasPage::createTaskRequested, this, [this, projectId](const QStringList &nodeIds) {
        requestTask(projectId, nodeIds);
    });
    connect(page, &CanvasPage::proposalsRequested, this, [this, projectId](const QString &stepId) {
        createProposedTasks(projectId, stepId);
    });
    connect(page, &CanvasPage::openTaskRequested, this, &CanvasWorkspace::openTaskRequested);
    connect(page, &CanvasPage::viewStored, this, [this, projectId]() { save(projectId); });
    connect(page, &CanvasPage::chatAskRequested, this,
            [this, projectId](const QString &question, const QStringList &contextIds, const QString &model) {
        askChat(projectId, question, contextIds, model);
    });
    canvas->page = page;
    page->setLibraryTasks(m_tasks.allTasks(), m_catalog.keys());
    return page;
}

CanvasPage *CanvasWorkspace::existingPage(const QString &projectId) const {
    const Canvas *canvas = m_canvases.value(projectId);
    return canvas ? canvas->page.data() : nullptr;
}

void CanvasWorkspace::closeProject(const QString &projectId) {
    Canvas *canvas = m_canvases.take(projectId);
    if (!canvas) {
        return;
    }
    canvas->automation->cancelAll();
    canvas->chat->cancel();
    if (canvas->page) {
        canvas->page->storeView();
        canvas->page->deleteLater();
    }
    m_files.scheduleCanvasSave(projectId, snapshot(canvas));
    // Dibuang di siklus event berikutnya, urut: halaman dulu, baru automation, chat, dan model yang dipakainya
    canvas->automation->deleteLater();
    canvas->chat->deleteLater();
    canvas->model->deleteLater();
    delete canvas;
}

void CanvasWorkspace::save(const QString &projectId) {
    if (const Canvas *canvas = m_canvases.value(projectId)) {
        m_files.scheduleCanvasSave(projectId, snapshot(canvas));
    }
}

QJsonObject CanvasWorkspace::snapshot(const Canvas *canvas) {
    QJsonObject root = canvas->model->board().toJson();
    const QJsonArray chat = canvas->chat->toJson();
    if (!chat.isEmpty()) {
        root[QStringLiteral("chat")] = chat;
    }
    return root;
}

void CanvasWorkspace::storeViews() {
    for (Canvas *canvas : std::as_const(m_canvases)) {
        if (canvas->page) {
            canvas->page->storeView();
        }
    }
}

void CanvasWorkspace::taskChanged(const TaskItem &task) {
    for (Canvas *canvas : std::as_const(m_canvases)) {
        refreshReferences(canvas, task.id);
    }
    scheduleLibraryUpdate();
}

void CanvasWorkspace::taskRemoved(const TaskItem &task) {
    taskChanged(task);
}

void CanvasWorkspace::tasksReloaded() {
    for (Canvas *canvas : std::as_const(m_canvases)) {
        refreshReferences(canvas);
    }
    scheduleLibraryUpdate();
}

void CanvasWorkspace::refreshReferences(Canvas *canvas, const QString &taskId) {
    // Salinan: refreshReference memancarkan sinyal yang bisa mengubah daftar kartu
    const QList<CanvasNode> nodes = canvas->model->board().nodes;
    for (const CanvasNode &node : nodes) {
        if (!node.isReference() || (!taskId.isEmpty() && node.source.taskId != taskId)) {
            continue;
        }
        std::optional<TaskItem> task = m_tasks.task(node.source.taskId);
        if (task && task->projectId != node.source.projectId) {
            task.reset();
        }
        const CanvasWorkflow::Reference reference = CanvasWorkflow::resolve(
            node.source, task, m_files.attachmentDirectory(node.source.projectId, node.source.taskId));
        if (reference.available) {
            canvas->model->refreshReference(node.id, reference.title, reference.detail, reference.text, true);
        } else {
            // Sumbernya hilang: salinan terakhir tetap dipakai, kartu ditandai tidak tersedia
            canvas->model->refreshReference(node.id, node.title, node.detail, node.text, false);
        }
    }
}

std::optional<AgentLaunch> CanvasWorkspace::launchFor(const QString &projectId, const QString &stepId,
                                                      QString *reason) const {
    const Canvas *canvas = m_canvases.value(projectId);
    if (!canvas) {
        *reason = QStringLiteral("kanvas sudah ditutup");
        return std::nullopt;
    }
    const CanvasBoard &board = canvas->model->board();
    const QString problem = CanvasWorkflow::stepProblem(board, stepId);
    if (!problem.isEmpty()) {
        *reason = problem;
        return std::nullopt;
    }
    // Foto lampiran yang tersambung ikut sebagai blok gambar
    std::optional<AgentLaunch> launch = environmentFor(projectId, board.inputsOf(stepId), reason);
    if (launch) {
        const CanvasNode *step = board.node(stepId);
        launch->agent = CanvasWorkflow::brainstormAgent(step->model, step->effort);
        launch->prompt = CanvasWorkflow::stepPrompt(board, stepId);
    }
    return launch;
}

std::optional<AgentLaunch> CanvasWorkspace::environmentFor(const QString &projectId, const QStringList &imageNodeIds,
                                                           QString *reason) const {
    const Canvas *canvas = m_canvases.value(projectId);
    if (!canvas) {
        *reason = QStringLiteral("kanvas sudah ditutup");
        return std::nullopt;
    }
    const QString directory = m_files.workingDirectory(projectId);
    if (directory.isEmpty() || !QDir(directory).exists()) {
        *reason = QStringLiteral("folder kerja project %1 belum dipilih atau sudah tidak ada").arg(projectId);
        return std::nullopt;
    }

    const CanvasBoard &board = canvas->model->board();
    AgentLaunch launch;
    launch.workingDirectory = directory;
    const QStringList references = m_files.referenceDirectories(projectId);
    for (const QString &reference : references) {
        if (QFileInfo(reference).isDir()) {
            launch.readableDirectories.append(QDir::toNativeSeparators(QDir::cleanPath(reference)));
        }
    }
    // Foto lampiran ikut sebagai blok gambar, dalam batas yang sama dengan lampiran task
    qint64 imageBytes = 0;
    for (const QString &input : imageNodeIds) {
        const CanvasNode *node = board.node(input);
        if (!node || !CanvasWorkflow::isImageReference(*node) || !isPlainName(node->source.attachment)) {
            continue;
        }
        const QString path = QDir(m_files.attachmentDirectory(node->source.projectId, node->source.taskId))
                                 .filePath(node->source.attachment);
        const QFileInfo info(path);
        if (!info.isFile() || launch.imagePaths.size() >= TaskAttachments::kMaxImages
            || imageBytes + info.size() > TaskAttachments::kMaxTotalImageBytes) {
            continue;
        }
        imageBytes += info.size();
        launch.imagePaths.append(QDir::toNativeSeparators(path));
    }
    return launch;
}

void CanvasWorkspace::addSources(const QString &projectId, const QList<CanvasSource> &sources, const QPointF &scenePos,
                                 bool exact) {
    Canvas *canvas = m_canvases.value(projectId);
    if (!canvas) {
        return;
    }
    QStringList added;
    int missing = 0;
    QRectF previous;
    canvas->model->beginMacro();
    for (const CanvasSource &source : sources) {
        const std::optional<TaskItem> task = m_tasks.task(source.taskId);
        if (!task || task->projectId != source.projectId) {
            ++missing;
            continue;
        }
        const CanvasWorkflow::Reference reference = CanvasWorkflow::resolve(
            source, task, m_files.attachmentDirectory(source.projectId, source.taskId));
        const QSizeF size = CanvasNode::defaultSize(source.isArtifact() ? CanvasNodeKind::Artifact : CanvasNodeKind::Task);
        // Kartu pertama yang dijatuhkan tepat di bawah kursor; kartu berikutnya berjajar ke bawah, dan
        // kartu dari klik dua kali di pustaka dicarikan tempat lapang
        QPointF pos;
        if (!previous.isNull()) {
            pos = canvas->model->board().openSpot(QRectF(previous.bottomLeft() + QPointF(0, 24), size));
        } else if (exact) {
            pos = scenePos - QPointF(size.width() / 2, 40);
            if (canvas->page) {
                pos = canvas->page->view()->freeSpot(pos);
            }
        } else {
            pos = canvas->model->board().openSpot(QRectF(scenePos - QPointF(size.width() / 2, size.height() / 2), size));
        }
        const QString id = canvas->model->addReference(source, pos,
                                                       reference.title.isEmpty() ? task->title : reference.title,
                                                       reference.detail, reference.text, reference.available);
        if (!id.isEmpty()) {
            added.append(id);
            previous = QRectF(pos, size);
        }
    }
    canvas->model->endMacro();
    if (canvas->page && !added.isEmpty()) {
        canvas->page->view()->selectNodes(added);
    }
    if (missing > 0) {
        message(projectId, QStringLiteral("%1 sumber sudah tidak ada dan dilewati").arg(missing), true);
    }
}

void CanvasWorkspace::runSteps(const QString &projectId, const QStringList &stepIds, bool withUpstream) {
    Canvas *canvas = m_canvases.value(projectId);
    if (!canvas || stepIds.isEmpty()) {
        return;
    }
    // Agent bekerja di folder kerja project (membaca kodenya); folder dipilih dulu bila belum ada
    if (m_ensureWorkingDirectory && m_ensureWorkingDirectory(projectId).isEmpty()) {
        message(projectId, QStringLiteral("Langkah AI butuh folder kerja project: pilih foldernya dulu"), true);
        return;
    }
    for (const QString &stepId : stepIds) {
        QString reason;
        const bool queued = withUpstream ? canvas->automation->runWithUpstream(stepId, &reason)
                                         : canvas->automation->run(stepId, &reason);
        if (!queued) {
            message(projectId, QStringLiteral("Langkah \"%1\": %2").arg(stepName(canvas, stepId), reason), true);
        }
    }
    QString missing;
    if (!m_runtime.isAvailable(&missing)) {
        emit runFailed(AgentResult::failure(QStringLiteral("failed_to_start"), missing));
    }
}

void CanvasWorkspace::runAll(const QString &projectId) {
    Canvas *canvas = m_canvases.value(projectId);
    if (!canvas) {
        return;
    }
    if (canvas->model->board().stepIds().isEmpty()) {
        message(projectId, QStringLiteral("Belum ada langkah AI. Tekan L atau tombol Langkah AI untuk membuatnya."), true);
        return;
    }
    if (m_ensureWorkingDirectory && m_ensureWorkingDirectory(projectId).isEmpty()) {
        message(projectId, QStringLiteral("Langkah AI butuh folder kerja project: pilih foldernya dulu"), true);
        return;
    }
    QString reason;
    if (!canvas->automation->runAll(&reason)) {
        message(projectId, reason, true);
        QString missing;
        if (!m_runtime.isAvailable(&missing)) {
            emit runFailed(AgentResult::failure(QStringLiteral("failed_to_start"), missing));
        }
    }
}

void CanvasWorkspace::requestTask(const QString &projectId, const QStringList &nodeIds) {
    const Canvas *canvas = m_canvases.value(projectId);
    if (!canvas) {
        return;
    }
    const CanvasBoard &board = canvas->model->board();
    QStringList existing;
    for (const QString &id : nodeIds) {
        if (board.node(id)) {
            existing.append(id);
        }
    }
    if (existing.isEmpty()) {
        return;
    }
    emit taskDraftRequested(projectId, existing.first(), CanvasWorkflow::suggestedTitle(*board.node(existing.first())),
                            CanvasWorkflow::taskBrief(board, existing));
}

void CanvasWorkspace::createProposedTasks(const QString &projectId, const QString &stepId) {
    const Canvas *canvas = m_canvases.value(projectId);
    const CanvasNode *step = canvas ? canvas->model->node(stepId) : nullptr;
    if (!step || !step->result.success) {
        return;
    }
    QString error;
    const QList<CanvasWorkflow::TaskProposal> proposals = CanvasWorkflow::parseTaskProposals(step->result.message, &error);
    if (proposals.isEmpty()) {
        message(projectId, error, true);
        return;
    }
    const QString origin = stepName(canvas, stepId);
    QList<TaskItem> created;
    for (const CanvasWorkflow::TaskProposal &proposal : proposals) {
        TaskItem task;
        task.id = uniqueTaskId();
        task.projectId = projectId;
        task.stage = QStringLiteral("WAITING");
        task.category = proposal.category;
        task.title = proposal.title;
        // Teks biasa: subtext tampil apa adanya di kartu kanban
        const QString provenance = QStringLiteral("Diusulkan dari kanvas brainstorm, langkah \"%1\".").arg(origin);
        task.subtext = proposal.instructions.isEmpty() ? provenance
                                                       : QStringLiteral("%1\n\n%2").arg(proposal.instructions, provenance);
        // TaskManager tetap pemilik data: MainWindow membuat kartunya dan menyimpan session.json
        m_tasks.addTask(task);
        created.append(task);
        emit taskLogged(task, QStringLiteral("[TASK CREATED] %1 -> WAITING: '%2' (usulan kanvas)").arg(projectId, task.title));
    }
    placeTasks(projectId, stepId, created);
    message(projectId, QStringLiteral("%1 task dibuat di WAITING dan ditaruh di kanvas").arg(created.size()));
}

void CanvasWorkspace::placeTasks(const QString &projectId, const QString &fromNodeId, const QList<TaskItem> &tasks) {
    Canvas *canvas = m_canvases.value(projectId);
    if (!canvas || tasks.isEmpty()) {
        return;
    }
    const QSizeF size = CanvasNode::defaultSize(CanvasNodeKind::Task);
    // Kolom di kanan kartu asalnya, di tempat yang belum terisi kartu lain. Pointer kartu tidak
    // disimpan: menambah kartu bisa memindahkan isi daftar kartu model.
    const CanvasNode *from = canvas->model->node(fromNodeId);
    const bool linked = from != nullptr;
    QPointF next = from ? from->pos + QPointF(from->size.width() + 120, 0)
                        : (canvas->page ? canvas->page->view()->centerScenePos() - QPointF(size.width() / 2, size.height() / 2)
                                        : QPointF());
    QStringList added;
    canvas->model->beginMacro();
    for (const TaskItem &task : tasks) {
        const CanvasSource source{projectId, task.id, QString(), QString()};
        const CanvasWorkflow::Reference reference = CanvasWorkflow::resolve(source, task, QString());
        const QPointF pos = canvas->model->board().openSpot(QRectF(next, size));
        next = pos + QPointF(0, size.height() + 24);
        const QString id = canvas->model->addReference(source, pos, reference.title, reference.detail, reference.text);
        if (linked) {
            canvas->model->connectNodes(fromNodeId, id);
        }
        added.append(id);
    }
    canvas->model->endMacro();
    if (canvas->page) {
        canvas->page->view()->selectNodes(added);
    }
}

void CanvasWorkspace::scheduleLibraryUpdate() {
    if (!m_canvases.isEmpty()) {
        m_libraryTimer.start();
    }
}

void CanvasWorkspace::updateLibraries() {
    const QList<TaskItem> tasks = m_tasks.allTasks();
    const QStringList stages = m_catalog.keys();
    for (Canvas *canvas : std::as_const(m_canvases)) {
        if (canvas->page) {
            canvas->page->setLibraryTasks(tasks, stages);
        }
    }
}

QImage CanvasWorkspace::thumbnail(const CanvasSource &source) const {
    if (!isPlainName(source.attachment)) {
        return QImage();
    }
    QImageReader reader(QDir(m_files.attachmentDirectory(source.projectId, source.taskId)).filePath(source.attachment));
    reader.setAutoTransform(true);
    const QSize size = reader.size();
    if (size.isValid() && (size.width() > kThumbnailSide || size.height() > kThumbnailSide)) {
        reader.setScaledSize(size.scaled(kThumbnailSide, kThumbnailSide, Qt::KeepAspectRatio));
    }
    return reader.read();
}

QString CanvasWorkspace::uniqueTaskId() const {
    // Sama dengan id dari form New Task (milidetik), tapi dijamin unik saat banyak task dibuat sekaligus
    qint64 stamp = QDateTime::currentMSecsSinceEpoch();
    while (m_tasks.task(QString::number(stamp))) {
        ++stamp;
    }
    return QString::number(stamp);
}

QString CanvasWorkspace::stepName(const Canvas *canvas, const QString &stepId) const {
    const CanvasNode *step = canvas ? canvas->model->node(stepId) : nullptr;
    const QString first = step ? step->text.trimmed().section(QLatin1Char('\n'), 0, 0).trimmed() : QString();
    return first.isEmpty() ? QStringLiteral("tanpa instruksi") : elided(first, 60);
}

void CanvasWorkspace::message(const QString &projectId, const QString &text, bool error) {
    const Canvas *canvas = m_canvases.value(projectId);
    if (canvas && canvas->page) {
        canvas->page->showMessage(text, error);
    }
}

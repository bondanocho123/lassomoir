#include "BranchViewer.h"
#include "CodeMetrics.h"
#include "ConsolePanelWidget.h"
#include "ConsoleTaskCard.h"
#include "DiagramViewer.h"
#include "DiffView.h"
#include "FakeAgentRuntime.h"
#include "GitSandbox.h"
#include "HoverInfoPopup.h"
#include "KanbanCardWidget.h"
#include "KanbanColumnWidget.h"
#include "MaintainabilityView.h"
#include "MarkdownView.h"
#include "MermaidRenderer.h"
#include "PromptComposer.h"
#include "PromptEditor.h"
#include "ResponseDrawer.h"
#include "RunPulse.h"
#include "StageCatalog.h"
#include "StageInfo.h"
#include "SwarmCoordinator.h"
#include "SwimlaneWidget.h"
#include "TaskAttachments.h"
#include "TaskManager.h"
#include "Theme.h"
#include "WorkspaceDiff.h"
#include "mainwindow.h"

#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QDropEvent>
#include <QEnterEvent>
#include <QFile>
#include <QFileDialog>
#include <QGraphicsView>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QSplitter>
#include <QStandardPaths>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <QWheelEvent>
#include <QtTest>

#include <algorithm>
#include <functional>
#include <memory>

namespace {

// Renderer palsu: mencatat permintaan, test yang memutuskan kapan gambar/gagal dikirim
class FakeMermaidRenderer final : public MermaidRenderer {
public:
    bool isAvailable(QString *reason) const override {
        Q_UNUSED(reason);
        return true;
    }
    void render(const QString &code) override { requests.append(code); }

    void complete(const QString &code, const QImage &image) { emit rendered(keyFor(code), image); }
    void fail(const QString &code, const QString &error) { emit failed(keyFor(code), error); }

    QStringList requests;
};

AgentResult successResult(const QString &message) {
    AgentResult result;
    result.success = true;
    result.outcome = QStringLiteral("success");
    result.message = message;
    return result;
}

QJsonObject taskJson(const QString &id, const QString &stage, const QString &title) {
    QJsonObject task;
    task[QStringLiteral("id")] = id;
    task[QStringLiteral("projectId")] = QStringLiteral("Demo");
    task[QStringLiteral("stage")] = stage;
    task[QStringLiteral("category")] = QStringLiteral("utility");
    task[QStringLiteral("title")] = title;
    task[QStringLiteral("subtext")] = QString();
    task[QStringLiteral("badge")] = QStringLiteral("✓ 0");
    return task;
}

bool runGit(const QString &directory, const QStringList &arguments) {
    QProcess process;
    process.setWorkingDirectory(directory);
    process.start(QStandardPaths::findExecutable(QStringLiteral("git")),
                  QStringList{QStringLiteral("-c"), QStringLiteral("user.name=Lassomoir Test"),
                              QStringLiteral("-c"), QStringLiteral("user.email=test@lassomoir.local"),
                              QStringLiteral("-c"), QStringLiteral("commit.gpgsign=false")}
                      + arguments);
    return process.waitForFinished(20000) && process.exitStatus() == QProcess::NormalExit
           && process.exitCode() == 0;
}

bool writeFile(const QString &path, const QByteArray &content) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(content) == content.size();
}

QByteArray readFile(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QImage solidImage(const QSize &size, const QColor &color) {
    QImage image(size, QImage::Format_RGB32);
    image.fill(color);
    return image;
}

QByteArray jpegBytes(const QImage &image) {
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "JPEG");
    return bytes;
}

// Widget anak yang masih tampil. Kotak prompt dan popup menyusun ulang isinya dengan deleteLater,
// jadi widget lama (atau baris induknya) sempat tersisa dalam keadaan tersembunyi.
template <typename T>
QList<T *> shownChildren(const QWidget *parent, const QString &name) {
    QList<T *> result;
    const QList<T *> children = parent->findChildren<T *>(name);
    for (T *child : children) {
        if (child->isVisibleTo(parent)) {
            result.append(child);
        }
    }
    return result;
}

}

// MainWindow asli + SwarmCoordinator asli, hanya runtime agent yang palsu:
// menguji jalur tombol kartu -> coordinator -> status kartu & konsol tanpa memanggil claude
class TestGui : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void runButtonDrivesCardAndConsole();
    void waitingCardCannotRun();
    void stopButtonCancelsRun();
    void closingProjectStopsItsAgents();
    // Kartu berkedip selama agent-nya berjalan (bukan saat antre), di board dan di konsol
    void runningCardsBlink();
    // Panel konsol: notifikasi dikelompokkan per task dalam kartu, ditambah kartu Sistem
    void consoleListsTasksAsCards();
    // Penanda Live di kepala konsol berupa ikon sinyal, bukan teks
    void consoleShowsLiveAsSignalIcon();

    // Hover judul atau ikon info di tiap kolom stage memunculkan penjelasan tugas dan fitur stage itu
    // seketika, tanpa berkedip saat kursor berpindah antara keduanya
    void stageColumnsExplainThemselves();

    void cardShowsHandCursor();
    void workingDirButtonShowsIconAndCaption();
    void doubleClickEditsTask();
    void cancelledEditKeepsTask();
    void deleteTaskFromCardMenu();
    void moveTaskFromCardMenu();

    // Review gate, drawer, dan viewer
    void specifierReviewApproveFlow();
    void revisionRerunsWithPreviousDocument();
    void dragPastGateIsRejected();
    void drawerShowsLiveOutput();
    void drawerExpandFillsBoardAndRestores();
    void reviewSurvivesRestart();
    void markdownViewRendersMermaid();
    // Diagram bisa di-zoom: klik diagram -> DiagramViewer
    void markdownViewOpensZoomableDiagram();
    void diagramViewerZoomsAndPans();
    void diagramViewerKeepsSmallDiagramAtActualSize();
    void architectDrawerShowsCodeChanges();
    void coderDrawerShowsCodeChanges();
    // Alur git: CODER di worktree -> QA (commit + push) -> dikembalikan -> QA lagi -> disetujui
    // tanpa merge (branch dibiarkan untuk di-PR manual)
    void qaFlowCommitsPushesWithoutMerging();
    // Form New Task: pilih branch dasar (termasuk yang baru ada di origin) -> di-pull; run pull lagi
    void newTaskChoosesBranchAndPulls();
    void maintainabilityViewShowsCSharpMembers();
    void umlTabShowsClassAndAgentDiagrams();
    // Perubahan dua kolom (sebelum | sesudah) yang sejajar dan digulir bersamaan
    void diffViewComparesSideBySide();
    // Jendela branch & commit: pilih branch, daftar commit, bandingkan commit dan branch
    void branchViewerComparesBranchesAndCommits();

    // Kotak prompt berfoto, lampiran Excel/Word/CSV, dan folder referensi
    void promptEditorAttachesPhotosAndDocuments();
    void newTaskWithAttachmentsFeedsRun();
    void referenceFoldersFeedRuns();

    // Nama project panjang dipotong "…" supaya tombol hapus dan "+" di barisnya tetap terlihat
    void longProjectNameKeepsRowButtonsVisible();

    // Notice Claude Code: belum terpasang / perlu update / belum login, dengan tombol buka link atau batal
    void runtimeCheckShowsNotice_data();
    void runtimeCheckShowsNotice();
    void readyRuntimeShowsNoNotice();
    void failedRunShowsLoginNoticeOnce();
    void unavailableRuntimeShowsInstallNotice();

    // Kartu yang stage-nya selesai diberi penanda "✓ Selesai"
    void finishedStageMarksCardDone();

    // Tema ikut mode sistem: gelap = "blue night" yang diturunkan dari styles.qss
    void themeMapsTextAndFillSeparately();
    void themeDarkStyleSheetHasNoLightBackgrounds();
    void themeAppliesAndFollowsOverride();
    void themedIconRecolorsInDarkMode();
    // Stylesheet milik widget (dari .ui / setStyleSheet) ikut dipetakan, dan kembali saat mode terang
    void themeAdaptsWidgetStyleSheets();

private:
    // Dialog notice runtime yang sedang tampil; nullptr bila tidak ada
    QDialog *runtimeNotice() const;
    // Dialog modal membuka event loop sendiri di dalam handler klik; timer ini jalan di loop itu
    // dan mengisi/menutup dialog. m_dialogSeen tetap false bila dialog tidak pernah muncul.
    void driveModalDialog(std::function<void(QDialog *)> action);
    // Sama, untuk dialog yang baru muncul beberapa siklus event kemudian (mis. pemilih folder
    // yang dibuka lewat QTimer::singleShot): dicari berulang sampai ketemu
    void driveNextModalDialog(std::function<void(QDialog *)> action);
    bool m_dialogSeen = false;

    // Rakit TaskManager + MainWindow seperti main.cpp (termasuk runFinished -> recordRun)
    void createWindow();
    // Jalankan SPECIFIER task t3 dengan runtime palsu sampai menunggu review
    void finishSpecifierRun(const QString &document);

    KanbanCardWidget *card(const QString &id) const;
    QPushButton *runButton(const QString &id) const;
    ConsolePanelWidget *consolePanel() const;
    // Isi log semua kartu konsol (kartu task + kartu Sistem)
    QString consoleText() const;
    ResponseDrawer *drawer() const;

    QTemporaryDir m_workDir;
    const StageCatalog m_catalog = StageCatalog::standard();
    FakeAgentRuntime m_runtime;
    TaskPromptComposer m_composer;
    FakeMermaidRenderer m_mermaid;
    std::unique_ptr<SwarmCoordinator> m_swarm;
    std::unique_ptr<TaskManager> m_tasks;
    std::unique_ptr<MainWindow> m_window;
};

void TestGui::init() {
    // Test mode QStandardPaths: session.json ditulis di lokasi khusus test, bukan AppData asli
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QVERIFY2(base.contains(QStringLiteral("qttest"), Qt::CaseInsensitive), qPrintable(base));
    QDir(base + QStringLiteral("/projects")).removeRecursively();
    QVERIFY(QDir().mkpath(base + QStringLiteral("/projects/Demo")));

    QJsonObject root;
    root[QStringLiteral("schemaVersion")] = 1;
    root[QStringLiteral("projectId")] = QStringLiteral("Demo");
    root[QStringLiteral("workingDirectory")] = m_workDir.path();
    root[QStringLiteral("tasks")] = QJsonArray{
        taskJson(QStringLiteral("t1"), QStringLiteral("CODER"), QStringLiteral("Tulis hello")),
        taskJson(QStringLiteral("t2"), QStringLiteral("WAITING"), QStringLiteral("Masih antre")),
        taskJson(QStringLiteral("t3"), QStringLiteral("SPECIFIER"), QStringLiteral("Sederhanakan landing")),
    };
    QFile file(base + QStringLiteral("/projects/Demo/session.json"));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QJsonDocument(root).toJson());
    file.close();

    m_runtime.sessions.clear();
    m_runtime.available = true;
    m_runtime.checkResult = RuntimeCheck();
    m_runtime.failureDiagnosis = RuntimeCheck();
    m_mermaid.requests.clear();
    m_swarm = std::make_unique<SwarmCoordinator>(m_catalog, m_runtime, m_composer);
    createWindow();
}

void TestGui::createWindow() {
    m_tasks = std::make_unique<TaskManager>(m_catalog);
    TaskManager *tasks = m_tasks.get();
    connect(m_swarm.get(), &SwarmCoordinator::runFinished, tasks,
            [tasks](const TaskItem &task, const AgentResult &result) {
        tasks->recordRun(task.id, StageRun::finished(task.stage, result));
    });
    m_window = std::make_unique<MainWindow>(m_catalog, *m_tasks, *m_swarm, m_mermaid);
    m_window->show();
}

void TestGui::cleanup() {
    // Urutan sama dengan main.cpp: jendela dulu, lalu data task, baru coordinator
    m_window.reset();
    m_tasks.reset();
    m_swarm.reset();

    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (base.contains(QStringLiteral("qttest"), Qt::CaseInsensitive)) {
        QDir(base + QStringLiteral("/projects")).removeRecursively();
    }
}

KanbanCardWidget *TestGui::card(const QString &id) const {
    const QList<KanbanCardWidget *> cards = m_window->findChildren<KanbanCardWidget *>();
    for (KanbanCardWidget *candidate : cards) {
        if (candidate->id() == id) {
            return candidate;
        }
    }
    return nullptr;
}

QPushButton *TestGui::runButton(const QString &id) const {
    KanbanCardWidget *target = card(id);
    return target ? target->findChild<QPushButton *>(QStringLiteral("btnCardRun")) : nullptr;
}

ConsolePanelWidget *TestGui::consolePanel() const {
    return m_window->findChild<ConsolePanelWidget *>();
}

QString TestGui::consoleText() const {
    ConsolePanelWidget *console = consolePanel();
    return console ? console->logText() : QString();
}

ResponseDrawer *TestGui::drawer() const {
    return m_window->findChild<ResponseDrawer *>();
}

void TestGui::finishSpecifierRun(const QString &document) {
    const int before = int(m_runtime.sessions.size());
    runButton(QStringLiteral("t3"))->click();
    // Folder kerja yang repository git di-pull dulu (thread pool); folder biasa langsung jalan
    QTRY_COMPARE(int(m_runtime.sessions.size()), before + 1);
    AgentResult result = successResult(document);
    result.durationMs = 84000;
    result.totalTokens = 302000;
    result.costUsd = 0.87;
    m_runtime.sessions.last()->finishWith(result);
}

void TestGui::runButtonDrivesCardAndConsole() {
    // Folder kerja tersimpan di session.json ditampilkan di header swimlane
    auto *workingDirButton = m_window->findChild<QPushButton *>(QStringLiteral("btnWorkingDir"));
    QVERIFY(workingDirButton);
    QCOMPARE(workingDirButton->text(), QDir(m_workDir.path()).dirName());

    KanbanCardWidget *coder = card(QStringLiteral("t1"));
    QPushButton *button = runButton(QStringLiteral("t1"));
    QVERIFY(coder && button);
    QVERIFY(button->isEnabled());

    button->click();
    QCOMPARE(int(m_runtime.sessions.size()), 1);
    QVERIFY(coder->runState() == RunState::Running);
    QCOMPARE(coder->property("state").toString(), QStringLiteral("running"));
    QCOMPARE(button->toolTip(), QStringLiteral("Hentikan agent"));
    QVERIFY(consoleText().contains(QStringLiteral("[RUN] Demo/Tulis hello · CODER · %1").arg(m_workDir.path())));
    QVERIFY(consoleText().contains(QStringLiteral("Judul: Tulis hello")));

    FakeAgentSession *session = m_runtime.sessions.first();
    QCOMPARE(session->launch().workingDirectory, m_workDir.path());
    QVERIFY(session->launch().agent.rolePrompt.startsWith(QStringLiteral("Kamu adalah agen CODER")));

    AgentEvent text;
    text.kind = AgentEvent::Kind::Text;
    text.text = QStringLiteral("Halo dari agent");
    session->emitEvent(text);
    QVERIFY(consoleText().contains(QStringLiteral("[AGENT:CODER] Halo dari agent")));

    AgentResult result;
    result.success = true;
    result.outcome = QStringLiteral("success");
    result.durationMs = 1200;
    result.totalTokens = 3400;
    session->finishWith(result);

    QVERIFY(coder->runState() == RunState::Idle);
    QCOMPARE(button->toolTip(), QStringLiteral("Jalankan agent stage ini"));
    QVERIFY(consoleText().contains(QStringLiteral("[AGENT:CODER] ✓ selesai · 1.2s · 3.4k tok")));

    // CODER tanpa gate: sukses berarti kartu maju sendiri ke stage berikutnya (run tetap manual)
    SwimlaneWidget *swimlane = m_window->findChild<SwimlaneWidget *>();
    QVERIFY(swimlane);
    QCOMPARE(swimlane->stageOf(coder), QStringLiteral("CLEANER"));
    QVERIFY(consoleText().contains(QStringLiteral("[TASK] Demo/Tulis hello CODER → CLEANER")));
    QCOMPARE(int(m_runtime.sessions.size()), 1);
}

void TestGui::waitingCardCannotRun() {
    QPushButton *button = runButton(QStringLiteral("t2"));
    QVERIFY(button);
    QVERIFY(!button->isEnabled());
    QVERIFY(button->toolTip().contains(QStringLiteral("tidak menjalankan agent")));

    button->click();   // tombol nonaktif: tidak ada run
    QCOMPARE(int(m_runtime.sessions.size()), 0);
}

void TestGui::stopButtonCancelsRun() {
    KanbanCardWidget *coder = card(QStringLiteral("t1"));
    QPushButton *button = runButton(QStringLiteral("t1"));
    QVERIFY(coder && button);

    button->click();
    QVERIFY(coder->runState() == RunState::Running);

    button->click();   // ■
    QVERIFY(coder->runState() == RunState::Idle);
    QVERIFY(consoleText().contains(QStringLiteral("[AGENT:CODER] ✗ cancelled")));
}

void TestGui::closingProjectStopsItsAgents() {
    QPushButton *button = runButton(QStringLiteral("t1"));
    QVERIFY(button);
    button->click();
    QVERIFY(m_runtime.sessions.first()->isRunning());

    auto *close = m_window->findChild<QPushButton *>(QStringLiteral("btnClose"));
    QVERIFY(close);
    close->click();

    QVERIFY(!m_runtime.sessions.first()->isRunning());
    QVERIFY(consoleText().contains(QStringLiteral("[AGENT:CODER] ✗ cancelled")));
    QTRY_VERIFY(m_window->findChildren<KanbanCardWidget *>().isEmpty());
}

void TestGui::runningCardsBlink() {
    // Penulis kedua di folder kerja yang sama harus antre di belakang CODER t1
    TaskItem cleaner;
    cleaner.id = QStringLiteral("t4");
    cleaner.projectId = QStringLiteral("Demo");
    cleaner.stage = QStringLiteral("CLEANER");
    cleaner.category = QStringLiteral("utility");
    cleaner.title = QStringLiteral("Rapikan hello");
    m_tasks->addTask(cleaner);

    KanbanCardWidget *coder = card(QStringLiteral("t1"));
    KanbanCardWidget *waiting = card(QStringLiteral("t4"));
    QVERIFY(coder && waiting);
    auto *coderPulse = coder->findChild<RunPulse *>();
    auto *waitingPulse = waiting->findChild<RunPulse *>();
    QVERIFY(coderPulse && waitingPulse);
    QVERIFY(!coderPulse->isActive());
    QCOMPARE(coderPulse->level(), 0.0);

    runButton(QStringLiteral("t1"))->click();
    runButton(QStringLiteral("t4"))->click();
    QCOMPARE(int(m_runtime.sessions.size()), 1);

    // Hanya run yang benar-benar berjalan yang berkedip; yang antre cukup bergaris putus-putus
    QVERIFY(coderPulse->isActive());
    QVERIFY(!waitingPulse->isActive());
    QCOMPARE(waiting->property("state").toString(), QStringLiteral("queued"));

    // Terangnya naik-turun seiring waktu (satu siklus penuh diamati)
    qreal lowest = 1.0;
    qreal highest = 0.0;
    for (int i = 0; i < 16; ++i) {
        QTest::qWait(80);
        lowest = qMin(lowest, coderPulse->level());
        highest = qMax(highest, coderPulse->level());
    }
    QVERIFY2(highest - lowest > 0.5, qPrintable(QStringLiteral("kedip %1..%2").arg(lowest).arg(highest)));

    // Kartu task yang sama di konsol ikut berkedip
    ConsoleTaskCard *coderEntry = consolePanel()->taskCard(QStringLiteral("t1"));
    ConsoleTaskCard *waitingEntry = consolePanel()->taskCard(QStringLiteral("t4"));
    QVERIFY(coderEntry && waitingEntry);
    QVERIFY(coderEntry->findChild<RunPulse *>()->isActive());
    QVERIFY(!waitingEntry->findChild<RunPulse *>()->isActive());

    // CODER selesai: kedipnya berhenti, dan CLEANER yang tadi antre mulai berjalan dan berkedip
    m_runtime.sessions.first()->finishWith(successResult(QStringLiteral("Selesai")));
    QVERIFY(!coderPulse->isActive());
    QVERIFY(!coderEntry->findChild<RunPulse *>()->isActive());
    QCOMPARE(int(m_runtime.sessions.size()), 2);
    QVERIFY(waitingPulse->isActive());
    QVERIFY(waitingEntry->findChild<RunPulse *>()->isActive());

    // Dihentikan lewat ■: kedip berhenti juga
    runButton(QStringLiteral("t4"))->click();
    QVERIFY(!waitingPulse->isActive());
    QVERIFY(!waitingEntry->findChild<RunPulse *>()->isActive());
}

void TestGui::consoleListsTasksAsCards() {
    ConsolePanelWidget *console = consolePanel();
    QVERIFY(console);
    const auto text = [](const QWidget *parent, const QString &name) {
        auto *label = parent->findChild<QLabel *>(name);
        return label ? label->text() : QStringLiteral("<tidak ada %1>").arg(name);
    };

    // Pesan awal aplikasi masuk kartu "Sistem"; belum ada kartu task
    QCOMPARE(console->cards().size(), 1);
    ConsoleTaskCard *system = console->cards().first();
    QVERIFY(system->isSystem());
    QCOMPARE(text(system, QStringLiteral("consoleCardProject")), QStringLiteral("SISTEM"));
    QVERIFY(system->logText().contains(QStringLiteral("Projects loaded: Demo")));

    // ▶ CODER t1: satu kartu untuk task itu, paling atas, lengkap dengan status dan notifikasinya
    runButton(QStringLiteral("t1"))->click();
    ConsoleTaskCard *coder = console->taskCard(QStringLiteral("t1"));
    QVERIFY(coder);
    QCOMPARE(console->cards().first(), coder);
    QVERIFY(coder->runState() == RunState::Running);
    QCOMPARE(coder->property("state").toString(), QStringLiteral("running"));
    QCOMPARE(text(coder, QStringLiteral("consoleCardProject")), QStringLiteral("Demo"));
    QCOMPARE(text(coder, QStringLiteral("consoleCardStage")), QStringLiteral("CODER"));
    QCOMPARE(text(coder, QStringLiteral("consoleCardTitle")), QStringLiteral("Tulis hello"));
    QCOMPARE(text(coder, QStringLiteral("consoleCardStatus")), QStringLiteral("Berjalan"));
    QCOMPARE(text(coder, QStringLiteral("consoleCardTag")), QStringLiteral("RUN"));
    // Isi prompt ikut notifikasi [RUN]-nya, bukan jadi notifikasi sendiri-sendiri
    QCOMPARE(coder->entryCount(), 1);
    QVERIFY(coder->logText().contains(QStringLiteral("[RUN] Demo/Tulis hello · CODER")));
    QVERIFY(coder->logText().contains(QStringLiteral("Judul: Tulis hello")));

    // Task kedua mendapat kartunya sendiri, di atas kartu t1
    runButton(QStringLiteral("t3"))->click();
    ConsoleTaskCard *spec = console->taskCard(QStringLiteral("t3"));
    QVERIFY(spec && spec != coder);
    QCOMPARE(console->cards().first(), spec);
    QCOMPARE(console->cards().size(), 3);

    // Output agent yang mengalir masuk kartu task-nya tanpa mengubah urutan daftar.
    // Ringkasan tanpa nama task (sudah di kepala kartu): tag stage + aksinya
    AgentEvent tool;
    tool.kind = AgentEvent::Kind::ToolUse;
    tool.toolName = QStringLiteral("Write");
    tool.toolDetail = QDir(m_workDir.path()).filePath(QStringLiteral("hello.txt"));
    m_runtime.sessions.first()->emitEvent(tool);
    QCOMPARE(console->cards().first(), spec);
    QCOMPARE(coder->entryCount(), 2);
    QCOMPARE(text(coder, QStringLiteral("consoleCardTag")), QStringLiteral("CODER"));
    QTRY_COMPARE(text(coder, QStringLiteral("consoleCardSummary")), QStringLiteral("→ Write hello.txt"));
    QVERIFY(!spec->logText().contains(QStringLiteral("hello.txt")));

    // CODER selesai: kartunya naik lagi dan kepalanya mengikuti stage baru (tanpa status aktif)
    m_runtime.sessions.first()->finishWith(successResult(QStringLiteral("Selesai")));
    QCOMPARE(console->cards().first(), coder);
    QCOMPARE(text(coder, QStringLiteral("consoleCardStage")), QStringLiteral("CLEANER"));
    QVERIFY(coder->findChild<QLabel *>(QStringLiteral("consoleCardStatus"))->isHidden());
    QVERIFY(coder->logText().contains(QStringLiteral("[TASK] Demo/Tulis hello CODER → CLEANER")));
    QTRY_VERIFY(text(coder, QStringLiteral("consoleCardSummary")).startsWith(QStringLiteral("✓ selesai")));

    // SPECIFIER selesai: menunggu review
    m_runtime.sessions.last()->finishWith(successResult(QStringLiteral("# Spek")));
    QCOMPARE(text(spec, QStringLiteral("consoleCardStatus")), QStringLiteral("Menunggu review"));
    QCOMPARE(spec->property("state").toString(), QStringLiteral("review"));

    // Log lengkap task dibuka-tutup lewat tombol "Log"
    auto *toggle = coder->findChild<QToolButton *>(QStringLiteral("btnConsoleCardLog"));
    auto *log = coder->findChild<QPlainTextEdit *>(QStringLiteral("consoleCardLog"));
    QVERIFY(toggle && log);
    QVERIFY(log->isHidden());
    toggle->click();
    QVERIFY(!log->isHidden());
    QVERIFY(log->toPlainText().contains(QStringLiteral("[AGENT:CODER] → Write hello.txt")));
    toggle->click();
    QVERIFY(log->isHidden());

    // Klik kartu task: hasil agent task itu terbuka di drawer, sama seperti klik kartu di board
    QTest::mouseClick(spec, Qt::LeftButton, {}, QPoint(4, 4));
    ResponseDrawer *panel = drawer();
    QVERIFY(panel && !panel->isHidden());
    QCOMPARE(panel->taskId(), QStringLiteral("t3"));

    // Pesan di luar task tetap di kartu Sistem, yang ikut naik ke paling atas
    // Kolom input di bawah panel sudah dihapus: konsol hanya menampilkan notifikasi
    QVERIFY(!m_window->findChild<QLineEdit *>(QStringLiteral("lineEditPrompt")));
    console->appendLog(QStringLiteral("[SYSTEM] halo"));
    QCOMPARE(console->cards().first(), system);
    QVERIFY(system->logText().contains(QStringLiteral("[SYSTEM] halo")));
    QVERIFY(!coder->logText().contains(QStringLiteral("[SYSTEM] halo")));

    // Task dihapus: kartunya tinggal sebagai riwayat, dan kliknya hanya membuka log
    QVERIFY(m_tasks->removeTask(QStringLiteral("t1")));
    QVERIFY(coder->isRemoved());
    QCOMPARE(console->cards().first(), coder);
    QCOMPARE(text(coder, QStringLiteral("consoleCardStatus")), QStringLiteral("Dihapus"));
    QVERIFY(coder->logText().contains(QStringLiteral("[TASK DELETED] Demo/Tulis hello (CLEANER)")));
    QTest::mouseClick(coder, Qt::LeftButton, {}, QPoint(4, 4));
    QVERIFY(!log->isHidden());
    QCOMPARE(panel->taskId(), QStringLiteral("t3"));
}

void TestGui::consoleShowsLiveAsSignalIcon() {
    ConsolePanelWidget *console = consolePanel();
    QVERIFY(console);

    auto *live = console->findChild<QLabel *>(QStringLiteral("labelLiveIndicator"));
    QVERIFY(live);
    QVERIFY(live->text().isEmpty());
    QVERIFY(!live->pixmap().isNull());
    // Tanpa teks, artinya dijelaskan tooltip dan terbaca oleh pembaca layar
    QVERIFY(live->toolTip().startsWith(QStringLiteral("Live")));
    QCOMPARE(live->accessibleName(), QStringLiteral("Live"));

    const QList<QLabel *> labels = console->findChildren<QLabel *>();
    for (const QLabel *label : labels) {
        QVERIFY2(!label->text().contains(QStringLiteral("Live")), qPrintable(label->text()));
    }
}

void TestGui::stageColumnsExplainThemselves() {
    auto *swimlane = m_window->findChild<SwimlaneWidget *>();
    QVERIFY(swimlane);

    for (const QString &key : m_catalog.keys()) {
        KanbanColumnWidget *column = swimlane->column(key);
        QVERIFY2(column, qPrintable(key));
        auto *info = column->findChild<QLabel *>(QStringLiteral("labelStageInfo"));
        auto *title = column->findChild<QLabel *>(QStringLiteral("labelStageTitle"));
        QVERIFY2(info && title, qPrintable(key));

        QVERIFY2(info->isVisibleTo(column), qPrintable(key));
        QVERIFY2(!info->pixmap().isNull(), qPrintable(key));
        QCOMPARE(column->stageInfo(), StageInfo::html(m_catalog, key));
        QVERIFY2(column->stageInfo().contains(QStringLiteral("<b>%1</b>").arg(key)), qPrintable(key));
        // Tanpa tooltip Qt: itu yang muncul terlambat dan berkedip. Pembaca layar tetap dapat teksnya.
        QVERIFY2(info->toolTip().isEmpty() && title->toolTip().isEmpty(), qPrintable(key));
        QVERIFY2(info->accessibleDescription().contains(QStringLiteral("TUGAS")), qPrintable(key));
    }

    // Hover: popup muncul seketika di bawah judul
    KanbanColumnWidget *spec = swimlane->column(QStringLiteral("SPECIFIER"));
    auto *info = spec->findChild<QLabel *>(QStringLiteral("labelStageInfo"));
    auto *title = spec->findChild<QLabel *>(QStringLiteral("labelStageTitle"));
    auto *popup = spec->findChild<HoverInfoPopup *>(QStringLiteral("hoverInfoPopup"));
    QVERIFY(popup && popup->isHidden());
    QCOMPARE(popup->info(), spec->stageInfo());
    auto enter = [](QWidget *widget) {
        QEnterEvent event(QPointF(2, 2), QPointF(2, 2), widget->mapToGlobal(QPointF(2, 2)));
        QApplication::sendEvent(widget, &event);
    };
    auto leave = [](QWidget *widget) {
        QEvent event(QEvent::Leave);
        QApplication::sendEvent(widget, &event);
    };
    enter(info);
    QVERIFY(popup->isVisible());
    QVERIFY(popup->geometry().top() >= title->mapToGlobal(QPoint(0, title->height())).y());
    const QRect shown = popup->geometry();

    // Kursor menyeberang dari ikon ke judul: popup tetap di tempat, tidak ditutup-buka (berkedip)
    int hides = 0;
    struct HideCounter : QObject {
        int *count;
        explicit HideCounter(int *c) : count(c) {}
        bool eventFilter(QObject *, QEvent *event) override {
            if (event->type() == QEvent::Hide) {
                ++*count;
            }
            return false;
        }
    } hideCounter(&hides);
    popup->installEventFilter(&hideCounter);
    leave(info);
    enter(title);
    QTest::qWait(300);
    QVERIFY(popup->isVisible());
    QCOMPARE(hides, 0);
    QCOMPARE(popup->geometry(), shown);

    // Kursor pergi: popup hilang sesudah jeda singkat; klik menutupnya seketika
    leave(title);
    QTRY_VERIFY_WITH_TIMEOUT(popup->isHidden(), 1000);
    enter(title);
    QVERIFY(popup->isVisible());
    QTest::mouseClick(title, Qt::LeftButton);
    QVERIFY(popup->isHidden());
    popup->removeEventFilter(&hideCounter);

    // Kolom tanpa penjelasan tidak menampilkan ikon yang tak berisi, dan hover tidak memunculkan apa pun
    KanbanColumnWidget bare;
    auto *bareInfo = bare.findChild<QLabel *>(QStringLiteral("labelStageInfo"));
    auto *barePopup = bare.findChild<HoverInfoPopup *>(QStringLiteral("hoverInfoPopup"));
    QVERIFY(bareInfo->isHidden());
    enter(bare.findChild<QLabel *>(QStringLiteral("labelStageTitle")));
    QVERIFY(barePopup->isHidden());
    bare.setStageInfo(QStringLiteral("Penjelasan"));
    QVERIFY(bareInfo->isVisibleTo(&bare));
    QCOMPARE(bare.stageInfo(), QStringLiteral("Penjelasan"));
    bare.setStageInfo(QString());
    QVERIFY(bareInfo->isHidden());
}

void TestGui::driveModalDialog(std::function<void(QDialog *)> action) {
    m_dialogSeen = false;
    QTimer::singleShot(0, this, [this, action]() {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!dialog) {
            return;
        }
        m_dialogSeen = true;
        action(dialog);
    });
    // Pengaman: dialog yang tidak tertutup oleh action jangan sampai menggantung test
    QTimer::singleShot(5000, this, []() {
        if (QWidget *modal = QApplication::activeModalWidget()) {
            modal->close();
        }
    });
}

void TestGui::driveNextModalDialog(std::function<void(QDialog *)> action) {
    m_dialogSeen = false;
    // Timer menumpang jendela utama: ikut hilang di cleanup(), jadi tidak menyentuh test berikutnya
    auto *poll = new QTimer(m_window.get());
    poll->setInterval(10);
    connect(poll, &QTimer::timeout, this, [this, poll, action]() {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!dialog) {
            return;
        }
        poll->stop();
        poll->deleteLater();
        m_dialogSeen = true;
        action(dialog);
    });
    poll->start();

    auto *safety = new QTimer(m_window.get());
    safety->setSingleShot(true);
    connect(safety, &QTimer::timeout, this, [poll = QPointer<QTimer>(poll)]() {
        if (poll) {
            poll->stop();
        }
        if (QWidget *modal = QApplication::activeModalWidget()) {
            modal->close();
        }
    });
    safety->start(5000);
}

void TestGui::cardShowsHandCursor() {
    KanbanCardWidget *coder = card(QStringLiteral("t1"));
    QVERIFY(coder);
    QCOMPARE(coder->cursor().shape(), Qt::PointingHandCursor);

    // Anak kartu (label judul) ikut mewarisi, jadi seluruh area kartu berkursor tangan
    auto *title = coder->findChild<QLabel *>(QStringLiteral("labelTitle"));
    QVERIFY(title);
    QCOMPARE(title->cursor().shape(), Qt::PointingHandCursor);
}

void TestGui::workingDirButtonShowsIconAndCaption() {
    SwimlaneWidget lane(QStringLiteral("Icon"), m_catalog);
    auto *button = lane.findChild<QPushButton *>(QStringLiteral("btnWorkingDir"));
    QVERIFY(button);

    // Belum ada folder: ajakan memilih. Ikon SVG harus benar-benar ter-render
    QCOMPARE(button->text(), QStringLiteral("Select"));
    const QSize side = button->iconSize();
    QVERIFY(side.isValid() && side.width() > 0);
    const QImage addIcon = button->icon().pixmap(side).toImage();
    QVERIFY(!addIcon.isNull());

    // Sudah ada folder: hanya nama foldernya, dengan ikon yang berbeda
    lane.setWorkingDirectory(m_workDir.path());
    QCOMPARE(button->text(), QDir(m_workDir.path()).dirName());
    const QImage folderIcon = button->icon().pixmap(side).toImage();
    QVERIFY(!folderIcon.isNull());
    QVERIFY(folderIcon != addIcon);
}

void TestGui::doubleClickEditsTask() {
    KanbanCardWidget *coder = card(QStringLiteral("t1"));
    QVERIFY(coder);

    driveModalDialog([](QDialog *dialog) {
        QCOMPARE(dialog->windowTitle(), QStringLiteral("Edit Task"));

        // Field terisi data kartu: judul (satu baris) dan subtext (kotak prompt multi-baris)
        const QList<QLineEdit *> inputs = dialog->findChildren<QLineEdit *>(QStringLiteral("taskFormInput"));
        QCOMPARE(inputs.size(), 1);
        QCOMPARE(inputs[0]->text(), QStringLiteral("Tulis hello"));
        auto *prompt = dialog->findChild<QPlainTextEdit *>(QStringLiteral("taskFormPrompt"));
        QVERIFY(prompt);
        QVERIFY(prompt->toPlainText().isEmpty());

        // Stage dikunci: pindah stage tetap lewat drag di board
        // Urutan combo: kategori, stage, lalu model & effort SPECIFIER dan CODER
        const QList<QComboBox *> combos = dialog->findChildren<QComboBox *>(QStringLiteral("taskFormCombo"));
        QCOMPARE(combos.size(), 6);
        QCOMPARE(combos[1]->currentText(), QStringLiteral("CODER"));
        QVERIFY(!combos[1]->isEnabled());

        // Item pertama menyebut bawaan stage; task ini belum punya pilihan sendiri
        QComboBox *coderModel = combos[4];
        QComboBox *coderEffort = combos[5];
        QCOMPARE(coderModel->itemText(0), QStringLiteral("Bawaan (Sonnet)"));
        QCOMPARE(coderEffort->itemText(0), QStringLiteral("Bawaan (Medium)"));
        QCOMPARE(coderModel->currentIndex(), 0);
        coderModel->setCurrentIndex(coderModel->findData(QStringLiteral("opus")));
        coderEffort->setCurrentIndex(coderEffort->findData(QStringLiteral("high")));

        inputs[0]->setText(QStringLiteral("Tulis hello v2"));
        prompt->setPlainText(QStringLiteral("PIC: Budi"));

        auto *save = dialog->findChild<QPushButton *>(QStringLiteral("btnTaskFormCreate"));
        QVERIFY(save);
        QCOMPARE(save->text(), QStringLiteral("Simpan"));
        save->click();
    });

    // Klik dua kali di label judul: event harus naik ke kartu
    auto *title = coder->findChild<QLabel *>(QStringLiteral("labelTitle"));
    QVERIFY(title);
    QTest::mouseDClick(title, Qt::LeftButton);

    QVERIFY(m_dialogSeen);
    QCOMPARE(coder->title(), QStringLiteral("Tulis hello v2"));
    QCOMPARE(coder->subtext(), QStringLiteral("PIC: Budi"));
    QCOMPARE(coder->id(), QStringLiteral("t1"));
    QCOMPARE(coder->badge(), QStringLiteral("✓ 0"));

    SwimlaneWidget *swimlane = m_window->findChild<SwimlaneWidget *>();
    QVERIFY(swimlane);
    QCOMPARE(swimlane->stageOf(coder), QStringLiteral("CODER"));
    QVERIFY(consoleText().contains(QStringLiteral("[TASK EDITED] Demo -> CODER: 'Tulis hello v2'")));

    // Pilihan model & effort dari form ikut tersimpan di task, hanya untuk stage yang disetel
    const std::optional<TaskItem> edited = m_tasks->task(QStringLiteral("t1"));
    QVERIFY(edited);
    QCOMPARE(int(edited->tuning.size()), 1);
    QCOMPARE(edited->tuning.value(QStringLiteral("CODER")).model, QStringLiteral("opus"));
    QCOMPARE(edited->tuning.value(QStringLiteral("CODER")).effort, QStringLiteral("high"));
}

void TestGui::cancelledEditKeepsTask() {
    KanbanCardWidget *coder = card(QStringLiteral("t1"));
    QVERIFY(coder);

    driveModalDialog([](QDialog *dialog) {
        const QList<QLineEdit *> inputs = dialog->findChildren<QLineEdit *>(QStringLiteral("taskFormInput"));
        QVERIFY(!inputs.isEmpty());
        inputs[0]->setText(QStringLiteral("Tidak jadi"));
        dialog->reject();
    });

    QTest::mouseDClick(coder, Qt::LeftButton);

    QVERIFY(m_dialogSeen);
    QCOMPARE(coder->title(), QStringLiteral("Tulis hello"));
    QVERIFY(!consoleText().contains(QStringLiteral("[TASK EDITED]")));
}

void TestGui::deleteTaskFromCardMenu() {
    KanbanCardWidget *coder = card(QStringLiteral("t1"));
    QVERIFY(coder);
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const QString attachments = base + QStringLiteral("/projects/Demo/attachments/t1");
    QVERIFY(QDir().mkpath(attachments));
    QVERIFY(writeFile(attachments + QStringLiteral("/data.csv"), "a,b\n"));

    // Agent sedang berjalan dan hasilnya terbuka di drawer
    runButton(QStringLiteral("t1"))->click();
    QVERIFY(m_runtime.sessions.first()->isRunning());
    QTest::mouseClick(coder, Qt::LeftButton);
    QVERIFY(drawer() && !drawer()->isHidden());

    // Klik kanan -> "Hapus task…" -> popup konfirmasi yang ditempel di kartu
    const auto openConfirm = [this, coder]() -> QFrame * {
        QContextMenuEvent event(QContextMenuEvent::Mouse, QPoint(10, 10), coder->mapToGlobal(QPoint(10, 10)));
        QApplication::sendEvent(coder, &event);
        auto *menu = coder->findChild<QMenu *>(QStringLiteral("cardContextMenu"));
        auto *remove = menu ? menu->findChild<QAction *>(QStringLiteral("actionDeleteTask")) : nullptr;
        if (!remove) {
            return nullptr;
        }
        menu->close();
        remove->trigger();
        return coder->findChild<QFrame *>(QStringLiteral("deleteConfirmPopup"));
    };

    QFrame *popup = openConfirm();
    QVERIFY(popup);
    const QString question = popup->findChild<QLabel *>(QStringLiteral("deleteConfirmLabel"))->text();
    QVERIFY(question.contains(QStringLiteral("Hapus task \"Tulis hello\"?")));
    QVERIFY(question.contains(QStringLiteral("dihentikan")));

    // Batal: tidak ada yang berubah
    popup->findChild<QPushButton *>(QStringLiteral("btnDeleteConfirmCancel"))->click();
    QTest::qWait(50);
    QVERIFY(card(QStringLiteral("t1")));
    QVERIFY(m_runtime.sessions.first()->isRunning());

    popup = openConfirm();
    QVERIFY(popup);
    popup->findChild<QPushButton *>(QStringLiteral("btnDeleteConfirmYes"))->click();

    QTRY_VERIFY(!card(QStringLiteral("t1")));
    QVERIFY(!m_tasks->task(QStringLiteral("t1")));
    // Session agent-nya sudah dibuang setelah berhenti; cukup lihat jejaknya di konsol
    QVERIFY(consoleText().contains(QStringLiteral("[AGENT:CODER] ✗ cancelled")));
    QVERIFY(consoleText().contains(QStringLiteral("[TASK DELETED] Demo/Tulis hello (CODER)")));
    QVERIFY(!QFileInfo::exists(attachments));
    QTRY_VERIFY(drawer()->isHidden());
    QVERIFY(card(QStringLiteral("t2")) && card(QStringLiteral("t3")));

    // Buka lagi: task tidak kembali dari session.json
    m_window.reset();
    m_tasks.reset();
    createWindow();
    QVERIFY(!card(QStringLiteral("t1")));
    QVERIFY(card(QStringLiteral("t2")) && card(QStringLiteral("t3")));
}

void TestGui::moveTaskFromCardMenu() {
    SwimlaneWidget *swimlane = m_window->findChild<SwimlaneWidget *>();
    KanbanCardWidget *coder = card(QStringLiteral("t1"));
    KanbanCardWidget *spec = card(QStringLiteral("t3"));
    QVERIFY(swimlane && coder && spec);

    // Klik kanan -> "Pindah ke stage" -> pilih stage; menu dibuang setelah dipakai
    const auto moveVia = [](KanbanCardWidget *target, const QString &stage) -> bool {
        QContextMenuEvent event(QContextMenuEvent::Mouse, QPoint(10, 10), target->mapToGlobal(QPoint(10, 10)));
        QApplication::sendEvent(target, &event);
        auto *menu = target->findChild<QMenu *>(QStringLiteral("cardContextMenu"));
        auto *move = menu ? menu->findChild<QMenu *>(QStringLiteral("cardMoveMenu")) : nullptr;
        QAction *action = move ? move->findChild<QAction *>(QStringLiteral("actionMoveTo_") + stage) : nullptr;
        const bool triggered = action && move->isEnabled() && action->isEnabled();
        if (triggered) {
            action->trigger();
        }
        delete menu;
        return triggered;
    };

    // Submenu memuat semua stage; stage sekarang bertanda ✓ dan tidak bisa dipilih
    {
        QContextMenuEvent event(QContextMenuEvent::Mouse, QPoint(10, 10), coder->mapToGlobal(QPoint(10, 10)));
        QApplication::sendEvent(coder, &event);
        auto *menu = coder->findChild<QMenu *>(QStringLiteral("cardContextMenu"));
        QVERIFY(menu);
        auto *move = menu->findChild<QMenu *>(QStringLiteral("cardMoveMenu"));
        QVERIFY(move && move->isEnabled());
        QCOMPARE(move->actions().size(), swimlane->columns().size());
        QAction *current = move->findChild<QAction *>(QStringLiteral("actionMoveTo_CODER"));
        QVERIFY(current && current->isChecked() && !current->isEnabled());
        delete menu;
    }

    // Maju ke CLEANER: kartu pindah kolom, task ikut, konsol mencatatnya
    QVERIFY(moveVia(coder, QStringLiteral("CLEANER")));
    QCOMPARE(swimlane->stageOf(coder), QStringLiteral("CLEANER"));
    QCOMPARE(coder->stage(), QStringLiteral("CLEANER"));
    QCOMPARE(m_tasks->task(QStringLiteral("t1"))->stage, QStringLiteral("CLEANER"));
    QVERIFY(consoleText().contains(QStringLiteral("[TASK] Demo/Tulis hello CODER → CLEANER")));

    // Mundur selalu boleh
    QVERIFY(moveVia(coder, QStringLiteral("WAITING")));
    QCOMPARE(swimlane->stageOf(coder), QStringLiteral("WAITING"));
    QCOMPARE(m_tasks->task(QStringLiteral("t1"))->stage, QStringLiteral("WAITING"));

    // Gate tetap berlaku: SPECIFIER belum disetujui, jadi tidak bisa maju
    QVERIFY(moveVia(spec, QStringLiteral("CODER")));
    QCOMPARE(swimlane->stageOf(spec), QStringLiteral("SPECIFIER"));
    QCOMPARE(m_tasks->task(QStringLiteral("t3"))->stage, QStringLiteral("SPECIFIER"));
    QVERIFY(consoleText().contains(
        QStringLiteral("[GATE] Demo/Sederhanakan landing tidak bisa dipindah: stage SPECIFIER butuh review")));

    // Agent sedang berjalan: submenu dikunci, sama seperti drag
    runButton(QStringLiteral("t3"))->click();
    QVERIFY(spec->runState() == RunState::Running);
    QVERIFY(!moveVia(spec, QStringLiteral("WAITING")));
    QCOMPARE(m_tasks->task(QStringLiteral("t3"))->stage, QStringLiteral("SPECIFIER"));
}

void TestGui::specifierReviewApproveFlow() {
    KanbanCardWidget *spec = card(QStringLiteral("t3"));
    QPushButton *button = runButton(QStringLiteral("t3"));
    QVERIFY(spec && button);

    finishSpecifierRun(QStringLiteral("# Spesifikasi\n\n| Blok | Aksi |\n|---|---|\n| Kalkulator | Hapus |\n\n"
                                      "## Pertanyaan terbuka\n1. Newsletter masih dipakai?"));

    // Stage ber-gate: berhenti untuk review, tombol berubah jadi 📋
    QVERIFY(spec->taskState() == TaskState::AwaitingReview);
    QCOMPARE(spec->property("state").toString(), QStringLiteral("review"));
    QVERIFY(button->toolTip().startsWith(QStringLiteral("Review")));
    QVERIFY(consoleText().contains(QStringLiteral("[GATE] Demo/Sederhanakan landing menunggu review di SPECIFIER")));

    button->click();
    ResponseDrawer *panel = drawer();
    QVERIFY(panel && !panel->isHidden());
    QCOMPARE(panel->taskId(), QStringLiteral("t3"));
    auto *view = panel->findChild<QTextBrowser *>(QStringLiteral("markdownView"));
    QVERIFY(view);
    QVERIFY(view->toPlainText().contains(QStringLiteral("Kalkulator")));
    auto *reviewPanel = panel->findChild<QWidget *>(QStringLiteral("drawerReviewPanel"));
    QVERIFY(reviewPanel && !reviewPanel->isHidden());
    auto *metrics = panel->findChild<QLabel *>(QStringLiteral("drawerMetrics"));
    QVERIFY(metrics->text().contains(QStringLiteral("1m 24s · 302k tok · $0.87")));

    auto *approve = panel->findChild<QPushButton *>(QStringLiteral("btnDrawerApprove"));
    QCOMPARE(approve->text(), QStringLiteral("Setujui → CODER"));
    panel->findChild<QPlainTextEdit *>(QStringLiteral("drawerNote"))->setPlainText(QStringLiteral("Newsletter tetap dipakai"));
    approve->click();

    SwimlaneWidget *swimlane = m_window->findChild<SwimlaneWidget *>();
    QCOMPARE(swimlane->stageOf(spec), QStringLiteral("CODER"));
    QCOMPARE(spec->badge(), QStringLiteral("✓ 1"));
    QVERIFY(spec->taskState() == TaskState::Idle);
    QVERIFY(reviewPanel->isHidden());
    QVERIFY(consoleText().contains(
        QStringLiteral("[GATE] Demo/Sederhanakan landing SPECIFIER disetujui → CODER (dengan catatan)")));

    // CODER menerima spesifikasi yang disetujui beserta catatannya
    button->click();
    const QString prompt = m_runtime.sessions.last()->launch().prompt;
    QVERIFY(prompt.contains(QStringLiteral("# Spesifikasi yang disetujui (SPECIFIER)")));
    QVERIFY(prompt.contains(QStringLiteral("| Kalkulator | Hapus |")));
    QVERIFY(prompt.contains(QStringLiteral("Catatan saat disetujui:\nNewsletter tetap dipakai")));
    QVERIFY(consoleText().contains(QStringLiteral("+ Spesifikasi yang disetujui (SPECIFIER)")));
}

void TestGui::revisionRerunsWithPreviousDocument() {
    finishSpecifierRun(QStringLiteral("Spek v1"));
    runButton(QStringLiteral("t3"))->click();   // 📋
    ResponseDrawer *panel = drawer();
    QVERIFY(panel && !panel->isHidden());

    // Revisi tanpa catatan ditolak di drawer, tidak ada run baru
    auto *revise = panel->findChild<QPushButton *>(QStringLiteral("btnDrawerRevise"));
    revise->click();
    QCOMPARE(int(m_runtime.sessions.size()), 1);
    QVERIFY(!panel->findChild<QLabel *>(QStringLiteral("drawerNoteHint"))->isHidden());

    panel->findChild<QPlainTextEdit *>(QStringLiteral("drawerNote"))->setPlainText(QStringLiteral("Kalkulator jangan dihapus"));
    revise->click();

    // Revisi langsung menjalankan SPECIFIER lagi dengan dokumen lama + catatan
    QCOMPARE(int(m_runtime.sessions.size()), 2);
    const QString prompt = m_runtime.sessions.last()->launch().prompt;
    QVERIFY(prompt.contains(QStringLiteral("# Dokumen sebelumnya (untuk direvisi)\n\nSpek v1")));
    QVERIFY(prompt.contains(QStringLiteral("Catatan revisi:\nKalkulator jangan dihapus")));
    QVERIFY(card(QStringLiteral("t3"))->runState() == RunState::Running);
    QVERIFY(consoleText().contains(QStringLiteral("[GATE] Demo/Sederhanakan landing revisi diminta")));
}

void TestGui::dragPastGateIsRejected() {
    KanbanCardWidget *spec = card(QStringLiteral("t3"));
    SwimlaneWidget *swimlane = m_window->findChild<SwimlaneWidget *>();
    KanbanColumnWidget *coderColumn = swimlane->column(QStringLiteral("CODER"));
    QVERIFY(spec && coderColumn);

    // Sama seperti drop: kolom menerima widget dulu, lalu memancarkan cardDropped
    coderColumn->insertCard(0, spec);
    emit coderColumn->cardDropped(spec, QStringLiteral("CODER"), 0);

    QCOMPARE(swimlane->stageOf(spec), QStringLiteral("SPECIFIER"));
    QCOMPARE(m_tasks->task(QStringLiteral("t3"))->stage, QStringLiteral("SPECIFIER"));
    QVERIFY(consoleText().contains(
        QStringLiteral("[GATE] Demo/Sederhanakan landing tidak bisa dipindah: stage SPECIFIER butuh review")));
}

void TestGui::drawerShowsLiveOutput() {
    runButton(QStringLiteral("t3"))->click();
    KanbanCardWidget *spec = card(QStringLiteral("t3"));
    QTest::mouseClick(spec, Qt::LeftButton);   // klik biasa pada kartu yang sedang berjalan

    ResponseDrawer *panel = drawer();
    QVERIFY(panel && !panel->isHidden());
    auto *selector = panel->findChild<QComboBox *>(QStringLiteral("drawerRunSelector"));
    QCOMPARE(selector->currentText(), QStringLiteral("● Live"));

    FakeAgentSession *session = m_runtime.sessions.last();
    AgentEvent text;
    text.kind = AgentEvent::Kind::Text;
    text.text = QStringLiteral("Membaca landing page dulu");
    session->emitEvent(text);
    AgentEvent tool;
    tool.kind = AgentEvent::Kind::ToolUse;
    tool.toolName = QStringLiteral("Read");
    tool.toolDetail = QDir(m_workDir.path()).filePath(QStringLiteral("app/Views/welcome_message.php"));
    session->emitEvent(tool);

    auto *view = panel->findChild<QTextBrowser *>(QStringLiteral("markdownView"));
    QVERIFY(view->toPlainText().contains(QStringLiteral("Membaca landing page dulu")));
    QVERIFY(view->toPlainText().contains(QStringLiteral("→ Read app/Views/welcome_message.php")));

    // Run selesai: drawer beralih dari Live ke hasil yang tersimpan, panel keputusan muncul
    session->finishWith(successResult(QStringLiteral("# Spesifikasi final")));
    QVERIFY(selector->currentText().startsWith(QStringLiteral("SPECIFIER #1")));
    QVERIFY(view->toPlainText().contains(QStringLiteral("Spesifikasi final")));
    QVERIFY(!panel->findChild<QWidget *>(QStringLiteral("drawerReviewPanel"))->isHidden());
}

void TestGui::drawerExpandFillsBoardAndRestores() {
    finishSpecifierRun(QStringLiteral("# Spek"));
    runButton(QStringLiteral("t3"))->click();   // 📋 membuka drawer

    ResponseDrawer *panel = drawer();
    auto *splitter = m_window->findChild<QSplitter *>(QStringLiteral("boardSplitter"));
    auto *expand = panel->findChild<QPushButton *>(QStringLiteral("btnDrawerExpand"));
    auto *close = panel->findChild<QPushButton *>(QStringLiteral("btnDrawerClose"));
    QVERIFY(splitter && expand && close);
    QWidget *board = splitter->widget(0);
    QVERIFY(!panel->isExpanded());
    QCOMPARE(expand->toolTip(), QStringLiteral("Perluas"));

    // Tunggu animasi buka selesai dan ingat lebar normalnya
    QTest::qWait(400);
    const int normalWidth = panel->width();
    QVERIFY(normalWidth >= 360 && normalWidth < splitter->width());

    // Expand: drawer memenuhi splitter, board tersembunyi, ikon jadi "kembalikan"
    expand->click();
    QVERIFY(panel->isExpanded());
    QCOMPARE(expand->toolTip(), QStringLiteral("Kembalikan ukuran"));
    QTRY_VERIFY(board->isHidden());
    QTRY_COMPARE(panel->width(), splitter->width());

    // Klik lagi: board muncul, drawer kembali ke lebar semula
    expand->click();
    QVERIFY(!panel->isExpanded());
    QCOMPARE(expand->toolTip(), QStringLiteral("Perluas"));
    QTRY_VERIFY(!board->isHidden());
    QTRY_VERIFY(qAbs(panel->width() - normalWidth) <= 2);

    // Ditutup saat sedang expand: board kembali, tombol kembali ke ikon "perluas",
    // dan pembukaan berikutnya memakai lebar sebelum expand (bukan lebar penuh)
    expand->click();
    QTRY_VERIFY(board->isHidden());
    close->click();
    QTRY_VERIFY(panel->isHidden());
    QVERIFY(!board->isHidden());
    QVERIFY(!panel->isExpanded());
    QCOMPARE(expand->toolTip(), QStringLiteral("Perluas"));

    runButton(QStringLiteral("t3"))->click();
    QVERIFY(!panel->isHidden());
    QTest::qWait(400);
    QVERIFY(!board->isHidden());
    QVERIFY(qAbs(panel->width() - normalWidth) <= 2);
}

void TestGui::reviewSurvivesRestart() {
    finishSpecifierRun(QStringLiteral("# Spek tersimpan"));

    // Tutup dan buka lagi: status Review dan riwayat run dibaca dari session.json
    m_window.reset();
    m_tasks.reset();
    createWindow();

    KanbanCardWidget *spec = card(QStringLiteral("t3"));
    QVERIFY(spec);
    QVERIFY(spec->taskState() == TaskState::AwaitingReview);
    QCOMPARE(spec->property("state").toString(), QStringLiteral("review"));

    runButton(QStringLiteral("t3"))->click();
    ResponseDrawer *panel = drawer();
    QVERIFY(panel && !panel->isHidden());
    QVERIFY(panel->findChild<QComboBox *>(QStringLiteral("drawerRunSelector"))
                ->currentText().startsWith(QStringLiteral("SPECIFIER #1")));
    QVERIFY(panel->findChild<QTextBrowser *>(QStringLiteral("markdownView"))
                ->toPlainText().contains(QStringLiteral("Spek tersimpan")));
}

void TestGui::markdownViewRendersMermaid() {
    FakeMermaidRenderer renderer;
    MarkdownView view(&renderer);
    const QString code = QStringLiteral("graph TD; A-->B");
    view.showMarkdown(QStringLiteral("# Alur\n\n```mermaid\n%1\n```\n\nSelesai.").arg(code));

    // Blok Mermaid diminta ke renderer sekali, sementara itu tampil placeholder
    QCOMPARE(renderer.requests, QStringList({code}));
    QVERIFY(view.toPlainText().contains(QStringLiteral("Merender diagram")));

    QImage image(120, 80, QImage::Format_ARGB32);
    image.fill(Qt::red);
    renderer.complete(code, image);
    QVERIFY(!view.toPlainText().contains(QStringLiteral("Merender diagram")));
    QVERIFY(view.document()->toHtml().contains(QStringLiteral("mermaid://") + MermaidRenderer::keyFor(code)));
    QVERIFY(view.toPlainText().contains(QStringLiteral("Selesai.")));

    // Gagal: kode tetap tampil beserta alasannya
    const QString broken = QStringLiteral("graph TD; A-->");
    view.showMarkdown(QStringLiteral("```mermaid\n%1\n```").arg(broken));
    renderer.fail(broken, QStringLiteral("Syntax error"));
    QVERIFY(view.toPlainText().contains(QStringLiteral("Diagram tidak bisa dirender: Syntax error")));
    QVERIFY(view.toPlainText().contains(broken));

    // Tanpa renderer: blok tetap tampil sebagai kode
    MarkdownView plain(nullptr);
    plain.showMarkdown(QStringLiteral("```mermaid\n%1\n```").arg(code));
    QVERIFY(plain.toPlainText().contains(code));
}

void TestGui::markdownViewOpensZoomableDiagram() {
    FakeMermaidRenderer renderer;
    MarkdownView view(&renderer);
    view.resize(420, 600);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));

    const QString code = QStringLiteral("sequenceDiagram\n  Kasir->>OrderService: Place");
    const QString markdown = QStringLiteral("# Alur\n\n```mermaid\n%1\n```\n\nSelesai.").arg(code);
    view.showMarkdown(markdown);
    // Diagram 1600 × 800 logis (piksel 2×): lebih lebar dari panel, jadi diperkecil + keterangan
    QImage image(3200, 1600, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    image.setDevicePixelRatio(2.0);
    renderer.complete(code, image);
    QVERIFY(view.toPlainText().contains(QStringLiteral("Klik diagram untuk membukanya di penampil zoom")));

    // Klik gambar diagram seperti pengguna
    int imageAt = -1;
    for (QTextBlock block = view.document()->begin(); block.isValid() && imageAt < 0; block = block.next()) {
        for (auto part = block.begin(); !part.atEnd(); ++part) {
            if (part.fragment().charFormat().isImageFormat()) {
                imageAt = part.fragment().position();
                break;
            }
        }
    }
    QVERIFY(imageAt >= 0);
    QTextCursor cursor(view.document());
    cursor.setPosition(imageAt);
    const QRect caret = view.cursorRect(cursor);
    const QPoint onImage(caret.left() + 40, caret.center().y());
    // Importer Markdown Qt membuang link di sekeliling gambar; MarkdownView memasangnya lagi
    QCOMPARE(view.anchorAt(onImage), QStringLiteral("mermaid://") + MermaidRenderer::keyFor(code));
    QTest::mouseClick(view.viewport(), Qt::LeftButton, {}, onImage);

    const QList<DiagramViewer *> viewers = view.findChildren<DiagramViewer *>();
    QCOMPARE(viewers.size(), 1);
    DiagramViewer *viewer = viewers.first();
    QVERIFY(QTest::qWaitForWindowExposed(viewer));
    QCOMPARE(viewer->windowTitle(), QStringLiteral("Diagram sequence"));
    QCOMPARE(viewer->key(), MermaidRenderer::keyFor(code));
    // Lebih besar dari jendela penampil: mulai dipaskan
    QVERIFY(viewer->fitsToWindow());
    QVERIFY(viewer->zoom() < 1.0);

    // Klik lagi: jendela yang sama dimunculkan, bukan jendela kedua
    QTest::mouseClick(view.viewport(), Qt::LeftButton, {}, onImage);
    QCOMPARE(view.findChildren<DiagramViewer *>().size(), 1);

    // Dokumen tanpa diagram melepas gambarnya; saat tampil lagi, diagram diminta ulang ke renderer.
    // Penampil yang terbuka tetap memegang salinannya sendiri.
    view.showMarkdown(QStringLiteral("Tanpa diagram."));
    view.showMarkdown(markdown);
    QCOMPARE(int(renderer.requests.count(code)), 2);
    QVERIFY(view.toPlainText().contains(QStringLiteral("Merender diagram")));
    QVERIFY(viewer->isVisible());

    // Judul penampil dari jenis diagram; komentar dan front matter dilewati
    QCOMPARE(DiagramViewer::titleFor(QStringLiteral("classDiagram\n  class Order")), QStringLiteral("Diagram kelas"));
    QCOMPARE(DiagramViewer::titleFor(QStringLiteral("%%{init: {'theme': 'base'}}%%\nsequenceDiagram\n  A->>B: x")),
             QStringLiteral("Diagram sequence"));
    QCOMPARE(DiagramViewer::titleFor(QStringLiteral("---\ntitle: Alur\n---\nflowchart LR\n  A-->B")),
             QStringLiteral("Flowchart"));
    QCOMPARE(DiagramViewer::titleFor(QStringLiteral("pie title Biaya\n  \"A\": 1")), QStringLiteral("Diagram"));
}

void TestGui::diagramViewerZoomsAndPans() {
    // Diagram 1500 × 600 logis (piksel 2×) di jendela 800 × 600
    QImage image(3000, 1200, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    image.setDevicePixelRatio(2.0);
    QPointer<DiagramViewer> viewer = new DiagramViewer(QStringLiteral("k1"), image, QStringLiteral("Diagram kelas"));
    auto closeViewer = qScopeGuard([&viewer]() { delete viewer.data(); });
    viewer->resize(800, 600);
    viewer->show();
    viewer->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(viewer));

    auto *canvas = viewer->findChild<QGraphicsView *>(QStringLiteral("diagramCanvas"));
    auto *level = viewer->findChild<QLabel *>(QStringLiteral("diagramZoomLevel"));
    auto *fit = viewer->findChild<QPushButton *>(QStringLiteral("btnDiagramFit"));
    auto *actual = viewer->findChild<QPushButton *>(QStringLiteral("btnDiagramActualSize"));
    auto *zoomIn = viewer->findChild<QPushButton *>(QStringLiteral("btnDiagramZoomIn"));
    QVERIFY(canvas && level && fit && actual && zoomIn);
    auto percent = [&viewer]() { return QStringLiteral("%1%").arg(qRound(viewer->zoom() * 100)); };

    // Lebih besar dari jendela: mulai Paskan, seluruh diagram terlihat tanpa perlu digeser
    QVERIFY(viewer->fitsToWindow());
    QCOMPARE(viewer->zoom(), viewer->fitZoom());
    QVERIFY(viewer->zoom() < 0.6);
    QCOMPARE(level->text(), percent());
    QVERIFY(fit->isChecked());
    QVERIFY(!actual->isChecked());
    QCOMPARE(canvas->horizontalScrollBar()->maximum(), 0);
    QCOMPARE(canvas->verticalScrollBar()->maximum(), 0);

    // Tombol +: anak tangga zoom berikutnya, keluar dari mode Paskan
    const qreal fitted = viewer->zoom();
    zoomIn->click();
    QVERIFY(viewer->zoom() > fitted);
    QVERIFY(!viewer->fitsToWindow());
    QVERIFY(!fit->isChecked());
    QCOMPARE(level->text(), percent());

    // 100% = ukuran asli; diagram lebih besar dari jendela jadi bisa digeser
    actual->click();
    QCOMPARE(viewer->zoom(), 1.0);
    QVERIFY(actual->isChecked());
    QCOMPARE(level->text(), QStringLiteral("100%"));
    QVERIFY(canvas->horizontalScrollBar()->maximum() > 0);
    QVERIFY(canvas->verticalScrollBar()->maximum() > 0);

    // Ctrl + scroll (juga pinch touchpad): zoom di titik kursor, titik diagram di bawahnya tetap
    const QPoint pointer(200, 150);
    const QPointF under = canvas->mapToScene(pointer);
    QWheelEvent pinch(QPointF(pointer), QPointF(canvas->viewport()->mapToGlobal(pointer)), QPoint(), QPoint(0, 120),
                      Qt::NoButton, Qt::ControlModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(canvas->viewport(), &pinch);
    QVERIFY(qAbs(viewer->zoom() - 1.2) < 1e-9);
    const QPointF still = canvas->mapToScene(pointer);
    QVERIFY2(qAbs(still.x() - under.x()) < 2 && qAbs(still.y() - under.y()) < 2,
             qPrintable(QStringLiteral("(%1, %2) -> (%3, %4)").arg(under.x()).arg(under.y()).arg(still.x()).arg(still.y())));

    // Scroll biasa menggeser, bukan zoom
    canvas->verticalScrollBar()->setValue(0);
    QWheelEvent scroll(QPointF(pointer), QPointF(canvas->viewport()->mapToGlobal(pointer)), QPoint(), QPoint(0, -120),
                       Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(canvas->viewport(), &scroll);
    QVERIFY(qAbs(viewer->zoom() - 1.2) < 1e-9);
    QVERIFY(canvas->verticalScrollBar()->value() > 0);

    // Seret ke kiri: diagram bergeser, yang tampil bagian kanannya
    canvas->horizontalScrollBar()->setValue(0);
    QTest::mousePress(canvas->viewport(), Qt::LeftButton, {}, QPoint(400, 300));
    QTest::mouseMove(canvas->viewport(), QPoint(300, 300));
    QTest::mouseRelease(canvas->viewport(), Qt::LeftButton, {}, QPoint(300, 300));
    QCOMPARE(canvas->horizontalScrollBar()->value(), 100);

    // Klik ganda: Paskan; klik ganda lagi: 100% di titik yang diklik
    QTest::mouseDClick(canvas->viewport(), Qt::LeftButton, {}, QPoint(300, 200));
    QVERIFY(viewer->fitsToWindow());
    QTest::mouseDClick(canvas->viewport(), Qt::LeftButton, {}, QPoint(300, 200));
    QCOMPARE(viewer->zoom(), 1.0);
    QVERIFY(!viewer->fitsToWindow());

    // Pintasan keyboard
    QTest::keyClick(canvas, Qt::Key_0, Qt::ControlModifier);
    QVERIFY(viewer->fitsToWindow());
    QTest::keyClick(canvas, Qt::Key_1, Qt::ControlModifier);
    QCOMPARE(viewer->zoom(), 1.0);
    QTest::keyClick(canvas, Qt::Key_Equal, Qt::ControlModifier);
    QCOMPARE(viewer->zoom(), 1.25);
    QTest::keyClick(canvas, Qt::Key_Minus, Qt::ControlModifier);
    QCOMPARE(viewer->zoom(), 1.0);

    // Mode Paskan ikut ukuran jendela
    viewer->fitToWindow();
    const qreal before = viewer->zoom();
    viewer->resize(1100, 760);
    QTRY_VERIFY(viewer->zoom() > before);
    QCOMPARE(viewer->zoom(), viewer->fitZoom());
    QVERIFY(viewer->fitsToWindow());

    // Batas atas 400%: tombol + nonaktif
    for (int i = 0; i < 20 && zoomIn->isEnabled(); ++i) {
        zoomIn->click();
    }
    QCOMPARE(viewer->zoom(), 4.0);
    QVERIFY(!zoomIn->isEnabled());
    QCOMPARE(level->text(), QStringLiteral("400%"));

    // Esc menutup dan menghapus jendela
    QTest::keyClick(canvas, Qt::Key_Escape);
    QTRY_VERIFY(viewer.isNull());
}

void TestGui::diagramViewerKeepsSmallDiagramAtActualSize() {
    QImage image(240, 120, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    QPointer<DiagramViewer> viewer = new DiagramViewer(QStringLiteral("k2"), image, QStringLiteral("Flowchart"));
    auto closeViewer = qScopeGuard([&viewer]() { delete viewer.data(); });
    viewer->show();
    QVERIFY(QTest::qWaitForWindowExposed(viewer));

    // Muat di jendela (minimal 720 × 480): tampil 100%, tidak diperbesar otomatis
    QVERIFY(viewer->width() >= 720 && viewer->height() >= 480);
    QCOMPARE(viewer->zoom(), 1.0);
    QVERIFY(!viewer->fitsToWindow());
    QVERIFY(viewer->findChild<QPushButton *>(QStringLiteral("btnDiagramActualSize"))->isChecked());

    // Paskan memperbesar diagram kecil sampai memenuhi jendela
    viewer->fitToWindow();
    QVERIFY(viewer->zoom() > 1.0);
    QCOMPARE(viewer->zoom(), viewer->fitZoom());

    // − dari Paskan turun ke anak tangga di bawahnya
    const qreal fitted = viewer->zoom();
    viewer->zoomOut();
    QVERIFY(viewer->zoom() < fitted);
    QVERIFY(!viewer->fitsToWindow());
}

void TestGui::architectDrawerShowsCodeChanges() {
    if (QStandardPaths::findExecutable(QStringLiteral("git")).isEmpty()) {
        QSKIP("git tidak ada di PATH");
    }

    // Folder kerja project Demo dijadikan repository: dua file diubah (satu kode), satu file baru
    const QDir dir(m_workDir.path());
    auto removeRepository = qScopeGuard([dir]() {
        QDir(dir.filePath(QStringLiteral(".git"))).removeRecursively();
        QFile::remove(dir.filePath(QStringLiteral("hello.txt")));
        QFile::remove(dir.filePath(QStringLiteral("hitung.py")));
        QFile::remove(dir.filePath(QStringLiteral("baru.txt")));
    });
    QVERIFY(runGit(dir.path(), {QStringLiteral("init"), QStringLiteral("-q")}));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("hello.txt")), "a\nb\n"));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("hitung.py")), "def hitung(x):\n    return x * 2\n"));
    QVERIFY(runGit(dir.path(), {QStringLiteral("add"), QStringLiteral("-A")}));
    QVERIFY(runGit(dir.path(), {QStringLiteral("commit"), QStringLiteral("-q"), QStringLiteral("-m"), QStringLiteral("awal")}));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("hello.txt")), "a\nc\n"));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("hitung.py")),
                      "def hitung(x):\n    if x > 1:\n        return x * 2\n    return 0\n"));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("baru.txt")), "x\n"));

    TaskItem task;
    task.id = QStringLiteral("t4");
    task.projectId = QStringLiteral("Demo");
    task.stage = QStringLiteral("ARCHITECT");
    task.category = QStringLiteral("utility");
    task.title = QStringLiteral("Tinjau struktur");
    m_tasks->addTask(task);
    KanbanCardWidget *architect = card(QStringLiteral("t4"));
    QVERIFY(architect);

    // Belum pernah di-run, tapi di stage peninjauan kode drawer tetap terbuka di tab perubahan kode
    QTest::mouseClick(architect, Qt::LeftButton);
    ResponseDrawer *panel = drawer();
    QVERIFY(panel && !panel->isHidden());
    QCOMPARE(panel->taskId(), QStringLiteral("t4"));
    auto *tabs = panel->findChild<QTabBar *>(QStringLiteral("drawerTabs"));
    QVERIFY(tabs && !tabs->isHidden());
    QCOMPARE(tabs->currentIndex(), 1);
    // Stage peninjauan: hasil agent, perubahan kode, maintainability, dan UML semuanya tampil
    for (int tab = 0; tab < tabs->count(); ++tab) {
        QVERIFY(tabs->isTabVisible(tab));
    }

    // git dibaca di thread pool; hasilnya menyusul
    auto *files = panel->findChild<QTreeWidget *>(QStringLiteral("diffFileList"));
    QVERIFY(files);
    QTRY_COMPARE(files->topLevelItemCount(), 3);
    QCOMPARE(tabs->tabText(1), QStringLiteral("Perubahan kode · 3"));
    QCOMPARE(files->topLevelItem(0)->text(1), QStringLiteral("baru.txt"));
    QCOMPARE(files->topLevelItem(1)->text(0), QStringLiteral("M"));
    QCOMPARE(files->topLevelItem(1)->text(1), QStringLiteral("hello.txt"));
    QCOMPARE(files->topLevelItem(1)->text(2), QStringLiteral("+1"));
    QCOMPARE(files->topLevelItem(1)->text(3), QStringLiteral("−1"));
    QCOMPARE(files->topLevelItem(2)->text(1), QStringLiteral("hitung.py"));
    QVERIFY(panel->findChild<QLabel *>(QStringLiteral("diffSummary"))
                ->text().startsWith(QStringLiteral("3 file berubah · +5 −2 · dibanding commit ")));
    const QString diffText = panel->findChild<QPlainTextEdit *>(QStringLiteral("diffText"))->toPlainText();
    QVERIFY(diffText.contains(QStringLiteral(" M  hello.txt")));
    QVERIFY(diffText.contains(QStringLiteral("2   - b")));
    QVERIFY(diffText.contains(QStringLiteral("  2 + c")));

    // Tab Maintainability: hanya file kode yang diukur; percabangan baru menurunkan skornya
    QVERIFY(tabs->tabText(2).startsWith(QStringLiteral("Maintainability · ")));
    auto *scores = panel->findChild<QTreeWidget *>(QStringLiteral("miFileList"));
    QVERIFY(scores);
    QCOMPARE(scores->topLevelItemCount(), 1);
    QTreeWidgetItem *hitung = scores->topLevelItem(0);
    QCOMPARE(hitung->text(0), QStringLiteral("hitung.py"));
    QVERIFY(hitung->text(1).toInt() > hitung->text(2).toInt());
    QVERIFY(hitung->text(3).startsWith(QStringLiteral("−")));
    const QString score = panel->findChild<QLabel *>(QStringLiteral("miScore"))->text();
    QCOMPARE(score, hitung->text(2));
    QCOMPARE(tabs->tabText(2), QStringLiteral("Maintainability · %1").arg(score));
    QVERIFY(panel->findChild<QLabel *>(QStringLiteral("miChange"))->text().startsWith(QStringLiteral("Turun ")));
    QVERIFY(panel->findChild<QLabel *>(QStringLiteral("miLegend"))->text().contains(QStringLiteral("2 file lain tidak diukur")));
    // Tanpa file C#, kolom DIT dan Coupling disembunyikan
    QVERIFY(scores->isColumnHidden(5));
    QVERIFY(scores->isColumnHidden(6));

    // Muat ulang membaca keadaan folder terbaru
    QVERIFY(QFile::remove(dir.filePath(QStringLiteral("baru.txt"))));
    panel->findChild<QPushButton *>(QStringLiteral("btnDiffRefresh"))->click();
    QTRY_COMPARE(files->topLevelItemCount(), 2);
    QCOMPARE(tabs->tabText(1), QStringLiteral("Perubahan kode · 2"));

    // Task di stage lain: tab disembunyikan, drawer kembali ke hasil agent
    finishSpecifierRun(QStringLiteral("# Spek"));
    runButton(QStringLiteral("t3"))->click();
    QCOMPARE(panel->taskId(), QStringLiteral("t3"));
    QVERIFY(tabs->isHidden());
    QVERIFY(panel->findChild<QTextBrowser *>(QStringLiteral("markdownView"))->isVisible());
}

void TestGui::coderDrawerShowsCodeChanges() {
    if (QStandardPaths::findExecutable(QStringLiteral("git")).isEmpty()) {
        QSKIP("git tidak ada di PATH");
    }
    const GitSandbox sandbox;

    const QDir dir(m_workDir.path());
    auto removeRepository = qScopeGuard([dir]() {
        QDir(dir.filePath(QStringLiteral(".git"))).removeRecursively();
        QFile::remove(dir.filePath(QStringLiteral("hello.txt")));
        QFile::remove(dir.filePath(QStringLiteral("dua.txt")));
        QFile::remove(dir.filePath(QStringLiteral("tiga.txt")));
    });
    QVERIFY(runGit(dir.path(), {QStringLiteral("init"), QStringLiteral("-q")}));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("hello.txt")), "a\nb\n"));
    QVERIFY(runGit(dir.path(), {QStringLiteral("add"), QStringLiteral("-A")}));
    QVERIFY(runGit(dir.path(), {QStringLiteral("commit"), QStringLiteral("-q"), QStringLiteral("-m"), QStringLiteral("awal")}));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("hello.txt")), "a\nc\n"));

    // t1 adalah kartu CODER yang belum pernah dijalankan; drawer tetap terbuka di tab perubahan kode
    QTest::mouseClick(card(QStringLiteral("t1")), Qt::LeftButton);
    ResponseDrawer *panel = drawer();
    QVERIFY(panel && !panel->isHidden());
    QCOMPARE(panel->taskId(), QStringLiteral("t1"));
    auto *tabs = panel->findChild<QTabBar *>(QStringLiteral("drawerTabs"));
    QVERIFY(tabs && !tabs->isHidden());
    QCOMPARE(tabs->currentIndex(), 1);
    // CODER menulis kode, bukan meninjaunya: hanya hasil agent dan perubahan kode
    QVERIFY(tabs->isTabVisible(0));
    QVERIFY(tabs->isTabVisible(1));
    QVERIFY(!tabs->isTabVisible(2));
    QVERIFY(!tabs->isTabVisible(3));

    auto *files = panel->findChild<QTreeWidget *>(QStringLiteral("diffFileList"));
    QVERIFY(files);
    QTRY_COMPARE(files->topLevelItemCount(), 1);
    QCOMPARE(tabs->tabText(1), QStringLiteral("Perubahan kode · 1"));
    QCOMPARE(files->topLevelItem(0)->text(1), QStringLiteral("hello.txt"));
    QCOMPARE(files->topLevelItem(0)->text(2), QStringLiteral("+1"));
    QCOMPARE(files->topLevelItem(0)->text(3), QStringLiteral("−1"));
    // Yang tidak ditampilkan tidak dikerjakan: tanpa pengukuran kode dan tanpa render diagram
    QCOMPARE(tabs->tabText(2), QStringLiteral("Maintainability"));
    QCOMPARE(tabs->tabText(3), QStringLiteral("UML"));
    QVERIFY(m_mermaid.requests.isEmpty());

    // Run dimulai: branch + worktree task disiapkan dulu (git di thread pool), lalu drawer beralih ke
    // output Live. Agent bekerja di worktree, jadi perubahan yang belum di-commit di folder project tidak ikut.
    runButton(QStringLiteral("t1"))->click();
    QTRY_COMPARE_WITH_TIMEOUT(int(m_runtime.sessions.size()), 1, 20000);
    QCOMPARE(tabs->currentIndex(), 0);
    FakeAgentSession *session = m_runtime.sessions.last();
    const QString workPath = session->launch().workingDirectory;
    const QDir work(workPath);
    QCOMPARE(workPath, m_tasks->task(QStringLiteral("t1"))->branch.worktree);
    QTRY_COMPARE(files->topLevelItemCount(), 0);
    QSignalSpy requested(panel, &ResponseDrawer::diffRequested);

    // Tool baca tidak mengubah folder kerja, jadi membuka tab perubahan kode tidak membaca git lagi
    AgentEvent read;
    read.kind = AgentEvent::Kind::ToolUse;
    read.toolName = QStringLiteral("Read");
    read.toolDetail = work.filePath(QStringLiteral("hello.txt"));
    session->emitEvent(read);
    tabs->setCurrentIndex(1);
    QCOMPARE(requested.count(), 0);
    QCOMPARE(files->topLevelItemCount(), 0);
    tabs->setCurrentIndex(0);

    // Agent menulis file: diff yang sudah dibaca basi dan dibaca ulang begitu tabnya dibuka
    QVERIFY(writeFile(work.filePath(QStringLiteral("dua.txt")), "x\n"));
    AgentEvent write;
    write.kind = AgentEvent::Kind::ToolUse;
    write.toolName = QStringLiteral("Write");
    write.toolDetail = work.filePath(QStringLiteral("dua.txt"));
    session->emitEvent(write);
    QCOMPARE(requested.count(), 0);   // belum ada yang melihat, jadi git belum dibaca
    tabs->setCurrentIndex(1);
    QCOMPARE(requested.count(), 1);
    QTRY_COMPARE(files->topLevelItemCount(), 1);
    QCOMPARE(tabs->tabText(1), QStringLiteral("Perubahan kode · 1"));

    // Run gagal setelah menulis lagi: kartu tetap di CODER dan diff yang sedang dilihat dibaca ulang
    QVERIFY(writeFile(work.filePath(QStringLiteral("tiga.txt")), "y\n"));
    write.toolDetail = work.filePath(QStringLiteral("tiga.txt"));
    session->emitEvent(write);
    session->finishWith(AgentResult::failure(QStringLiteral("error_max_turns"), QStringLiteral("Kehabisan giliran")));
    QCOMPARE(m_tasks->task(QStringLiteral("t1"))->stage, QStringLiteral("CODER"));
    QVERIFY(m_tasks->task(QStringLiteral("t1"))->state == TaskState::Failed);
    QCOMPARE(requested.count(), 2);
    QCOMPARE(tabs->currentIndex(), 1);
    QTRY_COMPARE(files->topLevelItemCount(), 2);
    QCOMPARE(tabs->tabText(1), QStringLiteral("Perubahan kode · 2"));

    // Dibuka lagi setelah gagal: yang tampil alasan gagalnya; diff tinggal satu klik
    panel->findChild<QPushButton *>(QStringLiteral("btnDrawerClose"))->click();
    QTRY_VERIFY(panel->isHidden());
    QTest::mouseClick(card(QStringLiteral("t1")), Qt::LeftButton);
    QVERIFY(!panel->isHidden());
    QCOMPARE(tabs->currentIndex(), 0);
    QVERIFY(panel->findChild<QTextBrowser *>(QStringLiteral("markdownView"))
                ->toPlainText().contains(QStringLiteral("Run gagal (error_max_turns)")));
    QTRY_COMPARE(files->topLevelItemCount(), 2);

    // Dicoba lagi dan sukses: worktree sudah terpasang, jadi run jalan di sana (sesudah pull); kartu
    // maju ke CLEANER, dan drawer yang mengikutinya tidak punya tab diff
    runButton(QStringLiteral("t1"))->click();
    QTRY_COMPARE(int(m_runtime.sessions.size()), 2);
    QCOMPARE(m_runtime.sessions.last()->launch().workingDirectory, workPath);
    m_runtime.sessions.last()->finishWith(successResult(QStringLiteral("Selesai")));
    QCOMPARE(m_tasks->task(QStringLiteral("t1"))->stage, QStringLiteral("CLEANER"));
    QVERIFY(tabs->isHidden());
}

void TestGui::qaFlowCommitsPushesWithoutMerging() {
    if (QStandardPaths::findExecutable(QStringLiteral("git")).isEmpty()) {
        QSKIP("git tidak ada di PATH");
    }
    const GitSandbox sandbox;
    constexpr int kGitTimeoutMs = 20000;

    // Folder kerja project Demo jadi repository di branch main, dengan remote origin (repository bare)
    const QDir dir(m_workDir.path());
    auto removeRepository = qScopeGuard([dir]() {
        QDir(dir.filePath(QStringLiteral(".git"))).removeRecursively();
        QFile::remove(dir.filePath(QStringLiteral("hello.txt")));
        QFile::remove(dir.filePath(QStringLiteral("login.txt")));
    });
    QTemporaryDir remote;
    const QString origin = QDir(remote.path()).filePath(QStringLiteral("origin.git"));
    QVERIFY(runGit(remote.path(), {QStringLiteral("init"), QStringLiteral("-q"), QStringLiteral("--bare"), origin}));
    QVERIFY(runGit(dir.path(), {QStringLiteral("init"), QStringLiteral("-q"), QStringLiteral("-b"), QStringLiteral("main")}));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("hello.txt")), "halo\n"));
    QVERIFY(runGit(dir.path(), {QStringLiteral("add"), QStringLiteral("-A")}));
    QVERIFY(runGit(dir.path(), {QStringLiteral("commit"), QStringLiteral("-q"), QStringLiteral("-m"), QStringLiteral("awal")}));
    QVERIFY(runGit(dir.path(), {QStringLiteral("remote"), QStringLiteral("add"), QStringLiteral("origin"), origin}));
    QVERIFY(runGit(dir.path(), {QStringLiteral("push"), QStringLiteral("-q"), QStringLiteral("origin"), QStringLiteral("main")}));

    // Task yang spesifikasinya sudah disetujui, siap dikerjakan CODER
    StageRun spec = StageRun::finished(QStringLiteral("SPECIFIER"), successResult(QStringLiteral("# Spek\n\n1. Ada form login")));
    spec.decision = ReviewDecision::Approved;
    TaskItem task;
    task.id = QStringLiteral("t5");
    task.projectId = QStringLiteral("Demo");
    task.stage = QStringLiteral("CODER");
    task.category = QStringLiteral("component");
    task.title = QStringLiteral("Tambah login");
    task.runs = {spec};
    m_tasks->addTask(task);

    // ▶ CODER: branch + worktree dibuat dulu dari main, lalu agent jalan di worktree itu
    runButton(QStringLiteral("t5"))->click();
    QTRY_COMPARE_WITH_TIMEOUT(int(m_runtime.sessions.size()), 1, kGitTimeoutMs);
    QVERIFY(consoleText().contains(QStringLiteral("[GIT] Demo/Tambah login pull main: sudah terbaru")));
    const TaskBranch branch = m_tasks->task(QStringLiteral("t5"))->branch;
    QCOMPARE(branch.name, QStringLiteral("lassomoir/t5-tambah-login"));
    QCOMPARE(branch.base, QStringLiteral("main"));
    const QString worktree = branch.worktree;
    QVERIFY2(worktree.endsWith(QStringLiteral("/projects/Demo/worktrees/t5")), qPrintable(worktree));
    QCOMPARE(m_runtime.sessions.last()->launch().workingDirectory, worktree);
    QVERIFY(consoleText().contains(QStringLiteral("[GIT] Demo/Tambah login branch lassomoir/t5-tambah-login dari main")));
    QVERIFY(writeFile(worktree + QStringLiteral("/login.txt"), "form\n"));
    m_runtime.sessions.last()->finishWith(successResult(QStringLiteral("Menambah login.txt")));
    QCOMPARE(m_tasks->task(QStringLiteral("t5"))->stage, QStringLiteral("CLEANER"));
    QVERIFY(!QFileInfo::exists(dir.filePath(QStringLiteral("login.txt"))));   // folder project belum tersentuh

    // Stage di antaranya dilewati; ▶ QA: kodenya di-commit + push dulu, baru agent QA jalan
    QVERIFY(m_tasks->moveTask(QStringLiteral("t5"), QStringLiteral("QA")));
    runButton(QStringLiteral("t5"))->click();
    QTRY_COMPARE_WITH_TIMEOUT(int(m_runtime.sessions.size()), 2, kGitTimeoutMs);
    QCOMPARE(m_runtime.sessions.last()->launch().workingDirectory, worktree);
    QVERIFY(consoleText().contains(QStringLiteral("[GIT] Demo/Tambah login commit ")));
    QVERIFY(consoleText().contains(QStringLiteral("[GIT] Demo/Tambah login push lassomoir/t5-tambah-login → origin")));
    QCOMPARE(GitSandbox::output(origin, {QStringLiteral("rev-parse"), branch.name}),
             GitSandbox::output(worktree, {QStringLiteral("rev-parse"), QStringLiteral("HEAD")}));
    m_runtime.sessions.last()->finishWith(successResult(QStringLiteral("Kriteria 1 gagal: form tanpa validasi")));
    QVERIFY(m_tasks->task(QStringLiteral("t5"))->state == TaskState::AwaitingReview);

    // Drawer QA: tombol setuju biasa (tidak pernah menyentuh main); QA menolak lewat "Kembalikan ke CODER"
    runButton(QStringLiteral("t5"))->click();   // 📋
    ResponseDrawer *panel = drawer();
    QVERIFY(panel && !panel->isHidden());
    QCOMPARE(panel->taskId(), QStringLiteral("t5"));
    auto *approve = panel->findChild<QPushButton *>(QStringLiteral("btnDrawerApprove"));
    QCOMPARE(approve->text(), QStringLiteral("Setujui → DONE"));
    auto *branchLabel = panel->findChild<QLabel *>(QStringLiteral("drawerBranch"));
    QCOMPARE(branchLabel->text(), QStringLiteral("Branch lassomoir/t5-tambah-login dari main"));
    panel->findChild<QPlainTextEdit *>(QStringLiteral("drawerNote"))->setPlainText(QStringLiteral("Tambah validasi"));
    QCOMPARE(panel->findChild<QComboBox *>(QStringLiteral("drawerSendBackTarget"))->currentText(), QStringLiteral("CODER"));
    panel->findChild<QPushButton *>(QStringLiteral("btnDrawerSendBack"))->click();
    QCOMPARE(m_tasks->task(QStringLiteral("t5"))->stage, QStringLiteral("CODER"));
    QCOMPARE(card(QStringLiteral("t5"))->badge(), QStringLiteral("✓ 1 · ↺ 1"));

    // Kartu yang sama dikerjakan lagi di worktree yang sama (branch task di-pull dulu); sukses
    // langsung kembali ke QA
    runButton(QStringLiteral("t5"))->click();
    QTRY_COMPARE_WITH_TIMEOUT(int(m_runtime.sessions.size()), 3, kGitTimeoutMs);
    QVERIFY(consoleText().contains(QStringLiteral("[GIT] Demo/Tambah login pull lassomoir/t5-tambah-login: sudah terbaru")));
    QCOMPARE(m_runtime.sessions.last()->launch().workingDirectory, worktree);
    QVERIFY(m_runtime.sessions.last()->launch().prompt.contains(QStringLiteral("# Hasil stage sebelumnya (QA)")));
    QVERIFY(writeFile(worktree + QStringLiteral("/login.txt"), "form\nvalidasi\n"));
    m_runtime.sessions.last()->finishWith(successResult(QStringLiteral("Validasi ditambah")));
    QCOMPARE(m_tasks->task(QStringLiteral("t5"))->stage, QStringLiteral("QA"));

    // Sebelum ▶ QA (yang otomatis commit + push), diff putaran ini sudah bisa dilihat lebih dulu:
    // drawer yang masih terbuka ikut menampilkan tab Perubahan kode begitu task kembali ke QA
    QVERIFY(!panel->isHidden());
    auto *codeTabs = panel->findChild<QTabBar *>(QStringLiteral("drawerTabs"));
    QVERIFY(codeTabs && !codeTabs->isHidden());
    QVERIFY(!codeTabs->isTabVisible(2));   // Maintainability: hanya ARCHITECT
    QVERIFY(!codeTabs->isTabVisible(3));   // UML: hanya ARCHITECT
    auto *files = panel->findChild<QTreeWidget *>(QStringLiteral("diffFileList"));
    QTRY_COMPARE(files->topLevelItemCount(), 1);
    QCOMPARE(files->topLevelItem(0)->text(1), QStringLiteral("login.txt"));   // perubahan sejak titik cabang, belum di-commit

    // ▶ QA putaran 2: commit kedua, dan QA melihat laporannya yang dulu
    runButton(QStringLiteral("t5"))->click();
    QTRY_COMPARE_WITH_TIMEOUT(int(m_runtime.sessions.size()), 4, kGitTimeoutMs);
    QVERIFY(m_runtime.sessions.last()->launch().prompt.contains(
        QStringLiteral("# Hasil sebelumnya di stage ini (sebelum dikembalikan)\n\nKriteria 1 gagal")));
    QVERIFY(GitSandbox::output(worktree, {QStringLiteral("log"), QStringLiteral("-1"), QStringLiteral("--format=%b")})
                .contains(QStringLiteral("putaran 2")));
    m_runtime.sessions.last()->finishWith(successResult(QStringLiteral("Semua kriteria lulus")));

    // Main tetap disimpan komitnya sebelum task terakhir: dipakai untuk memastikan main tak tersentuh
    const QString mainBeforeApprove = GitSandbox::output(dir.path(), {QStringLiteral("rev-parse"), QStringLiteral("HEAD")});

    // Setujui (tanpa merge — main diproteksi): task maju ke DONE seketika, tanpa menunggu git apa pun
    runButton(QStringLiteral("t5"))->click();   // 📋
    QVERIFY(!panel->isHidden());
    QVERIFY(approve->isVisibleTo(panel));
    approve->click();
    QCOMPARE(m_tasks->task(QStringLiteral("t5"))->stage, QStringLiteral("DONE"));
    QCOMPARE(card(QStringLiteral("t5"))->badge(), QStringLiteral("✓ 2 · ↺ 1"));
    QVERIFY(consoleText().contains(QStringLiteral("[GATE] Demo/Tambah login QA disetujui → DONE")));

    // main/folder kerja project tidak pernah tersentuh: tidak ada merge otomatis
    QVERIFY(!QFileInfo::exists(dir.filePath(QStringLiteral("login.txt"))));
    QCOMPARE(GitSandbox::output(dir.path(), {QStringLiteral("rev-parse"), QStringLiteral("HEAD")}), mainBeforeApprove);
    QCOMPARE(GitSandbox::output(origin, {QStringLiteral("rev-parse"), QStringLiteral("main")}), mainBeforeApprove);

    // Worktree dibuang (async) begitu DONE; branch task tetap ada di lokal & remote untuk di-PR manual.
    // Ditunggu lewat state TaskManager (bukan keberadaan folder saja): penghapusan folder terjadi di
    // thread lain sesaat sebelum sinyal finished-nya diproses GUI, jadi memeriksa folder duluan rentan
    // balapan terhadap setBranch() yang baru menyusul.
    QTRY_VERIFY_WITH_TIMEOUT(!m_tasks->task(QStringLiteral("t5"))->branch.hasWorktree(), kGitTimeoutMs);
    const TaskBranch afterDone = m_tasks->task(QStringLiteral("t5"))->branch;
    QVERIFY(!QFileInfo::exists(worktree));
    QVERIFY(afterDone.mergedCommit.isEmpty());
    QCOMPARE(afterDone.name, branch.name);
    QVERIFY(runGit(dir.path(), {QStringLiteral("show-ref"), QStringLiteral("--verify"), QStringLiteral("--quiet"),
                                QStringLiteral("refs/heads/") + branch.name}));
    QVERIFY(runGit(origin, {QStringLiteral("show-ref"), QStringLiteral("--verify"), QStringLiteral("--quiet"),
                            QStringLiteral("refs/heads/") + branch.name}));
    QVERIFY(consoleText().contains(QStringLiteral("dihapus; branch lassomoir/t5-tambah-login tetap ada")));
    // Drawer masih menampilkan task ini; label branch tidak berubah karena tidak pernah di-merge
    QCOMPARE(branchLabel->text(), QStringLiteral("Branch lassomoir/t5-tambah-login dari main"));

    // Task lain yang dihapus: worktree-nya ikut dibuang, branch-nya tetap
    TaskItem other = task;
    other.id = QStringLiteral("t6");
    other.title = QStringLiteral("Hapus saya");
    m_tasks->addTask(other);
    runButton(QStringLiteral("t6"))->click();
    QTRY_COMPARE_WITH_TIMEOUT(int(m_runtime.sessions.size()), 5, kGitTimeoutMs);
    const QString otherWorktree = m_runtime.sessions.last()->launch().workingDirectory;
    QVERIFY(QFileInfo::exists(otherWorktree));
    m_runtime.sessions.last()->finishWith(AgentResult::failure(QStringLiteral("cancelled"), QStringLiteral("dibatalkan")));
    QVERIFY(m_tasks->removeTask(QStringLiteral("t6")));
    QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(otherWorktree), kGitTimeoutMs);
    QVERIFY(runGit(dir.path(), {QStringLiteral("show-ref"), QStringLiteral("--verify"), QStringLiteral("--quiet"),
                                QStringLiteral("refs/heads/lassomoir/t6-hapus-saya")}));
}

void TestGui::newTaskChoosesBranchAndPulls() {
    if (QStandardPaths::findExecutable(QStringLiteral("git")).isEmpty()) {
        QSKIP("git tidak ada di PATH");
    }
    const GitSandbox sandbox;
    constexpr int kGitTimeoutMs = 20000;

    // Folder kerja project Demo: repository di main dengan branch lokal fitur (dan branch kerja task
    // lama); rekan kerja sudah push branch rilis yang belum ada di lokal
    const QDir dir(m_workDir.path());
    auto removeRepository = qScopeGuard([dir]() {
        QDir(dir.filePath(QStringLiteral(".git"))).removeRecursively();
        QFile::remove(dir.filePath(QStringLiteral("hello.txt")));
    });
    QTemporaryDir remote;
    const QString origin = QDir(remote.path()).filePath(QStringLiteral("origin.git"));
    const QString other = QDir(remote.path()).filePath(QStringLiteral("rekan"));
    QVERIFY(runGit(remote.path(), {QStringLiteral("init"), QStringLiteral("-q"), QStringLiteral("--bare"), origin}));
    QVERIFY(runGit(dir.path(), {QStringLiteral("init"), QStringLiteral("-q"), QStringLiteral("-b"), QStringLiteral("main")}));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("hello.txt")), "halo\n"));
    QVERIFY(runGit(dir.path(), {QStringLiteral("add"), QStringLiteral("-A")}));
    QVERIFY(runGit(dir.path(), {QStringLiteral("commit"), QStringLiteral("-q"), QStringLiteral("-m"), QStringLiteral("awal")}));
    QVERIFY(runGit(dir.path(), {QStringLiteral("remote"), QStringLiteral("add"), QStringLiteral("origin"), origin}));
    QVERIFY(runGit(dir.path(), {QStringLiteral("push"), QStringLiteral("-q"), QStringLiteral("-u"), QStringLiteral("origin"),
                                QStringLiteral("main")}));
    QVERIFY(runGit(dir.path(), {QStringLiteral("branch"), QStringLiteral("fitur")}));
    QVERIFY(runGit(dir.path(), {QStringLiteral("branch"), QStringLiteral("lassomoir/1-task-lama")}));
    QVERIFY(runGit(remote.path(), {QStringLiteral("clone"), QStringLiteral("-q"), origin, other}));
    QVERIFY(runGit(other, {QStringLiteral("switch"), QStringLiteral("-q"), QStringLiteral("-c"), QStringLiteral("rilis"),
                           QStringLiteral("origin/main")}));
    auto otherCommitsRilis = [other](const QByteArray &content) {
        return writeFile(other + QStringLiteral("/rilis.txt"), content)
               && runGit(other, {QStringLiteral("add"), QStringLiteral("-A")})
               && runGit(other, {QStringLiteral("commit"), QStringLiteral("-q"), QStringLiteral("-m"), QStringLiteral("rilis")})
               && runGit(other, {QStringLiteral("push"), QStringLiteral("-q"), QStringLiteral("origin"), QStringLiteral("rilis")});
    };
    QVERIFY(otherCommitsRilis("v1\n"));

    // "+": branch dipilih paling dulu, lewat label + caret yang membuka menu (bukan combobox).
    // Daftarnya dibaca git di latar belakang dan tombol simpan menunggu; yang aktif terpilih lebih
    // dulu, branch kerja task tidak ikut ditawarkan
    driveModalDialog([](QDialog *dialog) {
        QVERIFY(!dialog->findChild<QComboBox *>(QStringLiteral("taskFormBranch")));
        auto *branch = dialog->findChild<QPushButton *>(QStringLiteral("taskFormBranch"));
        QVERIFY(branch);
        auto *create = dialog->findChild<QPushButton *>(QStringLiteral("btnTaskFormCreate"));
        dialog->findChild<QLineEdit *>(QStringLiteral("taskFormInput"))->setText(QStringLiteral("Rilis v2"));
        QTRY_VERIFY(branch->isEnabled());
        QVERIFY(create->isEnabled());
        QMenu *menu = branch->menu();
        QVERIFY(menu);
        // Branch lokal tampil dulu; rilis menyusul sesudah origin di-fetch di latar belakang
        QTRY_COMPARE(menu->actions().size(), 3);
        QCOMPARE(dialog->findChild<QLabel *>(QStringLiteral("taskFormHint"))->text(),
                 QStringLiteral("Task bercabang dari branch ini. Branch ini di-pull dari origin saat task dibuat "
                                "dan sebelum setiap run."));
        const QList<QAction *> actions = menu->actions();
        QStringList items;
        for (QAction *action : actions) {
            items.append(action->text());
        }
        QCOMPARE(items, QStringList({"main (aktif)", "fitur", "origin/rilis"}));
        QCOMPARE(branch->text(), QStringLiteral("main (aktif)"));
        QVERIFY(actions[0]->font().bold());
        QVERIFY(actions[2]->toolTip().contains(QStringLiteral("Baru ada di origin")));
        actions[2]->trigger();
        QCOMPARE(branch->text(), QStringLiteral("origin/rilis"));
        QVERIFY(actions[2]->font().bold());
        QVERIFY(!actions[0]->font().bold());
        dialog->findChildren<QComboBox *>(QStringLiteral("taskFormCombo")).at(1)->setCurrentText(QStringLiteral("SPECIFIER"));
        create->click();
    });
    m_window->findChild<QPushButton *>(QStringLiteral("btnProjectNewTask"))->click();
    QVERIFY(m_dialogSeen);

    std::optional<TaskItem> created;
    for (const TaskItem &task : m_tasks->tasksForProject(QStringLiteral("Demo"))) {
        if (task.title == QLatin1String("Rilis v2")) {
            created = task;
        }
    }
    QVERIFY(created);
    QCOMPARE(created->branch.base, QStringLiteral("rilis"));
    QVERIFY(created->branch.isEmpty());   // branch task baru dibuat saat run pertama

    // Branch yang dipilih langsung di-pull: branch lokalnya dibuat dari origin
    QVERIFY(consoleText().contains(QStringLiteral("[GIT] Demo/Rilis v2 pull rilis dari origin…")));
    QTRY_VERIFY_WITH_TIMEOUT(consoleText().contains(QStringLiteral("[GIT] Demo/Rilis v2 pull rilis: branch lokal dibuat dari origin/rilis")),
                             kGitTimeoutMs);
    QCOMPARE(GitSandbox::output(dir.path(), {QStringLiteral("rev-parse"), QStringLiteral("rilis")}),
             GitSandbox::output(origin, {QStringLiteral("rev-parse"), QStringLiteral("rilis")}));

    // Rekan kerja push lagi. ▶ SPECIFIER: pull dulu, lalu agent membaca rilis terbaru di worktree
    // task sendiri, bukan main yang aktif di folder kerja project
    QVERIFY(otherCommitsRilis("v2\n"));
    const int before = int(m_runtime.sessions.size());
    runButton(created->id)->click();
    QTRY_COMPARE_WITH_TIMEOUT(int(m_runtime.sessions.size()), before + 1, kGitTimeoutMs);
    const TaskBranch branch = m_tasks->task(created->id)->branch;
    QCOMPARE(branch.base, QStringLiteral("rilis"));
    QVERIFY(branch.name.startsWith(QStringLiteral("lassomoir/")));
    QCOMPARE(m_runtime.sessions.last()->launch().workingDirectory, branch.worktree);
    QCOMPARE(readFile(branch.worktree + QStringLiteral("/rilis.txt")), QByteArray("v2\n"));
    QVERIFY(consoleText().contains(QStringLiteral("[GIT] Demo/Rilis v2 pull rilis: 1 commit baru dari origin")));
    QCOMPARE(GitSandbox::output(dir.path(), {QStringLiteral("branch"), QStringLiteral("--show-current")}), QStringLiteral("main"));
    m_runtime.sessions.last()->finishWith(successResult(QStringLiteral("# Spek rilis v2")));

    // Branch task sudah dibuat: branch dasarnya tidak bisa diganti lagi lewat form edit
    driveModalDialog([](QDialog *dialog) {
        auto *branchInput = dialog->findChild<QPushButton *>(QStringLiteral("taskFormBranch"));
        QVERIFY(branchInput);
        QCOMPARE(branchInput->text(), QStringLiteral("rilis"));
        QVERIFY(!branchInput->isEnabled());
        QVERIFY(!branchInput->menu());
        dialog->reject();
    });
    QTest::mouseDClick(card(created->id), Qt::LeftButton);
    QVERIFY(m_dialogSeen);

    // Worktree task dibuang sebelum folder sementara dihapus
    QVERIFY(m_tasks->removeTask(created->id));
    QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(branch.worktree), kGitTimeoutMs);
}

void TestGui::maintainabilityViewShowsCSharpMembers() {
    const QString path = QStringLiteral("Services/OrderService.cs");
    FileDiff file;
    file.path = path;
    file.before = CodeMetrics::measure(path, QStringLiteral(
        "namespace Shop;\n"
        "public class OrderService : ServiceBase\n"
        "{\n"
        "    public int Place(int quantity)\n"
        "    {\n"
        "        return quantity;\n"
        "    }\n"
        "\n"
        "    public void Refund(int id) { }\n"
        "}\n"));
    file.after = CodeMetrics::measure(path, QStringLiteral(
        "namespace Shop;\n"
        "public class OrderService : ServiceBase\n"
        "{\n"
        "    public int Place(int quantity)\n"
        "    {\n"
        "        if (quantity > 100 || quantity < 0) { return 0; }\n"
        "        return quantity > 10 ? quantity - 1 : quantity;\n"
        "    }\n"
        "\n"
        "    public void Cancel(Order order) { order.Close(); }\n"
        "\n"
        "    public void Refund(int id, string reason) { if (reason == null) { return; } }\n"
        "}\n"));
    WorkspaceDiff diff;
    diff.files = {file};

    MaintainabilityView view;
    view.showDiff(diff);
    auto *tree = view.findChild<QTreeWidget *>(QStringLiteral("miFileList"));
    QVERIFY(tree);

    // File → tipe → member, seperti Code Metrics Visual Studio
    QCOMPARE(tree->topLevelItemCount(), 1);
    QTreeWidgetItem *fileItem = tree->topLevelItem(0);
    QCOMPARE(fileItem->text(0), path);
    QCOMPARE(fileItem->childCount(), 1);
    QTreeWidgetItem *type = fileItem->child(0);
    QCOMPARE(type->text(0), QStringLiteral("OrderService"));
    QCOMPARE(type->text(5), QStringLiteral("≥2"));   // ServiceBase tidak ada di diff ini
    QCOMPARE(type->childCount(), 3);

    QTreeWidgetItem *place = type->child(0);
    QCOMPARE(place->text(0), QStringLiteral("Place(int)"));
    QVERIFY(place->text(1).toInt() > place->text(2).toInt());
    QVERIFY(place->text(3).startsWith(QStringLiteral("−")));
    QCOMPARE(place->text(4), QStringLiteral("4"));
    QVERIFY(place->text(5).isEmpty());   // DIT hanya untuk tipe

    QTreeWidgetItem *cancel = type->child(1);
    QCOMPARE(cancel->text(0), QStringLiteral("Cancel(Order)"));
    QCOMPARE(cancel->text(1), QStringLiteral("—"));
    QCOMPARE(cancel->text(3), QStringLiteral("baru"));
    QCOMPARE(cancel->text(6), QStringLiteral("1"));   // Order

    // Tanda tangan berubah: tetap dibandingkan dengan versi lamanya lewat nama
    QTreeWidgetItem *refund = type->child(2);
    QCOMPARE(refund->text(0), QStringLiteral("Refund(int, string)"));
    QVERIFY(refund->text(1) != QStringLiteral("—"));
    QVERIFY(refund->text(3) != QStringLiteral("baru"));
    QVERIFY(refund->toolTip(0).contains(QStringLiteral("Sebelumnya: Refund(int)")));

    QVERIFY(!tree->isColumnHidden(5));
    QVERIFY(!tree->isColumnHidden(6));
    QVERIFY(place->parent()->isExpanded());
    QVERIFY(view.findChild<QLabel *>(QStringLiteral("miLegend"))->text().contains(QStringLiteral("C#:")));
}

void TestGui::umlTabShowsClassAndAgentDiagrams() {
    FakeMermaidRenderer renderer;
    ResponseDrawer panel(&renderer);

    TaskItem task;
    task.id = QStringLiteral("u1");
    task.projectId = QStringLiteral("Demo");
    task.stage = QStringLiteral("ARCHITECT");
    task.title = QStringLiteral("Tinjau struktur");
    task.runs = {StageRun::finished(QStringLiteral("ARCHITECT"), successResult(QStringLiteral(
        "Temuan.\n\n```mermaid\nsequenceDiagram\n  Kasir->>OrderService: Place\n```\n")))};

    QSignalSpy requested(&panel, &ResponseDrawer::diffRequested);
    panel.showTask(task, RunState::Idle, QStringLiteral("HARDENER"), {QStringLiteral("CODER")});
    QCOMPARE(requested.count(), 1);

    auto *tabs = panel.findChild<QTabBar *>(QStringLiteral("drawerTabs"));
    QVERIFY(tabs);
    QCOMPARE(tabs->count(), 4);
    QCOMPARE(tabs->tabText(3), QStringLiteral("UML"));
    auto *uml = panel.findChild<QTextBrowser *>(QStringLiteral("umlView"));
    QVERIFY(uml);
    QVERIFY(uml->toPlainText().contains(QStringLiteral("Membaca kode")));
    // Diagram dari hasil agent ARCHITECT langsung diminta ke renderer
    QVERIFY(renderer.requests.contains(QStringLiteral("sequenceDiagram\n  Kasir->>OrderService: Place")));

    FileDiff file;
    file.path = QStringLiteral("Models/Order.cs");
    file.status = FileDiff::Status::Added;
    file.after = CodeMetrics::measure(file.path, QStringLiteral(
        "namespace Shop;\npublic class Order\n{\n    public int Id { get; set; }\n}\n"));
    WorkspaceDiff diff;
    diff.files = {file};
    panel.showDiff(QStringLiteral("u1"), diff);

    QCOMPARE(tabs->tabText(3), QStringLiteral("UML · 1"));
    QVERIFY(uml->toPlainText().contains(QStringLiteral("Hijau = tipe baru")));
    const auto classDiagram = std::find_if(renderer.requests.cbegin(), renderer.requests.cend(), [](const QString &code) {
        return code.startsWith(QStringLiteral("classDiagram")) && code.contains(QStringLiteral("class Order {"))
               && code.contains(QStringLiteral("+int Id"));
    });
    QVERIFY(classDiagram != renderer.requests.cend());

    // Tanpa tipe C#: penjelasan, bukan diagram kosong
    WorkspaceDiff text;
    FileDiff notes;
    notes.path = QStringLiteral("README.md");
    text.files = {notes};
    panel.showDiff(QStringLiteral("u1"), text);
    QCOMPARE(tabs->tabText(3), QStringLiteral("UML"));
    QVERIFY(uml->toPlainText().contains(QStringLiteral("tidak memuat tipe C#")));
}

namespace {

QString blockText(const QPlainTextEdit *text, int block) {
    return text->document()->findBlockByNumber(block).text();
}

bool blockHasBackground(const QPlainTextEdit *text, int block, QRgb color) {
    return text->document()->findBlockByNumber(block).blockFormat().background().color() == QColor(color);
}

// Bagian baris yang ditandai berubah (latar karakter berwarna)
QString markedText(const QPlainTextEdit *text, int block, QRgb color) {
    QString marked;
    const QTextBlock line = text->document()->findBlockByNumber(block);
    for (auto it = line.begin(); !it.atEnd(); ++it) {
        const QTextCharFormat format = it.fragment().charFormat();
        if (format.hasProperty(QTextFormat::BackgroundBrush) && format.background().color() == QColor(color)) {
            marked += it.fragment().text();
        }
    }
    return marked;
}

}

void TestGui::diffViewComparesSideBySide() {
    // Satu hunk: baris diubah (berpasangan), baris dihapus tanpa pasangan, dan akhir file tanpa
    // newline yang diganti dua baris
    WorkspaceDiff diff;
    diff.baseCommit = QStringLiteral("aaaaaaa");
    diff.targetCommit = QStringLiteral("bbbbbbb");
    diff.files = GitDiff::parse(QStringLiteral(
        "diff --git a/app.cpp b/app.cpp\n"
        "--- a/app.cpp\n"
        "+++ b/app.cpp\n"
        "@@ -1,6 +1,6 @@\n"
        " int main() {\n"
        "-    int nilai = 1;\n"
        "-    int lama = 0;\n"
        "+    int nilai = 2;\n"
        "     return nilai;\n"
        " }\n"
        "-// akhir\n"
        "\\ No newline at end of file\n"
        "+// akhir baru\n"
        "+// tambahan\n"
        "\\ No newline at end of file\n"));
    QCOMPARE(diff.files.size(), 1);

    DiffView view;
    view.resize(640, 150);
    view.show();
    view.showDiff(diff);
    QCOMPARE(view.findChild<QLabel *>(QStringLiteral("diffSummary"))->text(),
             QStringLiteral("1 file berubah · +3 −3 · aaaaaaa → bbbbbbb"));
    auto *unified = view.findChild<QPlainTextEdit *>(QStringLiteral("diffText"));
    auto *before = view.findChild<QPlainTextEdit *>(QStringLiteral("diffOldText"));
    auto *after = view.findChild<QPlainTextEdit *>(QStringLiteral("diffNewText"));
    auto *unifiedButton = view.findChild<QPushButton *>(QStringLiteral("btnDiffUnified"));
    auto *splitButton = view.findChild<QPushButton *>(QStringLiteral("btnDiffSplit"));
    QVERIFY(unified && before && after && unifiedButton && splitButton);
    // Bawaan satu kolom, seperti tab "Perubahan kode" di drawer
    QVERIFY(!view.isSideBySide());
    QVERIFY(unifiedButton->isChecked());
    QVERIFY(unified->isVisible());
    QVERIFY(!before->isVisible());
    QVERIFY(unified->toPlainText().contains(QStringLiteral("-     int lama = 0;")));

    QSignalSpy toggled(&view, &DiffView::sideBySideChanged);
    splitButton->click();
    QCOMPARE(toggled.count(), 1);
    QVERIFY(view.isSideBySide());
    QVERIFY(!unified->isVisible());
    QVERIFY(before->isVisible() && after->isVisible());
    QVERIFY(unified->toPlainText().isEmpty());   // hanya tampilan yang aktif yang diisi

    // Jumlah baris kedua sisi sama, jadi baris yang berpasangan selalu sejajar
    QCOMPARE(before->document()->blockCount(), 10);
    QCOMPARE(after->document()->blockCount(), 10);
    QCOMPARE(blockText(before, 0), QStringLiteral(" M  app.cpp"));
    QCOMPARE(blockText(after, 0), QStringLiteral(" M  app.cpp"));
    QCOMPARE(blockText(before, 2), QStringLiteral("1   int main() {"));
    QCOMPARE(blockText(after, 2), QStringLiteral("1   int main() {"));
    // Baris diubah: kiri lama, kanan baru; hanya angka yang berbeda yang ditandai
    QCOMPARE(blockText(before, 3), QStringLiteral("2 -     int nilai = 1;"));
    QCOMPARE(blockText(after, 3), QStringLiteral("2 +     int nilai = 2;"));
    QCOMPARE(markedText(before, 3, 0xffc0bc), QStringLiteral("1"));
    QCOMPARE(markedText(after, 3, 0xabf2bc), QStringLiteral("2"));
    // Baris dihapus tanpa pasangan: sisi kanan diganjal baris kosong
    QCOMPARE(blockText(before, 4), QStringLiteral("3 -     int lama = 0;"));
    QVERIFY(blockText(after, 4).isEmpty());
    QVERIFY(blockHasBackground(after, 4, 0xf6f1e9));
    QVERIFY(!markedText(before, 4, 0xffc0bc).size());
    // Konteks sesudahnya kembali sejajar dengan nomor baris masing-masing sisi
    QCOMPARE(blockText(before, 5), QStringLiteral("4       return nilai;"));
    QCOMPARE(blockText(after, 5), QStringLiteral("3       return nilai;"));
    // Akhir file: "\ No newline" ikut sisinya masing-masing
    QCOMPARE(blockText(before, 7), QStringLiteral("6 - // akhir"));
    QCOMPARE(blockText(after, 7), QStringLiteral("5 + // akhir baru"));
    QCOMPARE(markedText(after, 7, 0xabf2bc), QStringLiteral(" baru"));
    QVERIFY(blockText(before, 8).contains(QStringLiteral("No newline at end of file")));
    QCOMPARE(blockText(after, 8), QStringLiteral("6 + // tambahan"));
    QVERIFY(blockText(before, 9).isEmpty());
    QVERIFY(blockText(after, 9).contains(QStringLiteral("No newline at end of file")));

    // Digulir bersamaan: kiri ke bawah, kanan ikut; kanan ke atas, kiri ikut
    QScrollBar *left = before->verticalScrollBar();
    QScrollBar *right = after->verticalScrollBar();
    QTRY_VERIFY(left->maximum() > 0);
    left->setValue(left->maximum());
    QCOMPARE(right->value(), left->maximum());
    right->setValue(1);
    QCOMPARE(left->value(), 1);

    // Kembali ke satu kolom
    unifiedButton->click();
    QCOMPARE(toggled.count(), 2);
    QVERIFY(unified->isVisible());
    QVERIFY(!before->isVisible());
    QVERIFY(unified->toPlainText().contains(QStringLiteral("+     int nilai = 2;")));
    QVERIFY(before->toPlainText().isEmpty());

    // Pesan perubahan antar-commit: commit tanpa perubahan file, dan commit pertama repository
    WorkspaceDiff empty;
    empty.baseCommit = QStringLiteral("aaaaaaa");
    empty.targetCommit = QStringLiteral("bbbbbbb");
    view.showDiff(empty);
    QCOMPARE(view.findChild<QLabel *>(QStringLiteral("diffSummary"))->text(),
             QStringLiteral("Tidak ada perubahan file antara aaaaaaa dan bbbbbbb."));
    QVERIFY(!unified->isVisible());
    QVERIFY(!unifiedButton->isVisible());
    diff.baseCommit.clear();
    view.showDiff(diff);
    QCOMPARE(view.findChild<QLabel *>(QStringLiteral("diffSummary"))->text(),
             QStringLiteral("1 file berubah · +3 −3 · commit pertama bbbbbbb"));
}

void TestGui::branchViewerComparesBranchesAndCommits() {
    if (QStandardPaths::findExecutable(QStringLiteral("git")).isEmpty()) {
        QSKIP("git tidak ada di PATH");
    }
    const GitSandbox sandbox;
    constexpr int kGitTimeoutMs = 20000;

    // Folder kerja project Demo: main (awal → ubah nilai → tambah baris) dan fitur yang bercabang
    // dari "ubah nilai" (tambah fitur → ubah fitur)
    const QDir dir(m_workDir.path());
    auto removeRepository = qScopeGuard([dir]() {
        QDir(dir.filePath(QStringLiteral(".git"))).removeRecursively();
        QFile::remove(dir.filePath(QStringLiteral("hello.txt")));
        QFile::remove(dir.filePath(QStringLiteral("fitur.txt")));
    });
    auto commitAll = [&dir](const QString &message) {
        return runGit(dir.path(), {QStringLiteral("add"), QStringLiteral("-A")})
               && runGit(dir.path(), {QStringLiteral("commit"), QStringLiteral("-q"), QStringLiteral("-m"), message});
    };
    auto head = [&dir]() {
        return GitSandbox::output(dir.path(), {QStringLiteral("rev-parse"), QStringLiteral("--short"), QStringLiteral("HEAD")});
    };
    QVERIFY(runGit(dir.path(), {QStringLiteral("init"), QStringLiteral("-q"), QStringLiteral("-b"), QStringLiteral("main")}));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("hello.txt")), "a\nnilai = 1\nc\n"));
    QVERIFY(commitAll(QStringLiteral("awal")));
    const QString first = head();
    QVERIFY(writeFile(dir.filePath(QStringLiteral("hello.txt")), "a\nnilai = 2\nc\n"));
    QVERIFY(commitAll(QStringLiteral("ubah nilai")));
    const QString second = head();
    QVERIFY(runGit(dir.path(), {QStringLiteral("switch"), QStringLiteral("-q"), QStringLiteral("-c"), QStringLiteral("fitur")}));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("fitur.txt")), "x\n"));
    QVERIFY(commitAll(QStringLiteral("tambah fitur")));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("fitur.txt")), "x\ny\n"));
    QVERIFY(commitAll(QStringLiteral("ubah fitur")));
    QVERIFY(runGit(dir.path(), {QStringLiteral("switch"), QStringLiteral("-q"), QStringLiteral("main")}));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("hello.txt")), "a\nnilai = 2\nc\nd\n"));
    QVERIFY(commitAll(QStringLiteral("tambah baris")));
    const QString third = head();

    // Tombol branch di header swimlane muncul begitu folder kerja terbaca sebagai repository
    auto *branchButton = m_window->findChild<QPushButton *>(QStringLiteral("btnBranch"));
    QVERIFY(branchButton);
    QVERIFY(branchButton->isHidden());   // saat jendela dibuka folder kerja belum repository
    m_window->setActiveProject(QStringLiteral("Demo"));
    QTRY_COMPARE_WITH_TIMEOUT(branchButton->text(), QStringLiteral("main"), kGitTimeoutMs);
    QVERIFY(!branchButton->isHidden());
    QVERIFY(branchButton->toolTip().startsWith(QStringLiteral("Folder kerja sedang di branch main (%1)").arg(third)));

    // Klik: jendela branch & commit, dimulai dari branch yang aktif di folder kerja
    branchButton->click();
    QPointer<BranchViewer> viewer = m_window->findChild<BranchViewer *>();
    QVERIFY(viewer && viewer->isVisible());
    QCOMPARE(viewer->windowTitle(), QStringLiteral("Branch & commit — Demo"));
    auto *branches = viewer->findChild<QComboBox *>(QStringLiteral("branchSelector"));
    auto *compare = viewer->findChild<QComboBox *>(QStringLiteral("branchCompareSelector"));
    auto *list = viewer->findChild<QTreeWidget *>(QStringLiteral("branchCommitList"));
    auto *listTitle = viewer->findChild<QLabel *>(QStringLiteral("branchListTitle"));
    auto *detailTitle = viewer->findChild<QLabel *>(QStringLiteral("branchDetailTitle"));
    auto *detailMeta = viewer->findChild<QLabel *>(QStringLiteral("branchDetailMeta"));
    auto *files = viewer->findChild<QTreeWidget *>(QStringLiteral("diffFileList"));
    auto *summary = viewer->findChild<QLabel *>(QStringLiteral("diffSummary"));
    auto *before = viewer->findChild<QPlainTextEdit *>(QStringLiteral("diffOldText"));
    auto *after = viewer->findChild<QPlainTextEdit *>(QStringLiteral("diffNewText"));
    QVERIFY(branches && compare && list && listTitle && detailTitle && detailMeta && files && summary && before && after);
    auto subjects = [list]() {
        QStringList result;
        for (int row = 0; row < list->topLevelItemCount(); ++row) {
            result.append(list->topLevelItem(row)->text(0));
        }
        return result;
    };
    QTRY_COMPARE_WITH_TIMEOUT(subjects(), QStringList({"tambah baris", "ubah nilai", "awal"}), kGitTimeoutMs);
    QCOMPARE(viewer->branch(), QStringLiteral("main"));
    QCOMPARE(branches->currentText(), QStringLiteral("main"));
    QCOMPARE(branches->count(), 2);
    QCOMPARE(branches->itemText(1), QStringLiteral("fitur"));
    QCOMPARE(compare->itemText(0), QStringLiteral("Tidak dibandingkan"));
    QVERIFY(viewer->compareTarget().isEmpty());
    QCOMPARE(listTitle->text(), QStringLiteral("COMMIT · 3"));
    QVERIFY(list->topLevelItem(0)->data(0, Qt::UserRole + 2).toString().startsWith(third + QStringLiteral(" · Lassomoir Test · ")));

    // Commit terbaru langsung terpilih: perubahannya terhadap induknya, dua kolom berdampingan
    QTRY_COMPARE_WITH_TIMEOUT(detailTitle->text(), QStringLiteral("tambah baris"), kGitTimeoutMs);
    QTRY_COMPARE_WITH_TIMEOUT(files->topLevelItemCount(), 1, kGitTimeoutMs);
    QCOMPARE(files->topLevelItem(0)->text(1), QStringLiteral("hello.txt"));
    QCOMPARE(summary->text(), QStringLiteral("1 file berubah · +1 −0 · %1 → %2").arg(second, third));
    QVERIFY(before->isVisible() && after->isVisible());
    QVERIFY(detailMeta->text().contains(QStringLiteral("Induk %1").arg(second)));

    // Klik commit lain: sebelum | sesudah sejajar, angka yang berubah ditandai
    QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, list->visualItemRect(list->topLevelItem(1)).center());
    QTRY_COMPARE_WITH_TIMEOUT(detailTitle->text(), QStringLiteral("ubah nilai"), kGitTimeoutMs);
    QTRY_COMPARE_WITH_TIMEOUT(summary->text(), QStringLiteral("1 file berubah · +1 −1 · %1 → %2").arg(first, second),
                              kGitTimeoutMs);
    QCOMPARE(blockText(before, 3), QStringLiteral("2 - nilai = 1"));
    QCOMPARE(blockText(after, 3), QStringLiteral("2 + nilai = 2"));
    QCOMPARE(markedText(after, 3, 0xabf2bc), QStringLiteral("2"));

    // Ctrl + klik commit ketiga: dua commit dibandingkan, dari yang lama ke yang baru
    QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, list->visualItemRect(list->topLevelItem(0)).center());
    QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::ControlModifier,
                      list->visualItemRect(list->topLevelItem(2)).center());
    QCOMPARE(list->selectedItems().size(), 2);
    QTRY_COMPARE_WITH_TIMEOUT(detailTitle->text(), QStringLiteral("%1 → %2").arg(first, third), kGitTimeoutMs);
    QTRY_COMPARE_WITH_TIMEOUT(summary->text(), QStringLiteral("1 file berubah · +2 −1 · %1 → %2").arg(first, third),
                              kGitTimeoutMs);
    // Tiga commit: tidak jelas mana yang dibandingkan
    QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::ControlModifier,
                      list->visualItemRect(list->topLevelItem(1)).center());
    QTRY_VERIFY_WITH_TIMEOUT(summary->text().startsWith(QStringLiteral("Pilih satu commit, atau dua commit")), kGitTimeoutMs);
    QVERIFY(files->isHidden());

    // Pilih branch lain: daftar commit mengikuti
    branches->setCurrentIndex(branches->findData(QStringLiteral("fitur")));
    QTRY_COMPARE_WITH_TIMEOUT(subjects(), QStringList({"ubah fitur", "tambah fitur", "ubah nilai", "awal"}), kGitTimeoutMs);
    QCOMPARE(listTitle->text(), QStringLiteral("COMMIT · 4"));
    QVERIFY(list->topLevelItem(0)->data(0, Qt::UserRole + 3).toStringList().contains(QStringLiteral("fitur")));

    // Dibandingkan dengan main: hanya commit & perubahan fitur yang belum ada di main, seperti merge request
    compare->setCurrentIndex(compare->findData(QStringLiteral("main")));
    QTRY_COMPARE_WITH_TIMEOUT(subjects(), QStringList({"Semua perubahan", "ubah fitur", "tambah fitur"}), kGitTimeoutMs);
    QCOMPARE(viewer->compareTarget(), QStringLiteral("main"));
    QCOMPARE(listTitle->text(), QStringLiteral("COMMIT · 2 belum ada di main"));
    QCOMPARE(list->topLevelItem(0)->data(0, Qt::UserRole + 2).toString(),
             QStringLiteral("2 commit di depan · 1 di belakang main · titik cabang %1").arg(second));
    QTRY_COMPARE_WITH_TIMEOUT(detailTitle->text(), QStringLiteral("fitur → main"), kGitTimeoutMs);
    QTRY_COMPARE_WITH_TIMEOUT(files->topLevelItemCount(), 1, kGitTimeoutMs);
    QCOMPARE(files->topLevelItem(0)->text(1), QStringLiteral("fitur.txt"));   // hello.txt dari main tidak ikut
    QVERIFY(summary->text().startsWith(QStringLiteral("1 file berubah · +2 −0 · %1 → ").arg(second)));

    // Tukar: main dibanding fitur = yang ada di main tapi belum di fitur
    viewer->findChild<QToolButton *>(QStringLiteral("btnBranchSwap"))->click();
    QTRY_COMPARE_WITH_TIMEOUT(subjects(), QStringList({"Semua perubahan", "tambah baris"}), kGitTimeoutMs);
    QCOMPARE(viewer->branch(), QStringLiteral("main"));
    QCOMPARE(viewer->compareTarget(), QStringLiteral("fitur"));
    QTRY_COMPARE_WITH_TIMEOUT(summary->text(), QStringLiteral("1 file berubah · +1 −0 · %1 → %2").arg(second, third),
                              kGitTimeoutMs);

    // Satu kolom juga bisa dipilih di jendela ini
    viewer->findChild<QPushButton *>(QStringLiteral("btnDiffUnified"))->click();
    auto *unified = viewer->findChild<QPlainTextEdit *>(QStringLiteral("diffText"));
    QVERIFY(unified->isVisible());
    QVERIFY(unified->toPlainText().contains(QStringLiteral("4 + d")));

    // Dari drawer task ber-branch: "Lihat commit" membuka jendela yang sama di branch task itu,
    // dibanding branch dasarnya
    TaskItem task;
    task.id = QStringLiteral("t9");
    task.projectId = QStringLiteral("Demo");
    task.stage = QStringLiteral("CODER");
    task.category = QStringLiteral("utility");
    task.title = QStringLiteral("Fitur baru");
    task.branch.name = QStringLiteral("fitur");
    task.branch.base = QStringLiteral("main");
    m_tasks->addTask(task);
    QTest::mouseClick(card(QStringLiteral("t9")), Qt::LeftButton);
    ResponseDrawer *panel = drawer();
    QVERIFY(panel && !panel->isHidden());
    auto *history = panel->findChild<QPushButton *>(QStringLiteral("btnDrawerBranchHistory"));
    QVERIFY(history && history->isVisibleTo(panel));
    history->click();
    QCOMPARE(m_window->findChildren<BranchViewer *>().size(), 1);
    QTRY_COMPARE_WITH_TIMEOUT(subjects(), QStringList({"Semua perubahan", "ubah fitur", "tambah fitur"}), kGitTimeoutMs);
    QCOMPARE(viewer->branch(), QStringLiteral("fitur"));
    QCOMPARE(viewer->compareTarget(), QStringLiteral("main"));

    // Memilih branch di jendela ini tidak men-checkout apa pun
    QCOMPARE(GitSandbox::output(dir.path(), {QStringLiteral("branch"), QStringLiteral("--show-current")}),
             QStringLiteral("main"));

    // Project ditutup: jendelanya ikut ditutup
    m_window->findChild<QPushButton *>(QStringLiteral("btnClose"))->click();
    QTRY_VERIFY(viewer.isNull());
}

void TestGui::promptEditorAttachesPhotosAndDocuments() {
    PromptEditor editor;
    editor.resize(480, 320);
    editor.show();
    QPlainTextEdit *text = editor.textEdit();
    QCOMPARE(text->objectName(), QStringLiteral("taskFormPrompt"));
    auto thumbs = [&editor]() { return shownChildren<QToolButton>(&editor, QStringLiteral("promptThumb")); };

    // Ctrl+V gambar dari clipboard: jadi thumbnail, bukan teks
    QGuiApplication::clipboard()->setImage(solidImage(QSize(300, 200), Qt::red));
    text->paste();
    QCOMPARE(editor.imageCount(), 1);
    QVERIFY(text->toPlainText().isEmpty());
    QCOMPARE(thumbs().size(), 1);
    QVERIFY(!thumbs().first()->icon().isNull());
    QVERIFY(!editor.findChild<QScrollArea *>(QStringLiteral("promptImageStrip"))->isHidden());

    // Teks biasa tetap ditempel sebagai teks
    QGuiApplication::clipboard()->setText(QStringLiteral("Halo agent"));
    text->paste();
    QCOMPARE(text->toPlainText(), QStringLiteral("Halo agent"));

    // Seret file dari Explorer: foto dan dokumen masuk; format lama ditolak dengan alasannya
    QTemporaryDir dir;
    const QString photo = dir.filePath(QStringLiteral("foto.jpg"));
    QVERIFY(writeFile(photo, jpegBytes(solidImage(QSize(120, 80), Qt::blue))));
    const QString csv = dir.filePath(QStringLiteral("data.csv"));
    QVERIFY(writeFile(csv, "produk,harga\nkopi,15000\n"));
    const QString xls = dir.filePath(QStringLiteral("laporan.xls"));
    QVERIFY(writeFile(xls, "lama"));
    const QString txt = dir.filePath(QStringLiteral("catatan.txt"));
    QVERIFY(writeFile(txt, "bukan lampiran"));

    QMimeData onlyText;
    onlyText.setUrls({QUrl::fromLocalFile(txt)});
    QVERIFY(!editor.canTakeAttachments(&onlyText));   // file lain tetap tertempel sebagai path

    QMimeData files;
    files.setUrls({QUrl::fromLocalFile(photo), QUrl::fromLocalFile(csv), QUrl::fromLocalFile(xls), QUrl::fromLocalFile(txt)});
    QVERIFY(editor.canTakeAttachments(&files));
    // Seperti seret sungguhan: Drop hanya diantar ke widget yang menerima DragEnter-nya
    QDragEnterEvent enter(QPoint(20, 20), Qt::CopyAction, &files, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&editor, &enter);
    QVERIFY(enter.isAccepted());
    QDropEvent drop(QPointF(20, 20), Qt::CopyAction, &files, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&editor, &drop);
    QVERIFY2(editor.imageCount() == 2, qPrintable(editor.notice()));
    QCOMPARE(editor.documentCount(), 1);
    QCOMPARE(editor.notice(), QStringLiteral("laporan.xls: Format lama .xls tidak didukung; simpan ulang sebagai .xlsx"));
    auto *hint = editor.findChild<QLabel *>(QStringLiteral("promptHint"));
    QCOMPARE(hint->text(), editor.notice());
    QVERIFY(hint->property("error").toBool());

    // Foto tempelan diberi nama otomatis; foto dari file mempertahankan namanya
    const QList<TaskAttachments::Draft> drafts = editor.attachments();
    QCOMPARE(int(drafts.size()), 3);
    QVERIFY(drafts.at(0).fileName.startsWith(QStringLiteral("tempel-")) && drafts.at(0).fileName.endsWith(QStringLiteral(".png")));
    QCOMPARE(drafts.at(1).fileName, QStringLiteral("foto.jpg"));
    QVERIFY(!drafts.at(1).imageData.isEmpty());
    QCOMPARE(drafts.at(2).fileName, QStringLiteral("data.csv"));
    QCOMPARE(drafts.at(2).sourcePath, QFileInfo(csv).absoluteFilePath());

    // Dokumen yang sama tidak dilampirkan dua kali; penambahan yang berhasil menghapus pesan lama
    QCOMPARE(editor.addFiles({csv}), 0);
    QCOMPARE(editor.documentCount(), 1);
    QVERIFY(editor.notice().isEmpty());
    QVERIFY(!hint->property("error").toBool());

    // Baris dokumen: label jenis, nama, ukuran
    const QList<QLabel *> kinds = shownChildren<QLabel>(&editor, QStringLiteral("promptDocumentKind"));
    QCOMPARE(kinds.size(), 1);
    QCOMPARE(kinds.first()->text(), QStringLiteral("CSV"));
    QCOMPARE(kinds.first()->property("kind").toString(), QStringLiteral("csv"));
    QCOMPARE(shownChildren<QLabel>(&editor, QStringLiteral("promptDocumentName")).first()->text(), QStringLiteral("data.csv"));

    // Klik thumbnail: pratinjau ukuran penuh; "Hapus foto" membuang foto itu
    bool previewShown = false;
    QTimer::singleShot(0, this, [&previewShown]() {
        auto *preview = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!preview) {
            return;
        }
        auto *image = preview->findChild<QLabel *>(QStringLiteral("imagePreview"));
        previewShown = preview->objectName() == QLatin1String("imagePreviewDialog")
                       && preview->windowTitle() == QLatin1String("foto.jpg")
                       && image && image->pixmap().size() == QSize(120, 80);
        preview->findChild<QPushButton *>(QStringLiteral("btnPreviewRemove"))->click();
    });
    thumbs().at(1)->click();
    QVERIFY(previewShown);
    QCOMPARE(editor.imageCount(), 1);
    QCOMPARE(editor.attachments().first().fileName, drafts.at(0).fileName);

    // "Tutup" di pratinjau tidak mengubah apa pun
    QTimer::singleShot(0, this, []() {
        if (auto *preview = qobject_cast<QDialog *>(QApplication::activeModalWidget())) {
            preview->findChild<QPushButton *>(QStringLiteral("btnPreviewClose"))->click();
        }
    });
    thumbs().first()->click();
    QCOMPARE(editor.imageCount(), 1);

    // Tombol × di pojok thumbnail dan di baris dokumen
    thumbs().first()->findChild<QToolButton *>(QStringLiteral("btnThumbRemove"))->click();
    QCOMPARE(editor.imageCount(), 0);
    QVERIFY(thumbs().isEmpty());
    QVERIFY(editor.findChild<QScrollArea *>(QStringLiteral("promptImageStrip"))->isHidden());
    shownChildren<QToolButton>(&editor, QStringLiteral("btnDocumentRemove")).first()->click();
    QCOMPARE(editor.documentCount(), 0);
    QVERIFY(editor.findChild<QWidget *>(QStringLiteral("promptDocumentList"))->isHidden());

    // Batas jumlah foto per task; nama tempelan yang sama tetap unik
    for (int i = 0; i < TaskAttachments::kMaxImages; ++i) {
        QVERIFY(editor.addImage(solidImage(QSize(8, 8), Qt::green)));
    }
    QVERIFY(!editor.addImage(solidImage(QSize(8, 8), Qt::green)));
    QCOMPARE(editor.notice(), QStringLiteral("maksimal %1 foto per task").arg(TaskAttachments::kMaxImages));
    QSet<QString> names;
    for (const TaskAttachments::Draft &draft : editor.attachments()) {
        names.insert(draft.fileName.toLower());
    }
    QCOMPARE(int(names.size()), TaskAttachments::kMaxImages);
}

void TestGui::newTaskWithAttachmentsFeedsRun() {
    QTemporaryDir source;
    const QString csv = source.filePath(QStringLiteral("data.csv"));
    QVERIFY(writeFile(csv, "produk,harga\nkopi,15000\n"));

    // "+" di baris project sidebar: prompt multi-baris, satu foto tempelan, satu CSV
    driveModalDialog([csv](QDialog *dialog) {
        QCOMPARE(dialog->windowTitle(), QStringLiteral("New Task"));
        dialog->findChild<QLineEdit *>(QStringLiteral("taskFormInput"))->setText(QStringLiteral("Login dari mockup"));
        auto *editor = dialog->findChild<PromptEditor *>();
        QVERIFY(editor);
        editor->setText(QStringLiteral("Tiru mockup terlampir.\nPakai harga dari data.csv."));
        QVERIFY(editor->addImage(solidImage(QSize(64, 48), Qt::red)));
        QCOMPARE(editor->addFiles({csv}), 1);
        dialog->findChildren<QComboBox *>(QStringLiteral("taskFormCombo")).at(1)->setCurrentText(QStringLiteral("CODER"));
        dialog->findChild<QPushButton *>(QStringLiteral("btnTaskFormCreate"))->click();
    });
    m_window->findChild<QPushButton *>(QStringLiteral("btnProjectNewTask"))->click();
    QVERIFY(m_dialogSeen);

    std::optional<TaskItem> created;
    for (const TaskItem &task : m_tasks->tasksForProject(QStringLiteral("Demo"))) {
        if (task.title == QLatin1String("Login dari mockup")) {
            created = task;
        }
    }
    QVERIFY(created);
    QCOMPARE(created->subtext, QStringLiteral("Tiru mockup terlampir.\nPakai harga dari data.csv."));
    QCOMPARE(int(created->attachments.size()), 2);
    QVERIFY(created->attachments.at(0).endsWith(QStringLiteral(".png")));
    QCOMPARE(created->attachments.at(1), QStringLiteral("data.csv"));

    // Lampiran disalin ke folder lampiran task; file asal tidak dipakai lagi
    const QDir attachments(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                           + QStringLiteral("/projects/Demo/attachments/") + created->id);
    QVERIFY(QFileInfo::exists(attachments.filePath(created->attachments.at(0))));
    QVERIFY(QFileInfo::exists(attachments.filePath(QStringLiteral("data.csv"))));
    QVERIFY(QFile::remove(csv));

    // Kartu: ringkasan prompt dan penanda lampiran
    KanbanCardWidget *mockup = card(created->id);
    QVERIFY(mockup);
    QCOMPARE(mockup->findChild<QLabel *>(QStringLiteral("labelSubtext"))->text(),
             QStringLiteral("Tiru mockup terlampir.\nPakai harga dari data.csv."));
    auto *attachmentLabel = mockup->findChild<QLabel *>(QStringLiteral("labelAttachments"));
    QVERIFY(!attachmentLabel->isHidden());
    QCOMPARE(attachmentLabel->text(), QStringLiteral("1 foto · 1 file"));
    QVERIFY(card(QStringLiteral("t1"))->findChild<QLabel *>(QStringLiteral("labelAttachments"))->isHidden());

    // Run: foto jadi blok gambar, isi CSV masuk prompt, folder lampiran boleh dibaca agent
    runButton(created->id)->click();
    FakeAgentSession *session = m_runtime.sessions.last();
    const AgentLaunch launch = session->launch();
    QCOMPARE(launch.imagePaths, QStringList({QDir::toNativeSeparators(attachments.filePath(created->attachments.at(0)))}));
    QCOMPARE(launch.readableDirectories, QStringList({QDir::toNativeSeparators(attachments.absolutePath())}));
    QVERIFY(launch.prompt.contains(QStringLiteral("# Instruksi\n\nTiru mockup terlampir.\nPakai harga dari data.csv.")));
    QVERIFY(launch.prompt.contains(QStringLiteral("## data.csv\n```csv\nproduk,harga\nkopi,15000\n")));
    QVERIFY(consoleText().contains(QStringLiteral("+ Lampiran (")));
    session->finishWith(successResult(QStringLiteral("Selesai")));

    // Setelah aplikasi dibuka lagi, lampiran tetap terbaca dari session.json
    m_window.reset();
    m_tasks.reset();
    createWindow();
    mockup = card(created->id);
    QVERIFY(mockup);
    QCOMPARE(mockup->findChild<QLabel *>(QStringLiteral("labelAttachments"))->text(), QStringLiteral("1 foto · 1 file"));

    // Edit: lampiran lama tampil di kotak prompt; foto dihapus, prompt dipanjangkan
    driveModalDialog([](QDialog *dialog) {
        auto *editor = dialog->findChild<PromptEditor *>();
        QVERIFY(editor);
        QCOMPARE(editor->imageCount(), 1);
        QCOMPARE(editor->documentCount(), 1);
        const QList<QToolButton *> thumbs = shownChildren<QToolButton>(editor, QStringLiteral("promptThumb"));
        QCOMPARE(thumbs.size(), 1);
        QVERIFY(!thumbs.first()->icon().isNull());
        thumbs.first()->findChild<QToolButton *>(QStringLiteral("btnThumbRemove"))->click();
        editor->setText(QStringLiteral("satu\ndua\n\ntiga\nempat\nlima"));
        dialog->findChild<QPushButton *>(QStringLiteral("btnTaskFormCreate"))->click();
    });
    QTest::mouseDClick(mockup, Qt::LeftButton);
    QVERIFY(m_dialogSeen);

    const std::optional<TaskItem> edited = m_tasks->task(created->id);
    QCOMPARE(edited->attachments, QStringList({"data.csv"}));
    QVERIFY(!QFileInfo::exists(attachments.filePath(created->attachments.at(0))));
    QCOMPARE(mockup->findChild<QLabel *>(QStringLiteral("labelAttachments"))->text(), QStringLiteral("1 file"));
    // Prompt panjang: kartu hanya menampilkan tiga baris pertama, lengkapnya di tooltip
    auto *subtext = mockup->findChild<QLabel *>(QStringLiteral("labelSubtext"));
    QCOMPARE(subtext->text(), QStringLiteral("satu\ndua\ntiga…"));
    QCOMPARE(subtext->toolTip(), QStringLiteral("satu\ndua\n\ntiga\nempat\nlima"));
}

void TestGui::referenceFoldersFeedRuns() {
    QTemporaryDir shared;
    const QString reference = QDir(shared.path()).filePath(QStringLiteral("design system (v2)"));
    QVERIFY(QDir().mkpath(reference));

    SwimlaneWidget *swimlane = m_window->findChild<SwimlaneWidget *>();
    auto *button = swimlane->findChild<QPushButton *>(QStringLiteral("btnReferenceDirs"));
    QVERIFY(button);
    QCOMPARE(button->text(), QStringLiteral("Referensi"));
    QVERIFY(!button->icon().isNull());
    // Referensi, bukan salinan: swimlane diganti saat jendela dibuat ulang di bawah
    auto shownPopup = [&swimlane]() -> QFrame * {
        const QList<QFrame *> popups = swimlane->findChildren<QFrame *>(QStringLiteral("referencePopup"));
        for (QFrame *popup : popups) {
            if (popup->isVisible()) {
                return popup;
            }
        }
        return nullptr;
    };
    // Pilih folder lewat popup "Referensi" -> "Tambah folder…" -> pemilih folder
    auto addThroughPopup = [&](const QString &dir) {
        button->click();
        QFrame *popup = shownPopup();
        QVERIFY(popup);
        driveNextModalDialog([dir](QDialog *dialog) {
            auto *picker = qobject_cast<QFileDialog *>(dialog);
            QVERIFY(picker);
            QVERIFY(picker->windowTitle().startsWith(QStringLiteral("Folder referensi untuk Demo")));
            picker->selectFile(dir);
            dialog->accept();   // QFileDialog::accept() memeriksa pilihan; lewat QDialog karena protected di sana
        });
        popup->findChild<QPushButton *>(QStringLiteral("btnReferenceAdd"))->click();
        QTRY_VERIFY(m_dialogSeen);
    };

    // Popup kosong
    button->click();
    QFrame *popup = shownPopup();
    QVERIFY(popup);
    QVERIFY(popup->findChild<QLabel *>(QStringLiteral("referencePopupEmpty")));
    popup->close();

    addThroughPopup(reference);
    QTRY_COMPARE(button->text(), QStringLiteral("Referensi · 1"));
    QVERIFY(button->toolTip().contains(QDir::toNativeSeparators(reference)));
    QVERIFY(consoleText().contains(QStringLiteral("[SYSTEM] Folder referensi Demo + %1").arg(QDir::toNativeSeparators(reference))));

    // Folder kerja sendiri (atau isinya) sudah terbaca agent: ditolak
    addThroughPopup(m_workDir.path());
    QTRY_VERIFY(consoleText().contains(QStringLiteral("tidak ditambahkan: sudah termasuk folder kerja Demo")));
    QCOMPARE(button->text(), QStringLiteral("Referensi · 1"));

    // Run: folder referensi boleh dibaca (izin Read), dan disebut di prompt
    runButton(QStringLiteral("t1"))->click();
    FakeAgentSession *session = m_runtime.sessions.last();
    QCOMPARE(session->launch().readableDirectories, QStringList({QDir::toNativeSeparators(reference)}));
    QVERIFY(session->launch().imagePaths.isEmpty());
    QVERIFY(session->launch().prompt.contains(
        QStringLiteral("# Folder referensi (hanya dibaca)\n")));
    QVERIFY(session->launch().prompt.contains(QStringLiteral("- `%1`").arg(QDir::toNativeSeparators(reference))));
    session->finishWith(successResult(QStringLiteral("Selesai")));

    // Tersimpan di session.json: masih ada setelah aplikasi dibuka lagi
    m_window.reset();
    m_tasks.reset();
    createWindow();
    swimlane = m_window->findChild<SwimlaneWidget *>();
    button = swimlane->findChild<QPushButton *>(QStringLiteral("btnReferenceDirs"));
    QCOMPARE(button->text(), QStringLiteral("Referensi · 1"));

    // × di popup menghapus folder; popup yang sama tetap terbuka dengan daftar yang baru
    button->click();
    popup = shownPopup();
    QVERIFY(popup);
    QCOMPARE(shownChildren<QLabel>(popup, QStringLiteral("referenceRowName")).first()->text(),
             QStringLiteral("design system (v2)"));
    shownChildren<QToolButton>(popup, QStringLiteral("btnReferenceRemove")).first()->click();
    QCOMPARE(button->text(), QStringLiteral("Referensi"));
    QCOMPARE(shownPopup(), popup);
    QVERIFY(shownChildren<QToolButton>(popup, QStringLiteral("btnReferenceRemove")).isEmpty());
    QCOMPARE(shownChildren<QLabel>(popup, QStringLiteral("referencePopupEmpty")).size(), 1);
    // Tetap terbuka setelah event berikutnya diproses (tombol × lama baru benar-benar dihapus di sini)
    QTest::qWait(50);
    QCOMPARE(shownPopup(), popup);
    popup->close();
    QVERIFY(consoleText().contains(QStringLiteral("[SYSTEM] Folder referensi Demo − %1").arg(QDir::toNativeSeparators(reference))));

    runButton(QStringLiteral("t3"))->click();
    QVERIFY(m_runtime.sessions.last()->launch().readableDirectories.isEmpty());
    QVERIFY(!m_runtime.sessions.last()->launch().prompt.contains(QStringLiteral("Folder referensi")));
}

void TestGui::longProjectNameKeepsRowButtonsVisible() {
    const QString longName = QStringLiteral("Project Dengan Nama Yang Sangat Panjang Sekali Untuk Sidebar");
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QVERIFY(QDir().mkpath(base + QStringLiteral("/projects/") + longName));
    QJsonObject root;
    root[QStringLiteral("schemaVersion")] = 1;
    root[QStringLiteral("projectId")] = longName;
    root[QStringLiteral("tasks")] = QJsonArray{};
    QVERIFY(writeFile(base + QStringLiteral("/projects/") + longName + QStringLiteral("/session.json"),
                      QJsonDocument(root).toJson()));

    m_window.reset();
    m_tasks.reset();
    createWindow();
    m_window->resize(1200, 700);

    auto *list = m_window->findChild<QListWidget *>(QStringLiteral("projectList"));
    QVERIFY(list);
    QWidget *row = nullptr;
    for (int i = 0; i < list->count(); ++i) {
        if (list->item(i)->data(Qt::UserRole).toString() == longName) {
            row = list->itemWidget(list->item(i));
        }
    }
    QVERIFY(row);

    // Tombol baris harus utuh di dalam area tampil daftar, sesempit apa pun sidebar-nya
    const QRect viewport = list->viewport()->rect();
    for (const QString &name : {QStringLiteral("btnProjectDelete"), QStringLiteral("btnProjectNewTask")}) {
        auto *button = row->findChild<QPushButton *>(name);
        QVERIFY(button);
        const QRect rect(button->mapTo(list->viewport(), QPoint(0, 0)), button->size());
        QTRY_VERIFY2(viewport.contains(rect), qPrintable(name));
    }

    // Nama dipotong, nama lengkap tersedia lewat tooltip
    auto *label = row->findChild<QLabel *>(QStringLiteral("projectRowLabel"));
    QVERIFY(label);
    QTRY_VERIFY(label->text().endsWith(QChar(0x2026)));
    QCOMPARE(label->toolTip(), longName);
}

QDialog *TestGui::runtimeNotice() const {
    const QList<QDialog *> dialogs = m_window->findChildren<QDialog *>(QStringLiteral("runtimeNoticeDialog"));
    for (QDialog *dialog : dialogs) {
        if (dialog->isVisible()) {
            return dialog;
        }
    }
    return nullptr;
}

void TestGui::runtimeCheckShowsNotice_data() {
    QTest::addColumn<int>("status");
    QTest::addColumn<QString>("title");
    QTest::addColumn<QString>("url");
    QTest::newRow("belum terpasang") << int(RuntimeCheck::Status::Missing) << QStringLiteral("belum terpasang")
                                     << QStringLiteral("https://example.test/setup");
    QTest::newRow("perlu update") << int(RuntimeCheck::Status::Outdated) << QStringLiteral("perlu diperbarui")
                                  << QStringLiteral("https://example.test/update");
    QTest::newRow("belum login") << int(RuntimeCheck::Status::LoggedOut) << QStringLiteral("belum login")
                                 << QStringLiteral("https://example.test/auth");
}

void TestGui::runtimeCheckShowsNotice() {
    QFETCH(int, status);
    QFETCH(QString, title);
    QFETCH(QString, url);

    RuntimeCheck check = RuntimeCheck::of(RuntimeCheck::Status(status), QStringLiteral("detail dari CLI"),
                                          QStringLiteral("2.0.1"));
    check.helpUrl = url;
    m_runtime.checkResult = check;
    m_window->checkRuntime();

    QTRY_VERIFY(runtimeNotice());
    QDialog *dialog = runtimeNotice();
    QVERIFY(dialog->findChild<QLabel *>(QStringLiteral("runtimeNoticeTitle"))->text().contains(title));
    QVERIFY(dialog->findChild<QLabel *>(QStringLiteral("runtimeNoticeMessage"))->text().contains(
        QStringLiteral("detail dari CLI")));
    QCOMPARE(dialog->property("helpUrl").toString(), url);
    QVERIFY(dialog->findChild<QPushButton *>(QStringLiteral("btnRuntimeNoticeOpen")));

    // Batal menutup notice tanpa membuka apa pun
    dialog->findChild<QPushButton *>(QStringLiteral("btnRuntimeNoticeCancel"))->click();
    QTRY_VERIFY(!runtimeNotice());
}

void TestGui::readyRuntimeShowsNoNotice() {
    m_window->checkRuntime();
    QTest::qWait(200);
    QVERIFY(!runtimeNotice());
}

void TestGui::failedRunShowsLoginNoticeOnce() {
    m_runtime.failureDiagnosis = RuntimeCheck::of(RuntimeCheck::Status::LoggedOut,
                                                  QStringLiteral("Invalid API key · Please run /login"));

    runButton(QStringLiteral("t1"))->click();
    m_runtime.sessions.last()->finishWith(
        AgentResult::failure(QStringLiteral("no_result"), QStringLiteral("Invalid API key · Please run /login")));
    QTRY_VERIFY(runtimeNotice());
    QVERIFY(runtimeNotice()->findChild<QLabel *>(QStringLiteral("runtimeNoticeTitle"))->text().contains(
        QStringLiteral("belum login")));

    // Gagal lagi selagi notice masih terbuka: tidak menumpuk notice kedua
    runButton(QStringLiteral("t1"))->click();
    m_runtime.sessions.last()->finishWith(
        AgentResult::failure(QStringLiteral("no_result"), QStringLiteral("Invalid API key · Please run /login")));
    QTest::qWait(50);
    int shown = 0;
    for (QDialog *dialog : m_window->findChildren<QDialog *>(QStringLiteral("runtimeNoticeDialog"))) {
        shown += dialog->isVisible() ? 1 : 0;
    }
    QCOMPARE(shown, 1);
    runtimeNotice()->reject();
}

void TestGui::unavailableRuntimeShowsInstallNotice() {
    m_runtime.available = false;
    m_runtime.failureDiagnosis = RuntimeCheck::of(RuntimeCheck::Status::Missing, QStringLiteral("claude tidak ditemukan"));

    runButton(QStringLiteral("t1"))->click();
    QTRY_VERIFY(runtimeNotice());
    QVERIFY(runtimeNotice()->findChild<QLabel *>(QStringLiteral("runtimeNoticeTitle"))->text().contains(
        QStringLiteral("belum terpasang")));
    runtimeNotice()->reject();
}

void TestGui::finishedStageMarksCardDone() {
    auto doneLabel = [this](const QString &id) {
        KanbanCardWidget *target = card(id);
        return target ? target->findChild<QLabel *>(QStringLiteral("labelDone")) : nullptr;
    };
    QVERIFY(doneLabel(QStringLiteral("t1")));
    QVERIFY(!doneLabel(QStringLiteral("t1"))->isVisibleTo(card(QStringLiteral("t1"))));

    // CODER selesai -> kartu maju ke CLEANER dengan penanda stage yang baru selesai
    runButton(QStringLiteral("t1"))->click();
    m_runtime.sessions.last()->finishWith(successResult(QStringLiteral("Selesai")));
    QTRY_COMPARE(card(QStringLiteral("t1"))->stage(), QStringLiteral("CLEANER"));
    QTRY_VERIFY(doneLabel(QStringLiteral("t1"))->isVisibleTo(card(QStringLiteral("t1"))));
    QVERIFY(doneLabel(QStringLiteral("t1"))->text().contains(QStringLiteral("CODER")));
    QCOMPARE(card(QStringLiteral("t1"))->property("done").toBool(), true);

    // Run berikutnya mulai: penanda hilang selama agent bekerja
    runButton(QStringLiteral("t1"))->click();
    QTRY_VERIFY(!doneLabel(QStringLiteral("t1"))->isVisibleTo(card(QStringLiteral("t1"))));
    QCOMPARE(card(QStringLiteral("t1"))->property("done").toBool(), false);

    // WAITING tidak pernah diberi penanda
    QVERIFY(!doneLabel(QStringLiteral("t2"))->isVisibleTo(card(QStringLiteral("t2"))));
}

void TestGui::themeMapsTextAndFillSeparately() {
    const QString light = QStringLiteral(
        "QWidget#a { background-color: #ffffff; color: #3d3730; }\n"
        "QPushButton:hover { color: #ffffff; border: 1px solid #E7DDCD; }\n");
    QCOMPARE(Theme::styleSheetFor(light, Theme::Scheme::Light), light);

    const QString dark = Theme::styleSheetFor(light, Theme::Scheme::Dark);
    QVERIFY2(dark.contains(QStringLiteral("background-color: #222d40")), qPrintable(dark));
    QVERIFY2(dark.contains(QStringLiteral("color: #dbe3ef")), qPrintable(dark));
    // Putih sebagai teks (di atas tombol berwarna) tetap putih
    QVERIFY2(dark.contains(QStringLiteral("hover { color: #ffffff")), qPrintable(dark));
    QVERIFY2(dark.contains(QStringLiteral("1px solid #34435c")), qPrintable(dark));
    QVERIFY(dark.contains(QStringLiteral("QPushButton:hover {")));

    Theme::setSchemeOverride(Theme::Scheme::Dark);
    QCOMPARE(Theme::text(0x33517a).name(), QStringLiteral("#9dbbe6"));
    QCOMPARE(Theme::fill(0xffffff).name(), QStringLiteral("#222d40"));
    Theme::setSchemeOverride(Theme::Scheme::Light);
    QCOMPARE(Theme::text(0x33517a).name(), QStringLiteral("#33517a"));
    Theme::setSchemeOverride(std::nullopt);
}

void TestGui::themeDarkStyleSheetHasNoLightBackgrounds() {
    QFile file(QStringLiteral(":/styles.qss"));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QString dark = Theme::styleSheetFor(QString::fromUtf8(file.readAll()), Theme::Scheme::Dark);

    // Latar terang (kecerahan tinggi) yang tersisa di mode gelap berarti warna yang lupa dipetakan
    static const QRegularExpression background(
        QStringLiteral("background(?:-color)?\\s*:[^;]*#([0-9a-fA-F]{6})"));
    QStringList leftovers;
    QRegularExpressionMatchIterator it = background.globalMatch(dark);
    while (it.hasNext()) {
        const QString hex = it.next().captured(1);
        if (QColor(QStringLiteral("#") + hex).lightness() > 170) {
            leftovers.append(hex);
        }
    }
    leftovers.removeDuplicates();
    QVERIFY2(leftovers.isEmpty(), qPrintable(leftovers.join(QStringLiteral(", "))));
}

void TestGui::themeAppliesAndFollowsOverride() {
    const auto restore = qScopeGuard([]() {
        Theme::setSchemeOverride(std::nullopt);
        qApp->setStyleSheet(QString());
    });

    int changes = 0;
    const QMetaObject::Connection connection = connect(Theme::Notifier::instance(), &Theme::Notifier::changed,
                                                       this, [&changes]() { ++changes; });
    const auto disconnectGuard = qScopeGuard([connection]() { QObject::disconnect(connection); });

    Theme::setSchemeOverride(Theme::Scheme::Dark);
    QVERIFY(Theme::apply(*qApp));
    QVERIFY(qApp->styleSheet().contains(QStringLiteral("#222d40")));
    QCOMPARE(changes, 1);
    // Bagian tanpa stylesheet (viewport, splitter) ikut gelap lewat palet, bukan palet bawaan Windows
    QCOMPARE(QApplication::palette().color(QPalette::Base).name(), QStringLiteral("#222d40"));
    QCOMPARE(QApplication::palette().color(QPalette::Window).name(), QStringLiteral("#1b2433"));

    Theme::setSchemeOverride(Theme::Scheme::Light);
    QVERIFY(Theme::apply(*qApp));
    QVERIFY(!qApp->styleSheet().contains(QStringLiteral("#222d40")));
    QCOMPARE(changes, 2);
    QCOMPARE(QApplication::palette().color(QPalette::Base).name(), QStringLiteral("#ffffff"));
    QCOMPARE(QApplication::palette().color(QPalette::ButtonText).name(), QStringLiteral("#3d3730"));
}

void TestGui::themedIconRecolorsInDarkMode() {
    const auto restore = qScopeGuard([]() { Theme::setSchemeOverride(std::nullopt); });
    const QIcon icon = Theme::icon(QStringLiteral(":/icons/plus.svg"));

    Theme::setSchemeOverride(Theme::Scheme::Light);
    const QImage light = icon.pixmap(QSize(32, 32)).toImage();
    Theme::setSchemeOverride(Theme::Scheme::Dark);
    const QImage dark = icon.pixmap(QSize(32, 32)).toImage();
    QVERIFY(!light.isNull() && !dark.isNull());
    QVERIFY(light != dark);

    // Garis coklat ikon "+" menjadi amber di mode gelap
    auto hasColor = [](const QImage &image, const QColor &color) {
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                const QColor pixel = image.pixelColor(x, y);
                if (pixel.alpha() == 255 && pixel.rgb() == color.rgb()) {
                    return true;
                }
            }
        }
        return false;
    };
    QVERIFY(hasColor(light, QColor(0xa9, 0x74, 0x3f)));
    QVERIFY(hasColor(dark, QColor(0xe0, 0xb0, 0x7a)));
}

void TestGui::themeAdaptsWidgetStyleSheets() {
    const auto restore = qScopeGuard([]() {
        Theme::setSchemeOverride(Theme::Scheme::Light);
        Theme::apply(*qApp);
        Theme::setSchemeOverride(std::nullopt);
        qApp->setStyleSheet(QString());
    });
    Theme::setSchemeOverride(Theme::Scheme::Dark);
    QVERIFY(Theme::install(*qApp));

    // Widget yang dibuat sesudah tema terpasang
    QWidget widget;
    widget.setStyleSheet(QStringLiteral("background-color: #f7f9f8; color: #111827;"));
    widget.show();
    QTRY_VERIFY2(widget.styleSheet().contains(QStringLiteral("#1e2839")), qPrintable(widget.styleSheet()));
    QVERIFY(widget.styleSheet().contains(QStringLiteral("color: #dbe3ef")));

    // setStyleSheet berikutnya dari kode juga dipetakan
    widget.setStyleSheet(QStringLiteral("color: #a9743f;"));
    QCOMPARE(widget.styleSheet(), QStringLiteral("color: #e0b07a;"));

    // Jendela yang sudah ada: mis. top bar dari mainwindow.ui
    auto *topBar = m_window->findChild<QWidget *>(QStringLiteral("topNavBar"));
    QVERIFY(topBar);
    QVERIFY2(!topBar->styleSheet().contains(QStringLiteral("#f7f9f8")), qPrintable(topBar->styleSheet()));

    // Kembali ke terang: stylesheet asli dipulihkan
    Theme::setSchemeOverride(Theme::Scheme::Light);
    QVERIFY(Theme::apply(*qApp));
    QCOMPARE(widget.styleSheet(), QStringLiteral("color: #a9743f;"));
    QVERIFY(topBar->styleSheet().contains(QStringLiteral("#f7f9f8")));
}

int main(int argc, char *argv[]) {
    // Tanpa jendela sungguhan, dan AppData dialihkan ke lokasi khusus test
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QStandardPaths::setTestModeEnabled(true);

    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("LassomoirGuiTest"));

    TestGui test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_gui.moc"

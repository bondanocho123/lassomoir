#include "AgentAccess.h"
#include "AppFonts.h"
#include "BranchViewer.h"
#include "CanvasInspector.h"
#include "CanvasItems.h"
#include "CanvasLibrary.h"
#include "CanvasModel.h"
#include "CanvasPage.h"
#include "CanvasPreviewDrawer.h"
#include "CanvasView.h"
#include "CodeMetrics.h"
#include "ConsolePanelWidget.h"
#include "ConsoleTaskCard.h"
#include "DiagramViewer.h"
#include "DiffView.h"
#include "ElidedLabel.h"
#include "FakeAgentRuntime.h"
#include "FolderLauncher.h"
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
#include "SecretStore.h"
#include "SidePanelDock.h"
#include "StageCatalog.h"
#include "StageInfo.h"
#include "SwarmCoordinator.h"
#include "SwimlaneWidget.h"
#include "TaskAttachments.h"
#include "TaskManager.h"
#include "Theme.h"
#include "VerticalTabButton.h"
#include "WorkspaceDiff.h"
#include "mainwindow.h"

#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QDropEvent>
#include <QEnterEvent>
#include <QFile>
#include <QFileDialog>
#include <QFontDatabase>
#include <QGraphicsItem>
#include <QGraphicsView>
#include <QImage>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QShortcut>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextBrowser>
#include <QThreadPool>
#include <QTimer>
#include <QToolButton>
#include <QTransform>
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

public:
    // Penerima QDesktopServices::setUrlHandler: mencatat URL yang hendak dibuka di luar aplikasi
    Q_INVOKABLE void recordOpenedUrl(const QUrl &url) { m_openedUrls.append(url); }

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
    // Menu File / View / Help di baris paling atas jendela, mepet pojok kiri atas
    void menuBarSitsTopLeft();
    // Menu atas mengilap dengan garis bawah 1px; sidebar dan panel konsol berlatar sama (krem latar
    // yang lebih tua, mengilap) dan bergaris tepi 1px
    void chromeIsGlossyWithHairlines();
    // File > New Project: tanya nama, buat project + session.json, tampilkan di board
    void fileMenuCreatesProject();
    // File > Close Project: project yang tampil ditutup, datanya tetap di disk
    void fileMenuClosesProject();
    // File > Remove Project: setelah konfirmasi, project hilang dari UI sekaligus dari disk
    void fileMenuRemovesProject();
    // View > Source Control: jendela branch & commit project yang tampil (folder kerja repository git)
    void viewMenuOpensSourceControl();
    // View > Show in Explorer / Show in Terminal / Change Folder: folder kerja project yang tampil
    void viewMenuOpensAndChangesWorkingFolder();

    // Hover judul atau ikon info di tiap kolom stage memunculkan penjelasan tugas dan fitur stage itu
    // seketika, tanpa berkedip saat kursor berpindah antara keduanya
    void stageColumnsExplainThemselves();
    // Kolom stage selebar 300px (3/4 dari 400px semula)
    void stageColumnsAreThreeQuartersWide();

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
    // Sidebar menyempit setelah tampil pertama (ukuran splitter awal): baris ikut menyempit tanpa digeser
    void rowButtonsFollowSidebarWidth();
    // Top bar sudah tidak ada: tombol sidebar di pojok kanan atas sidebar, sejajar judul PROJECTS
    void sidebarButtonSitsInProjectsHeader();
    // Sidebar disembunyikan: tombolnya tetap ada di rel, hover menampilkan sidebar menimpa board
    // tanpa menggesernya, klik memasangnya kembali
    void hiddenSidebarKeepsButtonAndPeeksOnHover();

    // Panel Lieutenant: tombol × menyembunyikannya, View > Lieutenant memunculkannya lagi
    void lieutenantPanelClosesAndReturnsFromViewMenu();
    // Pin dilepas: tinggal tab tegak "Lieutenant" di rel kanan, caption diputar 90° ke kiri. Hover tab
    // menampilkan panel menimpa board tanpa menggesernya; tombol pin memasangnya kembali
    void lieutenantPanelUnpinsToSideTab();
    // Tab diklik tanpa kursor di atasnya (keyboard): panel bertahan sampai diklik lagi. Panel yang
    // disembunyikan selagi berupa tab muncul lagi sebagai tab
    void lieutenantTabTogglesOnClickAndSurvivesHiding();
    // Panel yang tampil sementara bertahan selama seretan yang berawal di atasnya (seleksi teks,
    // scrollbar), tetapi tidak ditahan seretan yang berawal di luar (kartu kanban)
    void lieutenantFlyoutSurvivesDragStartedInside();

    // Kanvas brainstorm: ▶ / ■ di toolbar tinggal ikon; Catatan dan Langkah AI jadi tombol bulat yang
    // mengambang di pojok kiri atas kanvas, zoom dan Paskan di pojok kiri bawahnya
    void canvasFloatsCreateAndZoomButtons();
    // Halaman kanvas muat di antara sidebar dan panel Lieutenant yang terpasang: jendela tidak dipaksa
    // lebih lebar dari ukurannya (isi yang terpotong saat jendela dimaksimalkan)
    void canvasFitsBesidePinnedLieutenant();
    // Tombol Pratinjau di panel detail: isi kartu terpilih sebagai Markdown jadi di drawer yang
    // menimpa kanvas dari tepi kanannya, mengikuti ketikan dan pilihan kartu
    void canvasPreviewsMarkdownInDrawer();
    // Garis menempel di sisi kartu yang menghadap tujuannya (bukan selalu kanan ke kiri), menghindari
    // lorong yang diisi kartu lain, dan ditata ulang saat kartu mana pun berpindah
    void canvasEdgesAttachToFacingSides();
    // Titik sambung ada di keempat sisi kartu: garis baru bisa ditarik dari sisi mana pun
    void canvasConnectsFromAnySide();
    // Kartu menampilkan teks yang masih terbaca di zoom berapa pun: yang tidak muat berakhir elipsis,
    // dan titik sambung serta daerah klik garis tidak ikut mengecil
    void canvasCardsShowWhatFitsAtAnyZoom();

    // Notice Claude Code: belum terpasang / perlu update / belum login, dengan tombol buka link atau batal
    void runtimeCheckShowsNotice_data();
    void runtimeCheckShowsNotice();
    void readyRuntimeShowsNoNotice();
    void failedRunShowsLoginNoticeOnce();
    void unavailableRuntimeShowsInstallNotice();
    // Notice "API key bermasalah": tombolnya membuka File > Integrations, bukan halaman panduan
    void badApiKeyNoticeOpensIntegrations();

    // File > Integrations: akses agent lewat login Claude Code atau API key Anthropic
    void integrationsSavesApiKeyAndReturnsToLogin();
    void integrationsRunsAccountLogin();

    // Kartu yang stage-nya selesai diberi penanda "✓ Selesai"
    void finishedStageMarksCardDone();

    // Tema ikut mode sistem: gelap = "blue night" yang diturunkan dari styles.qss
    void themeMapsTextAndFillSeparately();
    void themeDarkStyleSheetHasNoLightBackgrounds();
    void themeAppliesAndFollowsOverride();
    void themedIconRecolorsInDarkMode();
    // Stylesheet milik widget (dari .ui / setStyleSheet) ikut dipetakan, dan kembali saat mode terang
    void themeAdaptsWidgetStyleSheets();

    // Font open-source tertanam; bawaan tetap font yang sekarang
    void bundledFontsAreRegistered();
    // File > Preferences -> pilih font -> diterapkan dan diingat
    void preferencesChangesFont();

private:
    // Item menu File berdasarkan objectName-nya (actionNewProject / actionCloseProject / actionRemoveProject)
    QAction *fileAction(const QString &name) const;
    // Sidebar dan project yang tampil di board
    QListWidget *projectList() const;
    QString shownProject() const;
    QString projectFile(const QString &projectId) const;
    // Dialog notice runtime yang sedang tampil; nullptr bila tidak ada
    QDialog *runtimeNotice() const;
    // Dialog Integrations yang sedang tampil; nullptr bila tidak ada
    QDialog *integrationsDialog() const;
    // File > Integrations, lalu dialog yang muncul
    QDialog *openIntegrations();
    // Dialog modal membuka event loop sendiri di dalam handler klik; timer ini jalan di loop itu
    // dan mengisi/menutup dialog. m_dialogSeen tetap false bila dialog tidak pernah muncul.
    void driveModalDialog(std::function<void(QDialog *)> action);
    // Sama, untuk dialog yang baru muncul beberapa siklus event kemudian (mis. pemilih folder
    // yang dibuka lewat QTimer::singleShot): dicari berulang sampai ketemu
    void driveNextModalDialog(std::function<void(QDialog *)> action);
    bool m_dialogSeen = false;
    QList<QUrl> m_openedUrls;

    // Rakit TaskManager + MainWindow seperti main.cpp (termasuk runFinished -> recordRun);
    // size valid = ukuran jendela dipasang sebelum tampil, seperti main.cpp
    void createWindow(const QSize &size = QSize());
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
    m_runtime.accountStatus = AccountStatus();
    m_runtime.logins.clear();
    m_mermaid.requests.clear();
    m_swarm = std::make_unique<SwarmCoordinator>(m_catalog, m_runtime, m_composer);
    createWindow();
}

void TestGui::createWindow(const QSize &size) {
    m_tasks = std::make_unique<TaskManager>(m_catalog);
    TaskManager *tasks = m_tasks.get();
    connect(m_swarm.get(), &SwarmCoordinator::runFinished, tasks,
            [tasks](const TaskItem &task, const AgentResult &result) {
        tasks->recordRun(task.id, StageRun::finished(task.stage, result));
    });
    m_window = std::make_unique<MainWindow>(m_catalog, *m_tasks, *m_swarm, m_runtime, m_mermaid);
    if (size.isValid()) {
        m_window->resize(size);
    }
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

void TestGui::menuBarSitsTopLeft() {
    // Tampilan menu bergantung pada styles.qss, yang biasanya tidak dimuat test
    QFile qss(QStringLiteral(":/styles.qss"));
    QVERIFY(qss.open(QIODevice::ReadOnly));
    qApp->setStyleSheet(QString::fromUtf8(qss.readAll()));
    const auto resetStyle = qScopeGuard([]() { qApp->setStyleSheet(QString()); });

    auto *bar = m_window->findChild<QMenuBar *>(QStringLiteral("topMenuBar"));
    QVERIFY(bar);
    // Baris menu jendela itu sendiri: di atas isi jendela, "File" mepet pojok kiri atas
    QCOMPARE(m_window->menuWidget(), bar);
    auto *content = m_window->findChild<QWidget *>(QStringLiteral("contentWidget"));
    QVERIFY(content);
    // Tata letak disusun ulang setelah stylesheet berganti
    QTRY_VERIFY(bar->isVisible() && content->mapTo(m_window.get(), QPoint(0, 0)).y() > bar->geometry().bottom());
    QCOMPARE(bar->geometry().topLeft(), QPoint(0, 0));

    auto entries = [](const QMenu *menu) {
        QStringList texts;
        const QList<QAction *> actions = menu->actions();
        for (const QAction *action : actions) {
            texts.append(action->isSeparator() ? QStringLiteral("---") : action->text());
        }
        return texts;
    };
    const QList<QAction *> menus = bar->actions();
    QCOMPARE(menus.size(), 3);
    QCOMPARE(menus.at(0)->text(), QStringLiteral("&File"));
    QCOMPARE(entries(menus.at(0)->menu()),
             (QStringList{QStringLiteral("New Project"), QStringLiteral("Close Project"),
                          QStringLiteral("Remove Project"), QStringLiteral("---"),
                          QStringLiteral("Integrations…"), QStringLiteral("Preferences…"),
                          QStringLiteral("---"), QStringLiteral("Exit")}));
    QCOMPARE(menus.at(1)->text(), QStringLiteral("&View"));
    QCOMPARE(entries(menus.at(1)->menu()),
             (QStringList{QStringLiteral("Kanvas Brainstorm"), QStringLiteral("Lieutenant"), QStringLiteral("---"),
                          QStringLiteral("Source Control"), QStringLiteral("Show in Explorer"),
                          QStringLiteral("Show in Terminal"), QStringLiteral("---"),
                          QStringLiteral("Change Folder…")}));
    QCOMPARE(menus.at(2)->text(), QStringLiteral("&Help"));
    QCOMPARE(entries(menus.at(2)->menu()),
             (QStringList{QStringLiteral("Report Issue"), QStringLiteral("Tutorial (Tips && Tricks)"),
                          QStringLiteral("---"), QStringLiteral("About")}));
    QCOMPARE(bar->actionGeometry(menus.at(0)).left(), 0);

    // Sorotan hover selebar menunya, juga di menu satu item (View)
    for (QAction *entry : menus) {
        QMenu *menu = entry->menu();
        const QList<QAction *> actions = menu->actions();
        for (QAction *action : actions) {
            if (action->isSeparator()) continue;
            const QRect item = menu->actionGeometry(action);
            QVERIFY2(menu->sizeHint().width() - 1 - item.right() <= item.left(),
                     qPrintable(QStringLiteral("%1: item %2..%3, menu %4").arg(action->text())
                                    .arg(item.left()).arg(item.right()).arg(menu->sizeHint().width())));
        }
    }
}

void TestGui::chromeIsGlossyWithHairlines() {
    QFile qss(QStringLiteral(":/styles.qss"));
    QVERIFY(qss.open(QIODevice::ReadOnly));
    qApp->setStyleSheet(QString::fromUtf8(qss.readAll()));
    const auto resetStyle = qScopeGuard([]() { qApp->setStyleSheet(QString()); });

    auto *bar = m_window->findChild<QMenuBar *>(QStringLiteral("topMenuBar"));
    auto *sidebar = m_window->findChild<QWidget *>(QStringLiteral("sidebarPanel"));
    ConsolePanelWidget *console = consolePanel();
    QVERIFY(bar && sidebar && console);
    QTest::qWait(50);   // ukuran splitter awal sudah diterapkan
    QTRY_VERIFY(sidebar->isVisible() && console->isVisible() && sidebar->height() == console->height());

    const QImage image = m_window->grab().toImage();
    auto lightness = [this, &image](const QWidget *widget, const QPoint &local) {
        return image.pixelColor(widget->mapTo(m_window.get(), local)).lightness();
    };
    auto color = [this, &image](const QWidget *widget, const QPoint &local) {
        return image.pixelColor(widget->mapTo(m_window.get(), local));
    };
    const int page = QColor(QStringLiteral("#f8f4ee")).lightness();

    // Menu atas: di kanan item menu, separuh atas lebih terang dari separuh bawah, dan baris
    // terbawahnya garis pembatas yang lebih tua dari keduanya
    const int barX = bar->width() - 10;
    const int barTop = lightness(bar, QPoint(barX, 1));
    const int barLow = lightness(bar, QPoint(barX, bar->height() - 4));
    const int barRule = lightness(bar, QPoint(barX, bar->height() - 1));
    QVERIFY2(barTop > barLow, qPrintable(QStringLiteral("%1 vs %2").arg(barTop).arg(barLow)));
    QVERIFY2(barRule < barLow, qPrintable(QStringLiteral("%1 vs %2").arg(barRule).arg(barLow)));

    // Sidebar dan panel konsol: latar sama persis, lebih tua dari latar halaman, dengan kilau di atas
    const int middle = sidebar->height() / 2;
    QCOMPARE(color(sidebar, QPoint(4, middle)), color(console, QPoint(4, middle)));
    QCOMPARE(color(sidebar, QPoint(4, sidebar->height() - 12)), color(console, QPoint(4, console->height() - 12)));
    QVERIFY2(lightness(sidebar, QPoint(4, middle)) < page, qPrintable(color(sidebar, QPoint(4, middle)).name()));
    QVERIFY(lightness(sidebar, QPoint(12, 2)) > lightness(sidebar, QPoint(4, middle)));
    QVERIFY(lightness(console, QPoint(12, 2)) > lightness(console, QPoint(4, middle)));

    // Garis tepi 1px: piksel terluar lebih tua dari isi panel, piksel berikutnya sudah isi panel
    for (const QWidget *panel : {sidebar, static_cast<QWidget *>(console)}) {
        const int inside = lightness(panel, QPoint(4, middle));
        QVERIFY2(lightness(panel, QPoint(0, middle)) < inside, qPrintable(panel->objectName()));
        QVERIFY2(lightness(panel, QPoint(panel->width() - 1, middle)) < inside, qPrintable(panel->objectName()));
        QCOMPARE(color(panel, QPoint(1, middle)), color(panel, QPoint(4, middle)));
    }
}

QAction *TestGui::fileAction(const QString &name) const {
    return m_window->findChild<QAction *>(name);
}

QListWidget *TestGui::projectList() const {
    return m_window->findChild<QListWidget *>(QStringLiteral("projectList"));
}

QString TestGui::shownProject() const {
    auto *stack = m_window->findChild<QStackedWidget *>(QStringLiteral("boardStack"));
    auto *swimlane = stack ? qobject_cast<SwimlaneWidget *>(stack->currentWidget()) : nullptr;
    return swimlane ? swimlane->projectId() : QString();
}

QString TestGui::projectFile(const QString &projectId) const {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/projects/%1/session.json").arg(projectId);
}

void TestGui::fileMenuCreatesProject() {
    QAction *create = fileAction(QStringLiteral("actionNewProject"));
    QVERIFY(create);
    QListWidget *list = projectList();
    QVERIFY(list);
    QCOMPARE(list->count(), 1);

    const auto enterName = [this, create](const QString &name) {
        driveModalDialog([name](QDialog *dialog) {
            qobject_cast<QInputDialog *>(dialog)->setTextValue(name);
            dialog->accept();
        });
        create->trigger();
        QVERIFY(m_dialogSeen);
    };

    // Dibatalkan, atau nama kosong: tidak ada yang dibuat
    driveModalDialog([](QDialog *dialog) { dialog->reject(); });
    create->trigger();
    QVERIFY(m_dialogSeen);
    QCOMPARE(list->count(), 1);
    enterName(QStringLiteral("   "));
    QCOMPARE(list->count(), 1);

    enterName(QStringLiteral("  Baru  "));
    QCOMPARE(list->count(), 2);
    // Nama dipangkas, project baru langsung tampil di board dan terpilih di sidebar
    QCOMPARE(shownProject(), QStringLiteral("Baru"));
    QCOMPARE(list->currentItem()->data(Qt::UserRole).toString(), QStringLiteral("Baru"));
    QVERIFY(consoleText().contains(QStringLiteral("[SYSTEM] Project swimlane baru dibuat: Baru")));
    QTRY_VERIFY(QFileInfo::exists(projectFile(QStringLiteral("Baru"))));

    // Nama yang sudah dipakai tidak menggandakan project, hanya menampilkannya
    enterName(QStringLiteral("Demo"));
    QCOMPARE(list->count(), 2);
    QCOMPARE(shownProject(), QStringLiteral("Demo"));
    QVERIFY(consoleText().contains(QStringLiteral("Project 'Demo' sudah ada.")));

    // Dibuka lagi: project baru ikut termuat dari disk
    m_window.reset();
    m_tasks.reset();
    createWindow();
    QCOMPARE(projectList()->count(), 2);
}

void TestGui::fileMenuClosesProject() {
    QAction *close = fileAction(QStringLiteral("actionCloseProject"));
    QMenu *menu = qobject_cast<QMenu *>(close ? close->parent() : nullptr);
    QVERIFY(menu);
    QCOMPARE(shownProject(), QStringLiteral("Demo"));

    // Menu dibuka saat ada project yang tampil: item aktif
    menu->aboutToShow();
    QVERIFY(close->isEnabled());

    close->trigger();
    QCOMPARE(projectList()->count(), 0);
    QVERIFY(shownProject().isEmpty());
    QTRY_VERIFY(m_window->findChildren<KanbanCardWidget *>().isEmpty());
    QVERIFY2(consoleText().contains(QStringLiteral("[SYSTEM] Swimlane 'Demo' ditutup.")), qPrintable(consoleText()));
    // Beda dengan Remove: datanya tetap ada, project muncul lagi saat aplikasi dibuka
    QVERIFY(QFileInfo::exists(projectFile(QStringLiteral("Demo"))));

    // Board kosong: tidak ada yang bisa ditutup atau dihapus
    menu->aboutToShow();
    QVERIFY(!close->isEnabled());
    QVERIFY(!fileAction(QStringLiteral("actionRemoveProject"))->isEnabled());

    m_window.reset();
    m_tasks.reset();
    createWindow();
    QVERIFY(card(QStringLiteral("t1")));
}

void TestGui::fileMenuRemovesProject() {
    QAction *remove = fileAction(QStringLiteral("actionRemoveProject"));
    QMenu *menu = qobject_cast<QMenu *>(remove ? remove->parent() : nullptr);
    QVERIFY(menu);
    // Project kedua di disk: Remove hanya boleh menyentuh project yang tampil
    driveModalDialog([](QDialog *dialog) {
        qobject_cast<QInputDialog *>(dialog)->setTextValue(QStringLiteral("Lain"));
        dialog->accept();
    });
    fileAction(QStringLiteral("actionNewProject"))->trigger();
    QVERIFY(m_dialogSeen);
    QTRY_VERIFY(QFileInfo::exists(projectFile(QStringLiteral("Lain"))));
    m_window->setActiveProject(QStringLiteral("Demo"));
    QCOMPARE(shownProject(), QStringLiteral("Demo"));

    menu->aboutToShow();
    QVERIFY(remove->isEnabled());

    // Project yang tampil masih punya agent berjalan: ikut dihentikan
    runButton(QStringLiteral("t1"))->click();
    QVERIFY(m_runtime.sessions.first()->isRunning());

    // Batal (atau tutup dialog): tidak ada yang berubah
    const auto answer = [this, remove](const QString &button) {
        driveModalDialog([button](QDialog *dialog) {
            auto *box = qobject_cast<QMessageBox *>(dialog);
            QVERIFY(box);
            QVERIFY(box->text().contains(QStringLiteral("Hapus project \"Demo\"?")));
            const QList<QAbstractButton *> buttons = box->buttons();
            for (QAbstractButton *candidate : buttons) {
                if (candidate->text() == button) {
                    candidate->click();
                }
            }
        });
        remove->trigger();
        QVERIFY(m_dialogSeen);
    };
    answer(QStringLiteral("Batal"));
    QCOMPARE(projectList()->count(), 2);
    QVERIFY(QFileInfo::exists(projectFile(QStringLiteral("Demo"))));
    QVERIFY(m_runtime.sessions.first()->isRunning());

    // Enter di dialog = "Batal": default-nya bukan tombol penghapus
    driveModalDialog([](QDialog *dialog) {
        auto *box = qobject_cast<QMessageBox *>(dialog);
        QVERIFY(box);
        QVERIFY(box->defaultButton());
        QCOMPARE(box->defaultButton()->text(), QStringLiteral("Batal"));
        QVERIFY(box->escapeButton() == box->defaultButton());
        box->defaultButton()->click();
    });
    remove->trigger();
    QVERIFY(m_dialogSeen);
    QCOMPARE(projectList()->count(), 2);

    answer(QStringLiteral("Ya"));
    QCOMPARE(projectList()->count(), 1);
    QVERIFY(!m_runtime.sessions.first()->isRunning());
    QTRY_VERIFY(!card(QStringLiteral("t1")));
    QVERIFY2(consoleText().contains(QStringLiteral("[SYSTEM] Project 'Demo' dihapus permanen.")),
             qPrintable(consoleText()));
    QVERIFY(!QFileInfo::exists(projectFile(QStringLiteral("Demo"))));
    // Project lain tidak tersentuh dan gantian tampil di board
    QCOMPARE(shownProject(), QStringLiteral("Lain"));
    QVERIFY(QFileInfo::exists(projectFile(QStringLiteral("Lain"))));

    // Dibuka lagi: Demo tidak kembali
    m_window.reset();
    m_tasks.reset();
    createWindow();
    QCOMPARE(projectList()->count(), 1);
    QCOMPARE(shownProject(), QStringLiteral("Lain"));
    QVERIFY(!card(QStringLiteral("t1")));
}

void TestGui::viewMenuOpensSourceControl() {
    if (QStandardPaths::findExecutable(QStringLiteral("git")).isEmpty()) {
        QSKIP("git tidak ada di PATH");
    }
    const GitSandbox sandbox;
    constexpr int kGitTimeoutMs = 20000;

    QAction *sourceControl = fileAction(QStringLiteral("actionSourceControl"));
    QMenu *menu = qobject_cast<QMenu *>(sourceControl ? sourceControl->parent() : nullptr);
    QVERIFY(menu);

    // Folder kerja belum repository git: tidak ada riwayat untuk dilihat
    menu->aboutToShow();
    QVERIFY(!sourceControl->isEnabled());

    const QDir dir(m_workDir.path());
    auto removeRepository = qScopeGuard([dir]() {
        // Git yang masih membaca repository (thread pool) ditunggu dulu supaya foldernya bisa dihapus
        QThreadPool::globalInstance()->waitForDone(kGitTimeoutMs);
        QDir(dir.filePath(QStringLiteral(".git"))).removeRecursively();
        QFile::remove(dir.filePath(QStringLiteral("hello.txt")));
    });
    QVERIFY(runGit(dir.path(), {QStringLiteral("init"), QStringLiteral("-q"), QStringLiteral("-b"), QStringLiteral("main")}));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("hello.txt")), "a\n"));
    QVERIFY(runGit(dir.path(), {QStringLiteral("add"), QStringLiteral("-A")}));
    QVERIFY(runGit(dir.path(), {QStringLiteral("commit"), QStringLiteral("-q"), QStringLiteral("-m"), QStringLiteral("awal")}));

    menu->aboutToShow();
    QVERIFY(sourceControl->isEnabled());

    // Jendela yang sama dengan tombol branch di header swimlane, dimulai dari branch yang aktif
    sourceControl->trigger();
    QPointer<BranchViewer> viewer = BranchViewer::find(QStringLiteral("Demo"), m_window.get());
    QVERIFY(viewer && viewer->isVisible());
    QCOMPARE(viewer->windowTitle(), QStringLiteral("Branch & commit — Demo"));
    QTRY_COMPARE_WITH_TIMEOUT(viewer->branch(), QStringLiteral("main"), kGitTimeoutMs);
    auto *list = viewer->findChild<QTreeWidget *>(QStringLiteral("branchCommitList"));
    QVERIFY(list);
    QTRY_COMPARE_WITH_TIMEOUT(list->topLevelItemCount(), 1, kGitTimeoutMs);
    QCOMPARE(list->topLevelItem(0)->text(0), QStringLiteral("awal"));

    // Dipilih lagi: jendela yang sudah terbuka dimunculkan, bukan jendela kedua
    sourceControl->trigger();
    QCOMPARE(m_window->findChildren<BranchViewer *>().size(), 1);

    // Project ditutup: jendelanya ikut tertutup dan item menunya mati
    fileAction(QStringLiteral("actionCloseProject"))->trigger();
    QTRY_VERIFY(!viewer || !viewer->isVisible());
    menu->aboutToShow();
    QVERIFY(!sourceControl->isEnabled());
}

void TestGui::viewMenuOpensAndChangesWorkingFolder() {
    QAction *explorer = fileAction(QStringLiteral("actionShowInExplorer"));
    QAction *terminal = fileAction(QStringLiteral("actionShowInTerminal"));
    QAction *change = fileAction(QStringLiteral("actionChangeFolder"));
    QVERIFY(explorer && terminal && change);
    QMenu *menu = qobject_cast<QMenu *>(explorer->parent());
    QVERIFY(menu);

    // Tidak ada jendela yang benar-benar dibuka: File Explorer dan terminal dicegat di sini
    struct Launch {
        FolderLauncher::Command command;
        QString directory;
    };
    QList<Launch> launches;
    bool launchResult = true;
    m_openedUrls.clear();
    QDesktopServices::setUrlHandler(QStringLiteral("file"), this, "recordOpenedUrl");
    FolderLauncher::setProcessStarter([&launches, &launchResult](const FolderLauncher::Command &command,
                                                                  const QString &directory) {
        launches.append(Launch{command, directory});
        return launchResult;
    });
    const auto restore = qScopeGuard([]() {
        QDesktopServices::unsetUrlHandler(QStringLiteral("file"));
        FolderLauncher::setProcessStarter({});
    });

    // Demo punya folder kerja yang ada: ketiganya aktif
    const QString workDir = m_workDir.path();
    menu->aboutToShow();
    QVERIFY(explorer->isEnabled() && terminal->isEnabled() && change->isEnabled());

    explorer->trigger();
    QCOMPARE(m_openedUrls, QList<QUrl>{QUrl::fromLocalFile(workDir)});
    QVERIFY(launches.isEmpty());

    terminal->trigger();
    QCOMPARE(launches.size(), 1);
    QCOMPARE(launches.first().directory, workDir);
    QVERIFY(!launches.first().command.program.isEmpty());
    QVERIFY(launches.first().command == FolderLauncher::terminalCommand(workDir));
    QCOMPARE(m_openedUrls.size(), 1);
    QVERIFY2(!consoleText().contains(QStringLiteral("Gagal membuka")), qPrintable(consoleText()));

    // Terminal tidak bisa dijalankan: dicatat di konsol
    launchResult = false;
    terminal->trigger();
    QVERIFY2(consoleText().contains(QStringLiteral("[SYSTEM] Gagal membuka folder kerja Demo di terminal: %1")
                                        .arg(QDir::toNativeSeparators(workDir))),
             qPrintable(consoleText()));
    launchResult = true;

    // Change Folder…: pemilih folder yang sama dengan tombol folder di header swimlane
    QTemporaryDir other;
    QVERIFY(other.isValid());
    driveNextModalDialog([&other](QDialog *dialog) {
        auto *picker = qobject_cast<QFileDialog *>(dialog);
        QVERIFY(picker);
        QVERIFY(picker->windowTitle().startsWith(QStringLiteral("Folder kerja agent untuk Demo")));
        picker->selectFile(other.path());
        dialog->accept();   // QFileDialog::accept() memeriksa pilihan; lewat QDialog karena protected di sana
    });
    change->trigger();
    QVERIFY(m_dialogSeen);
    const QString changed = QDir::cleanPath(other.path());
    QVERIFY2(consoleText().contains(QStringLiteral("[SYSTEM] Folder kerja Demo: ")), qPrintable(consoleText()));

    // Explorer dan terminal kini membuka folder yang baru
    m_openedUrls.clear();
    launches.clear();
    explorer->trigger();
    terminal->trigger();
    QCOMPARE(m_openedUrls.size(), 1);
    QCOMPARE(QDir::cleanPath(m_openedUrls.first().toLocalFile()), changed);
    QCOMPARE(launches.size(), 1);
    QCOMPARE(QDir::cleanPath(launches.first().directory), changed);

    // Folder kerja hilang: tidak ada yang bisa dibuka, tapi masih bisa diganti
    QVERIFY(other.remove());
    menu->aboutToShow();
    QVERIFY(!explorer->isEnabled() && !terminal->isEnabled());
    QVERIFY(change->isEnabled());

    // Project ditutup: ketiganya mati
    fileAction(QStringLiteral("actionCloseProject"))->trigger();
    menu->aboutToShow();
    QVERIFY(!explorer->isEnabled() && !terminal->isEnabled() && !change->isEnabled());
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

void TestGui::stageColumnsAreThreeQuartersWide() {
    auto *swimlane = m_window->findChild<SwimlaneWidget *>();
    QVERIFY(swimlane);
    for (const QString &key : m_catalog.keys()) {
        KanbanColumnWidget *column = swimlane->column(key);
        QVERIFY2(column, qPrintable(key));
        QCOMPARE(column->minimumWidth(), 300);
        QCOMPARE(column->maximumWidth(), 300);
    }
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

    // Tombol baris harus utuh di dalam area tampil daftar, sesempit apa pun sidebar-nya. Dihitung
    // ulang tiap percobaan: lebar sidebar baru mapan setelah ukuran splitter awal diterapkan.
    for (const QString &name : {QStringLiteral("btnProjectDelete"), QStringLiteral("btnProjectNewTask")}) {
        auto *button = row->findChild<QPushButton *>(name);
        QVERIFY(button);
        QTRY_VERIFY2(list->viewport()->rect().contains(QRect(button->mapTo(list->viewport(), QPoint(0, 0)), button->size())),
                     qPrintable(name));
    }

    // Nama dipotong, nama lengkap tersedia lewat tooltip
    auto *label = row->findChild<QLabel *>(QStringLiteral("projectRowLabel"));
    QVERIFY(label);
    QTRY_VERIFY(label->text().endsWith(QChar(0x2026)));
    QCOMPARE(label->toolTip(), longName);
}

void TestGui::rowButtonsFollowSidebarWidth() {
    auto *list = m_window->findChild<QListWidget *>(QStringLiteral("projectList"));
    auto *splitter = m_window->findChild<QSplitter *>(QStringLiteral("mainSplitter"));
    QVERIFY(list && splitter && list->count() > 0);

    auto buttonsInside = [&list]() {
        const QRect viewport = list->viewport()->rect();
        for (int i = 0; i < list->count(); ++i) {
            QWidget *row = list->itemWidget(list->item(i));
            for (const QString &name : {QStringLiteral("btnProjectDelete"), QStringLiteral("btnProjectNewTask")}) {
                auto *button = row->findChild<QPushButton *>(name);
                if (!button || !viewport.contains(QRect(button->mapTo(list->viewport(), QPoint(0, 0)), button->size()))) {
                    return false;
                }
            }
        }
        return true;
    };

    // Lebar dulu (seperti sebelum ukuran splitter awal diterapkan), lalu menyempit tanpa event lain
    QList<int> sizes = splitter->sizes();
    sizes[0] = 240;
    splitter->setSizes(sizes);
    QTRY_VERIFY(buttonsInside());
    sizes = splitter->sizes();
    sizes[0] = 160;
    splitter->setSizes(sizes);
    QTRY_VERIFY(buttonsInside());

    // Persis seperti main.cpp: ukuran jendela dipasang sebelum tampil pertama, lalu konstruktor
    // menerapkan ukuran splitter awal di siklus event berikutnya
    m_window.reset();
    m_tasks.reset();
    createWindow(QSize(1440, 850));
    list = m_window->findChild<QListWidget *>(QStringLiteral("projectList"));
    splitter = m_window->findChild<QSplitter *>(QStringLiteral("mainSplitter"));
    QTest::qWait(50);   // ukuran splitter awal (QTimer::singleShot 0 di konstruktor) sudah diterapkan
    QTRY_VERIFY2_WITH_TIMEOUT(buttonsInside(), qPrintable(QStringLiteral("viewport %1, baris %2, visualRect %3, sidebar %4")
                                                              .arg(list->viewport()->width())
                                                              .arg(list->itemWidget(list->item(0))->width())
                                                              .arg(list->visualItemRect(list->item(0)).width())
                                                              .arg(splitter->sizes().value(0))),
                              1000);
}

void TestGui::sidebarButtonSitsInProjectsHeader() {
    // Top bar beserta wordmark dan tombol New Project-nya sudah dibuang
    QVERIFY(!m_window->findChild<QWidget *>(QStringLiteral("topNavBar")));
    QVERIFY(!m_window->findChild<QWidget *>(QStringLiteral("labelAppName")));
    QVERIFY(!m_window->findChild<QWidget *>(QStringLiteral("btnGlobalNewProject")));

    auto *panel = m_window->findChild<QWidget *>(QStringLiteral("sidebarPanel"));
    auto *title = m_window->findChild<QLabel *>(QStringLiteral("labelSidebarTitle"));
    auto *button = m_window->findChild<QPushButton *>(QStringLiteral("btnToggleSidebar"));
    QVERIFY(panel && title && button);
    QVERIFY(panel->isAncestorOf(button));
    QTRY_VERIFY(button->isVisible());

    // Sebaris dengan judul PROJECTS, menempel tepi kanan panel
    auto inPanel = [panel](const QWidget *widget) {
        return QRect(widget->mapTo(panel, QPoint(0, 0)), widget->size());
    };
    QTRY_VERIFY(inPanel(button).left() > inPanel(title).right());
    QVERIFY(qAbs(inPanel(button).center().y() - inPanel(title).center().y()) <= 1);
    QTRY_VERIFY2(panel->width() - inPanel(button).right() <= 12,
                 qPrintable(QStringLiteral("panel %1, tombol sampai %2").arg(panel->width()).arg(inPanel(button).right())));
}

void TestGui::hiddenSidebarKeepsButtonAndPeeksOnHover() {
    auto *splitter = m_window->findChild<QSplitter *>(QStringLiteral("mainSplitter"));
    auto *panel = m_window->findChild<QWidget *>(QStringLiteral("sidebarPanel"));
    auto *rail = m_window->findChild<QWidget *>(QStringLiteral("sidebarRail"));
    auto *hide = m_window->findChild<QPushButton *>(QStringLiteral("btnToggleSidebar"));
    auto *show = m_window->findChild<QPushButton *>(QStringLiteral("btnShowSidebar"));
    QListWidget *list = projectList();
    QVERIFY(splitter && panel && rail && hide && show && list);

    const QPoint cursorBefore = QCursor::pos();
    const auto restoreCursor = qScopeGuard([cursorBefore]() { QCursor::setPos(cursorBefore); });
    auto hover = [](QWidget *widget) {
        const QPoint local = widget->rect().center();
        const QPoint global = widget->mapToGlobal(local);
        QCursor::setPos(global);
        QEnterEvent enter(local, widget->window()->mapFromGlobal(global), global);
        QApplication::sendEvent(widget, &enter);
    };
    auto leave = [this]() {
        QCursor::setPos(consolePanel()->mapToGlobal(consolePanel()->rect().center()));
    };
    auto railRect = [rail, panel]() {
        return QRect(rail->mapTo(panel->parentWidget(), QPoint(0, 0)), rail->size());
    };

    QTest::qWait(50);   // ukuran splitter awal sudah diterapkan
    QTRY_VERIFY(panel->isVisible());
    QVERIFY(!show->isVisible());
    const QList<int> dockedSizes = splitter->sizes();
    const int dockedWidth = dockedSizes.at(0);
    QTRY_COMPARE(panel->geometry(), railRect());

    // Disembunyikan: sidebar hilang, tombolnya pindah ke rel sempit di tempat yang sama
    hide->click();
    QTRY_VERIFY(!panel->isVisible());
    QVERIFY(show->isVisible());
    const QList<int> hiddenSizes = splitter->sizes();
    QVERIFY2(hiddenSizes.at(0) < 60, qPrintable(QString::number(hiddenSizes.at(0))));
    // Lebar yang dilepas sidebar jatuh ke board; konsol tidak berubah
    QCOMPARE(hiddenSizes.at(0) + hiddenSizes.at(1), dockedSizes.at(0) + dockedSizes.at(1));
    QCOMPARE(hiddenSizes.at(2), dockedSizes.at(2));

    // Hover tombol: sidebar tampil di kanan rel, menimpa board tanpa menggeser pane mana pun
    hover(show);
    QTRY_VERIFY(panel->isVisible());
    QVERIFY(list->isVisible());
    QVERIFY(show->isVisible());
    QVERIFY(panel->geometry().left() > railRect().right());
    QCOMPARE(panel->width(), dockedWidth);
    QCOMPARE(panel->height(), rail->height());
    QCOMPARE(splitter->sizes(), hiddenSizes);

    // Selama kursor di atas sidebar ia bertahan; begitu kursor pergi, hilang lagi
    QCursor::setPos(list->mapToGlobal(list->rect().center()));
    QTest::qWait(500);
    QVERIFY(panel->isVisible());
    leave();
    QTRY_VERIFY(!panel->isVisible());
    QVERIFY(show->isVisible());
    QCOMPARE(splitter->sizes(), hiddenSizes);

    // Klik tombol rel selagi sidebar tampil sementara: terpasang kembali selebar semula
    hover(show);
    QTRY_VERIFY(panel->isVisible());
    show->click();
    QTRY_COMPARE(splitter->sizes().at(0), dockedWidth);
    QTRY_VERIFY(!show->isVisible());
    QTRY_COMPARE(panel->geometry(), railRect());
    leave();
    QTest::qWait(500);
    QVERIFY(panel->isVisible());
}

void TestGui::lieutenantPanelClosesAndReturnsFromViewMenu() {
    auto *splitter = m_window->findChild<QSplitter *>(QStringLiteral("mainSplitter"));
    auto *rail = m_window->findChild<QWidget *>(QStringLiteral("consoleRail"));
    auto *pin = m_window->findChild<QPushButton *>(QStringLiteral("btnConsolePin"));
    auto *close = m_window->findChild<QPushButton *>(QStringLiteral("btnConsoleClose"));
    QAction *view = fileAction(QStringLiteral("actionLieutenant"));
    ConsolePanelWidget *console = consolePanel();
    QVERIFY(splitter && rail && pin && close && view && console);
    auto railRect = [rail, console]() {
        return QRect(rail->mapTo(console->parentWidget(), QPoint(0, 0)), rail->size());
    };

    QTest::qWait(50);   // ukuran splitter awal sudah diterapkan
    QTRY_VERIFY(console->isVisible());
    QVERIFY(view->isCheckable() && view->isChecked());
    QTRY_COMPARE(console->geometry(), railRect());
    const QList<int> shownSizes = splitter->sizes();

    // Tombol pin dan × di kepala panel, menempel tepi kanannya; × paling kanan
    QVERIFY(console->isAncestorOf(pin) && console->isAncestorOf(close));
    QTRY_VERIFY(pin->isVisible() && close->isVisible());
    auto inConsole = [console](const QWidget *widget) {
        return QRect(widget->mapTo(console, QPoint(0, 0)), widget->size());
    };
    QVERIFY(inConsole(close).left() > inConsole(pin).right());
    QVERIFY2(console->width() - inConsole(close).right() <= 24,
             qPrintable(QStringLiteral("panel %1, × sampai %2").arg(console->width()).arg(inConsole(close).right())));
    QVERIFY(!pin->toolTip().isEmpty() && close->toolTip().contains(QStringLiteral("View > Lieutenant")));

    // ×: panel hilang berikut relnya; lebarnya (dan handle-nya) jatuh ke board, sidebar tidak berubah
    close->click();
    QVERIFY(!view->isChecked());
    QVERIFY(!console->isVisible());
    QTRY_VERIFY(!rail->isVisible());
    QTRY_COMPARE(splitter->sizes(), (QList<int>{shownSizes.at(0),
                                                shownSizes.at(1) + shownSizes.at(2) + splitter->handleWidth(), 0}));

    // Notifikasi tetap tercatat selama panel disembunyikan
    console->appendLog(QStringLiteral("[SYSTEM] selagi tersembunyi"));
    QVERIFY(consoleText().contains(QStringLiteral("[SYSTEM] selagi tersembunyi")));

    // View > Lieutenant: panel terpasang kembali selebar semula
    view->trigger();
    QVERIFY(view->isChecked());
    QVERIFY(console->isVisible());
    QVERIFY(console->isPinned());
    QTRY_COMPARE(splitter->sizes(), shownSizes);
    QTRY_COMPARE(console->geometry(), railRect());

    // Item yang sama juga menyembunyikan panel yang sedang tampil
    view->trigger();
    QVERIFY(!view->isChecked());
    QVERIFY(!console->isVisible());
    QTRY_VERIFY(!rail->isVisible());
    view->trigger();
    QTRY_COMPARE(splitter->sizes(), shownSizes);
    QVERIFY(console->isVisible());
}

void TestGui::lieutenantPanelUnpinsToSideTab() {
    auto *splitter = m_window->findChild<QSplitter *>(QStringLiteral("mainSplitter"));
    auto *board = m_window->findChild<QSplitter *>(QStringLiteral("boardSplitter"));
    auto *rail = m_window->findChild<QWidget *>(QStringLiteral("consoleRail"));
    auto *tab = m_window->findChild<VerticalTabButton *>(QStringLiteral("consoleTab"));
    auto *pin = m_window->findChild<QPushButton *>(QStringLiteral("btnConsolePin"));
    QAction *view = fileAction(QStringLiteral("actionLieutenant"));
    ConsolePanelWidget *console = consolePanel();
    QVERIFY(splitter && board && rail && tab && pin && view && console);

    const QPoint cursorBefore = QCursor::pos();
    const auto restoreCursor = qScopeGuard([cursorBefore]() { QCursor::setPos(cursorBefore); });
    auto hover = [](QWidget *widget) {
        const QPoint local = widget->rect().center();
        const QPoint global = widget->mapToGlobal(local);
        QCursor::setPos(global);
        QEnterEvent enter(local, widget->window()->mapFromGlobal(global), global);
        QApplication::sendEvent(widget, &enter);
    };
    // Pojok kiri bawah board: jauh dari rel maupun dari panel yang tampil sementara
    auto leave = [board]() { QCursor::setPos(board->mapToGlobal(QPoint(40, board->height() - 40))); };
    auto railRect = [rail, console]() {
        return QRect(rail->mapTo(console->parentWidget(), QPoint(0, 0)), rail->size());
    };
    // Rel selesai menciut: lebarnya dipatok selebar tab
    auto settledAsTab = [rail]() {
        return rail->minimumWidth() > 0 && rail->minimumWidth() == rail->maximumWidth()
               && rail->width() == rail->minimumWidth();
    };

    QTest::qWait(50);   // ukuran splitter awal sudah diterapkan
    leave();
    QTRY_VERIFY(console->isVisible());
    QVERIFY(console->isPinned());
    QVERIFY(!tab->isVisible());
    QTRY_COMPARE(console->geometry(), railRect());
    const QList<int> pinnedSizes = splitter->sizes();
    const int pinnedWidth = pinnedSizes.at(2);
    const QString pinnedToolTip = pin->toolTip();

    // Pin dilepas: panel hilang, tinggal rel sempit berisi tab. Lebar yang dilepas jatuh ke board.
    pin->click();
    QVERIFY(!console->isVisible());
    QVERIFY(!console->isPinned());
    QVERIFY(pin->toolTip() != pinnedToolTip);
    QVERIFY(view->isChecked());
    QTRY_VERIFY(settledAsTab());
    QVERIFY(tab->isVisible());
    const QList<int> tabSizes = splitter->sizes();
    QVERIFY2(tabSizes.at(2) < 60, qPrintable(QString::number(tabSizes.at(2))));
    QCOMPARE(tabSizes.at(0), pinnedSizes.at(0));
    QCOMPARE(tabSizes.at(1) + tabSizes.at(2), pinnedSizes.at(1) + pinnedSizes.at(2));

    // Tab tegak di dalam rel, ber-caption "Lieutenant"
    QCOMPARE(tab->text(), QStringLiteral("Lieutenant"));
    QVERIFY2(tab->height() > 2 * tab->width(),
             qPrintable(QStringLiteral("%1 x %2").arg(tab->width()).arg(tab->height())));
    QVERIFY(rail->rect().contains(tab->geometry()));

    // Caption diputar 90° ke kiri (dibaca dari bawah ke atas): gambar tab jauh lebih mirip teks
    // mendatar yang diputar ke kiri daripada yang diputar ke kanan
    const QImage shown = tab->grab().toImage().convertToFormat(QImage::Format_RGB32);
    const QColor background = shown.pixelColor(0, 0);
    QImage flat(shown.height(), shown.width(), QImage::Format_RGB32);
    flat.fill(background);
    {
        QPainter painter(&flat);
        painter.setFont(tab->font());
        painter.setPen(tab->palette().color(tab->foregroundRole()));
        painter.drawText(flat.rect(), Qt::AlignCenter, tab->text());
    }
    auto isInk = [&background](const QImage &image, int x, int y) {
        return image.rect().contains(x, y)
               && qAbs(image.pixelColor(x, y).lightness() - background.lightness()) > 40;
    };
    // Piksel tinta `image` yang tidak punya tinta di piksel yang sama maupun tetangganya pada
    // `other`. Toleransi 1 px: teks yang dilukis terputar tidak di-hint seperti teks mendatar.
    auto strayInk = [&isInk](const QImage &image, const QImage &other) {
        int stray = 0;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                if (!isInk(image, x, y)) continue;
                bool matched = false;
                for (int dy = -1; dy <= 1 && !matched; ++dy) {
                    for (int dx = -1; dx <= 1 && !matched; ++dx) {
                        matched = isInk(other, x + dx, y + dy);
                    }
                }
                stray += !matched;
            }
        }
        return stray;
    };
    const QImage turnedLeft = flat.transformed(QTransform().rotate(-90));
    const QImage turnedRight = flat.transformed(QTransform().rotate(90));
    QCOMPARE(turnedLeft.size(), shown.size());
    // Terhadap gambar kosong semua tinta "meleset": jumlah piksel tinta tab
    const int ink = strayInk(shown, QImage());
    const int strayLeft = strayInk(shown, turnedLeft) + strayInk(turnedLeft, shown);
    const int strayRight = strayInk(shown, turnedRight) + strayInk(turnedRight, shown);
    const QString inkReport = QStringLiteral("tinta %1, meleset: kiri %2, kanan %3").arg(ink).arg(strayLeft).arg(strayRight);
    QVERIFY2(ink > 40, qPrintable(inkReport));
    QVERIFY2(strayLeft * 10 < ink, qPrintable(inkReport));
    QVERIFY2(strayRight > 4 * strayLeft && strayRight * 4 > ink, qPrintable(inkReport));

    // Hover tab: panel tampil di kiri rel, menimpa board tanpa menggeser pane mana pun
    hover(tab);
    QTRY_VERIFY(console->isVisible());
    QVERIFY(tab->isVisible());
    QVERIFY(tab->isActive());
    QVERIFY(console->geometry().right() < railRect().left());
    QCOMPARE(console->width(), pinnedWidth);
    QCOMPARE(console->height(), rail->height());
    QCOMPARE(splitter->sizes(), tabSizes);

    // Selama kursor di atas panel ia bertahan; begitu kursor pergi, hilang lagi
    QCursor::setPos(console->mapToGlobal(console->rect().center()));
    QTest::qWait(500);
    QVERIFY(console->isVisible());
    leave();
    QTRY_VERIFY(!console->isVisible());
    QVERIFY(tab->isVisible());
    QVERIFY(!tab->isActive());
    QCOMPARE(splitter->sizes(), tabSizes);

    // Tombol pin di panel yang tampil sementara: terpasang kembali selebar semula
    hover(tab);
    QTRY_VERIFY(console->isVisible());
    pin->click();
    QVERIFY(console->isPinned());
    QCOMPARE(pin->toolTip(), pinnedToolTip);
    QVERIFY(console->isVisible());
    QVERIFY(!tab->isVisible());
    QTRY_COMPARE(splitter->sizes(), pinnedSizes);
    QTRY_COMPARE(console->geometry(), railRect());
    leave();
    QTest::qWait(500);
    QVERIFY(console->isVisible());
}

void TestGui::lieutenantTabTogglesOnClickAndSurvivesHiding() {
    auto *splitter = m_window->findChild<QSplitter *>(QStringLiteral("mainSplitter"));
    auto *board = m_window->findChild<QSplitter *>(QStringLiteral("boardSplitter"));
    auto *rail = m_window->findChild<QWidget *>(QStringLiteral("consoleRail"));
    auto *tab = m_window->findChild<VerticalTabButton *>(QStringLiteral("consoleTab"));
    auto *pin = m_window->findChild<QPushButton *>(QStringLiteral("btnConsolePin"));
    auto *close = m_window->findChild<QPushButton *>(QStringLiteral("btnConsoleClose"));
    auto *dock = m_window->findChild<SidePanelDock *>();
    QAction *view = fileAction(QStringLiteral("actionLieutenant"));
    ConsolePanelWidget *console = consolePanel();
    QVERIFY(splitter && board && rail && tab && pin && close && dock && view && console);

    const QPoint cursorBefore = QCursor::pos();
    const auto restoreCursor = qScopeGuard([cursorBefore]() { QCursor::setPos(cursorBefore); });
    auto settledAsTab = [rail]() {
        return rail->minimumWidth() > 0 && rail->minimumWidth() == rail->maximumWidth()
               && rail->width() == rail->minimumWidth();
    };

    QTest::qWait(50);   // ukuran splitter awal sudah diterapkan
    // Kursor jauh dari rel dan panel selama test ini: tab diaktifkan tanpa hover
    QCursor::setPos(board->mapToGlobal(QPoint(40, board->height() - 40)));
    QTRY_VERIFY(console->isVisible());
    QVERIFY(dock->mode() == SidePanelDock::Mode::Pinned);

    pin->click();
    QVERIFY(dock->mode() == SidePanelDock::Mode::Tab);
    // Selama rel masih menciut, tab belum membuka panel
    tab->click();
    QVERIFY(!console->isVisible());
    QTRY_VERIFY(settledAsTab());
    const QList<int> tabSizes = splitter->sizes();

    // Klik tab tanpa kursor di atasnya: panel tampil dan bertahan, klik berikutnya menutupnya
    tab->click();
    QVERIFY(console->isVisible());
    QVERIFY(dock->isFlyoutOpen());
    QTest::qWait(500);
    QVERIFY(console->isVisible());
    QCOMPARE(splitter->sizes(), tabSizes);
    tab->click();
    QVERIFY(!console->isVisible());
    QVERIFY(!dock->isFlyoutOpen());

    // Disembunyikan dari View selagi berupa tab: relnya ikut hilang
    view->trigger();
    QVERIFY(!view->isChecked());
    QVERIFY(dock->mode() == SidePanelDock::Mode::Hidden);
    QTRY_VERIFY(!rail->isVisible());
    QVERIFY(!console->isVisible());
    QTRY_COMPARE(splitter->sizes(), (QList<int>{tabSizes.at(0),
                                                tabSizes.at(1) + tabSizes.at(2) + splitter->handleWidth(), 0}));

    // Dimunculkan lagi: kembali sebagai tab, bukan terpasang
    view->trigger();
    QVERIFY(view->isChecked());
    QVERIFY(dock->mode() == SidePanelDock::Mode::Tab);
    QTRY_VERIFY(settledAsTab());
    QVERIFY(tab->isVisible());
    QVERIFY(!console->isVisible());
    QVERIFY(!console->isPinned());
    QCOMPARE(splitter->sizes(), tabSizes);

    // × di panel yang tampil sementara: panel berikut relnya disembunyikan
    tab->click();
    QVERIFY(console->isVisible());
    close->click();
    QVERIFY(!console->isVisible());
    QVERIFY(!dock->isFlyoutOpen());
    QVERIFY(!view->isChecked());
    QTRY_VERIFY(!rail->isVisible());
}

void TestGui::lieutenantFlyoutSurvivesDragStartedInside() {
    auto *board = m_window->findChild<QSplitter *>(QStringLiteral("boardSplitter"));
    auto *rail = m_window->findChild<QWidget *>(QStringLiteral("consoleRail"));
    auto *tab = m_window->findChild<VerticalTabButton *>(QStringLiteral("consoleTab"));
    auto *pin = m_window->findChild<QPushButton *>(QStringLiteral("btnConsolePin"));
    // Dua label yang tidak bereaksi terhadap klik: satu di luar panel, satu di kepala panel
    auto *outside = m_window->findChild<QLabel *>(QStringLiteral("labelSidebarTitle"));
    ConsolePanelWidget *console = consolePanel();
    QVERIFY(board && rail && tab && pin && outside && console);
    auto *inside = console->findChild<QLabel *>(QStringLiteral("label"));
    QVERIFY(inside);

    const QPoint cursorBefore = QCursor::pos();
    const auto restoreCursor = qScopeGuard([cursorBefore]() { QCursor::setPos(cursorBefore); });
    auto hover = [](QWidget *widget) {
        const QPoint local = widget->rect().center();
        const QPoint global = widget->mapToGlobal(local);
        QCursor::setPos(global);
        QEnterEvent enter(local, widget->window()->mapFromGlobal(global), global);
        QApplication::sendEvent(widget, &enter);
    };
    auto leave = [board]() { QCursor::setPos(board->mapToGlobal(QPoint(40, board->height() - 40))); };
    auto settledAsTab = [rail]() {
        return rail->minimumWidth() > 0 && rail->minimumWidth() == rail->maximumWidth()
               && rail->width() == rail->minimumWidth();
    };

    QTest::qWait(50);   // ukuran splitter awal sudah diterapkan
    leave();
    // Keadaan tombol mouse milik aplikasi bisa tersisa "tertekan" dari test sebelumnya (klik ganda
    // yang membuka dialog modal); satu klik biasa menormalkannya
    QTest::mouseClick(outside, Qt::LeftButton);
    QVERIFY(QGuiApplication::mouseButtons() == Qt::NoButton);
    pin->click();
    QTRY_VERIFY(settledAsTab());

    // Tombol mouse ditekan di atas panel lalu kursor diseret keluar: panel bertahan sampai dilepas
    hover(tab);
    QTRY_VERIFY(console->isVisible());
    QCursor::setPos(inside->mapToGlobal(inside->rect().center()));
    QTest::qWait(300);
    QTest::mousePress(inside, Qt::LeftButton);
    QTest::qWait(300);
    leave();
    QTest::qWait(700);
    QVERIFY(console->isVisible());
    QTest::mouseRelease(inside, Qt::LeftButton);
    QTRY_VERIFY(!console->isVisible());

    // Tombol mouse ditekan di luar panel selagi panel masih tampil: panel tetap menutup
    hover(tab);
    QTRY_VERIFY(console->isVisible());
    leave();
    QTest::mousePress(outside, Qt::LeftButton);
    QTRY_VERIFY(!console->isVisible());
    QTest::mouseRelease(outside, Qt::LeftButton);
    QVERIFY(QGuiApplication::mouseButtons() == Qt::NoButton);
}

void TestGui::canvasFloatsCreateAndZoomButtons() {
    // Ukuran dan bentuk tombol bergantung pada styles.qss, yang biasanya tidak dimuat test
    QFile qss(QStringLiteral(":/styles.qss"));
    QVERIFY(qss.open(QIODevice::ReadOnly));
    qApp->setStyleSheet(QString::fromUtf8(qss.readAll()));
    const auto resetStyle = qScopeGuard([]() { qApp->setStyleSheet(QString()); });

    QAction *canvas = fileAction(QStringLiteral("actionCanvas"));
    QVERIFY(canvas);
    canvas->trigger();
    auto *page = m_window->findChild<CanvasPage *>();
    QVERIFY(page);
    CanvasView *view = page->view();
    auto *toolbar = page->findChild<QWidget *>(QStringLiteral("canvasToolbar"));
    QVERIFY(view && toolbar);
    QTRY_VERIFY(view->isVisible() && view->width() > 300 && view->height() > 300);

    // Toolbar: ▶ Jalankan alur dan ■ Hentikan tinggal ikon, hijau dan merah seperti tombol run kartu task
    auto *run = toolbar->findChild<QToolButton *>(QStringLiteral("btnCanvasRunAll"));
    auto *stop = toolbar->findChild<QToolButton *>(QStringLiteral("btnCanvasStopAll"));
    QVERIFY(run && stop);
    auto iconColor = [](const QAbstractButton *button) {
        return button->icon().pixmap(QSize(24, 24)).toImage().pixelColor(12, 12);
    };
    QVERIFY(run->text().isEmpty() && !run->icon().isNull());
    QVERIFY(stop->text().isEmpty() && !stop->icon().isNull());
    const QColor play = iconColor(run);
    QVERIFY2(play.green() > play.red() && play.green() > play.blue(), qPrintable(play.name()));
    const QColor square = iconColor(stop);
    QVERIFY2(square.red() > square.green() && square.red() > square.blue(), qPrintable(square.name()));
    // Tanpa teks, artinya dijelaskan tooltip
    QVERIFY(run->toolTip().startsWith(QStringLiteral("Jalankan")));
    QVERIFY(stop->toolTip().startsWith(QStringLiteral("Hentikan")));
    QVERIFY(run->isEnabled());
    QVERIFY(!stop->isEnabled());   // belum ada langkah AI yang berjalan

    // Tombol buat kartu dan zoom pindah dari toolbar ke atas kanvas
    auto floating = [page, view, toolbar](const char *name) -> QAbstractButton * {
        auto *button = page->findChild<QAbstractButton *>(QLatin1String(name));
        return button && view->isAncestorOf(button) && !toolbar->isAncestorOf(button) ? button : nullptr;
    };
    QAbstractButton *note = floating("btnCanvasAddNote");
    QAbstractButton *step = floating("btnCanvasAddStep");
    QAbstractButton *zoomOut = floating("btnCanvasZoomOut");
    QAbstractButton *zoomIn = floating("btnCanvasZoomIn");
    QAbstractButton *fit = floating("btnCanvasFit");
    auto *level = page->findChild<QLabel *>(QStringLiteral("canvasZoomLevel"));
    QVERIFY(note && step && zoomOut && zoomIn && fit && level);
    QVERIFY(view->isAncestorOf(level));
    auto inView = [view](const QWidget *widget) { return QRect(widget->mapTo(view, QPoint(0, 0)), widget->size()); };
    auto describe = [](const QRect &rect) {
        return QStringLiteral("%1,%2 %3x%4").arg(rect.left()).arg(rect.top()).arg(rect.width()).arg(rect.height());
    };

    // Catatan dan Langkah AI: tombol bulat tanpa teks di pojok kiri atas, tidak saling menimpa
    QTRY_VERIFY(note->isVisible() && step->isVisible());
    for (const QAbstractButton *button : {note, step}) {
        QVERIFY(button->text().isEmpty() && !button->icon().isNull() && !button->toolTip().isEmpty());
        QCOMPARE(button->width(), button->height());
        const QRect rect = inView(button);
        QVERIFY2(rect.left() >= 4 && rect.left() <= 32 && rect.top() >= 4 && rect.bottom() <= 140,
                 qPrintable(describe(rect)));
    }
    QVERIFY(!inView(note).intersects(inView(step)));
    // Lingkaran: isian putihnya tidak sampai ke pojok kotak tombol, di sana kanvasnya yang terlihat
    {
        const QImage shot = view->grab().toImage();
        for (const QAbstractButton *button : {note, step}) {
            const QRect rect = inView(button);
            const QColor fill = shot.pixelColor(rect.center().x(), rect.top() + 4);
            QCOMPARE(fill.name(), QStringLiteral("#ffffff"));
            QVERIFY2(shot.pixelColor(rect.left() + 1, rect.top() + 1) != fill, qPrintable(describe(rect)));
            QVERIFY2(shot.pixelColor(rect.right() - 1, rect.bottom() - 1) != fill, qPrintable(describe(rect)));
        }
    }

    // Zoom dan Paskan: satu baris di pojok kiri bawah
    QTRY_VERIFY(zoomOut->isVisible() && zoomIn->isVisible() && fit->isVisible() && level->isVisible());
    QCOMPARE(fit->text(), QStringLiteral("Paskan"));
    for (const QWidget *widget : {static_cast<const QWidget *>(zoomOut), static_cast<const QWidget *>(level),
                                  static_cast<const QWidget *>(zoomIn), static_cast<const QWidget *>(fit)}) {
        const QRect rect = inView(widget);
        QVERIFY2(rect.left() >= 4 && view->height() - rect.bottom() <= 40 && view->height() - rect.bottom() >= 4,
                 qPrintable(QStringLiteral("%1 di kanvas setinggi %2").arg(describe(rect)).arg(view->height())));
    }
    QVERIFY(inView(zoomOut).right() < inView(level).left());
    QVERIFY(inView(level).right() < inView(zoomIn).left());
    QVERIFY(inView(zoomIn).right() < inView(fit).left());
    QVERIFY2(inView(zoomOut).left() <= 32, qPrintable(describe(inView(zoomOut))));

    // Tetap di sudutnya saat kanvas berubah ukuran
    const QRect noteBefore = inView(note);
    const int zoomGap = view->height() - inView(fit).bottom();
    m_window->resize(m_window->width() - 120, m_window->height() - 90);
    QTRY_COMPARE(view->height() - inView(fit).bottom(), zoomGap);
    QCOMPARE(inView(note), noteBefore);

    // Catatan: kartu baru di tengah tampilan, terpilih
    QVERIFY(view->selectedNodeIds().isEmpty());
    note->click();
    QCOMPARE(view->selectedNodeIds().size(), 1);
    const QString noteId = view->selectedNodeIds().first();
    view->finishEditing();
    // Langkah AI: klik = langkah berkeluaran dokumen; menunya memilih jenis keluaran
    step->click();
    QCOMPARE(view->selectedNodeIds().size(), 1);
    const QString stepId = view->selectedNodeIds().first();
    QVERIFY(stepId != noteId);
    auto *stepButton = qobject_cast<QToolButton *>(step);
    QVERIFY(stepButton && stepButton->menu());
    QCOMPARE(stepButton->menu()->actions().size(), 2);
    stepButton->menu()->actions().at(1)->trigger();
    QCOMPARE(view->selectedNodeIds().size(), 1);
    QVERIFY(view->selectedNodeIds().first() != stepId);

    // Zoom bertahap, persentasenya tampil di antara − dan +
    QCOMPARE(level->text(), QStringLiteral("100%"));
    zoomIn->click();
    QCOMPARE(level->text(), QStringLiteral("125%"));
    QCOMPARE(view->zoom(), 1.25);
    zoomOut->click();
    zoomOut->click();
    QCOMPARE(level->text(), QStringLiteral("75%"));
    zoomIn->click();
    zoomIn->click();
    zoomIn->click();
    QCOMPARE(level->text(), QStringLiteral("150%"));
    // Paskan: semua kartu terlihat, tidak diperbesar melewati 100%
    fit->click();
    QVERIFY2(view->zoom() <= 1.0, qPrintable(QString::number(view->zoom())));
    QCOMPARE(level->text(), QStringLiteral("%1%").arg(qRound(view->zoom() * 100)));

    // Klik dua kali di atas tombol mengambang bukan klik dua kali di ruang kosong kanvas: tidak
    // membuat catatan baru
    const qsizetype items = view->scene()->items().size();
    QTest::mouseDClick(level, Qt::LeftButton);
    QTest::mouseDClick(level->parentWidget(), Qt::LeftButton, {}, QPoint(2, 2));
    QCOMPARE(view->scene()->items().size(), items);
}

void TestGui::canvasFitsBesidePinnedLieutenant() {
    // Lebar toolbar bergantung pada styles.qss, yang biasanya tidak dimuat test
    QFile qss(QStringLiteral(":/styles.qss"));
    QVERIFY(qss.open(QIODevice::ReadOnly));
    qApp->setStyleSheet(QString::fromUtf8(qss.readAll()));
    const auto resetStyle = qScopeGuard([]() { qApp->setStyleSheet(QString()); });

    // Jendela selebar layar 1920 px pada skala 150% (1280 px logis), seperti saat dimaksimalkan di sana
    const QSize size(1280, 720);
    m_window.reset();
    m_tasks.reset();
    createWindow(size);

    auto *splitter = m_window->findChild<QSplitter *>(QStringLiteral("mainSplitter"));
    auto *content = m_window->findChild<QWidget *>(QStringLiteral("contentWidget"));
    auto *rail = m_window->findChild<QWidget *>(QStringLiteral("consoleRail"));
    auto *pin = m_window->findChild<QPushButton *>(QStringLiteral("btnConsolePin"));
    QAction *canvas = fileAction(QStringLiteral("actionCanvas"));
    ConsolePanelWidget *console = consolePanel();
    QVERIFY(splitter && content && rail && pin && canvas && console);

    QTest::qWait(50);   // ukuran splitter awal sudah diterapkan
    QTRY_VERIFY(console->isVisible());
    QVERIFY(console->isPinned());

    canvas->trigger();
    auto *page = m_window->findChild<CanvasPage *>();
    QVERIFY(page);
    auto *toolbar = page->findChild<QWidget *>(QStringLiteral("canvasToolbar"));
    QVERIFY(toolbar);
    QTRY_VERIFY(page->isVisible() && toolbar->isVisible());

    // Seluruh isi jendela di dalam jendela: panel Lieutenant sampai tepi kanannya, dan tombol
    // toolbar kanvas tidak ada yang terdorong keluar
    auto overflow = [&]() -> QString {
        if (m_window->size() != size) {
            return QStringLiteral("jendela %1x%2").arg(m_window->width()).arg(m_window->height());
        }
        if (content->width() > m_window->width() || splitter->geometry().right() >= content->width()) {
            return QStringLiteral("isi %1, splitter sampai %2").arg(content->width()).arg(splitter->geometry().right());
        }
        const QRect panel = console->geometry();
        if (!console->isVisible() || panel.width() < 200 || panel.right() >= content->width()) {
            return QStringLiteral("panel %1..%2 di isi selebar %3").arg(panel.left()).arg(panel.right()).arg(content->width());
        }
        const QList<QAbstractButton *> buttons = toolbar->findChildren<QAbstractButton *>();
        for (const QAbstractButton *button : buttons) {
            const QRect rect(button->mapTo(page, QPoint(0, 0)), button->size());
            if (button->isVisible() && (rect.left() < 0 || rect.right() >= page->width()
                                        || button->width() < button->minimumSizeHint().width())) {
                return QStringLiteral("%1 di %2..%3, halaman selebar %4")
                    .arg(button->objectName()).arg(rect.left()).arg(rect.right()).arg(page->width());
            }
        }
        return QString();
    };
    QTest::qWait(300);
    QVERIFY2(overflow().isEmpty(), qPrintable(overflow()));

    // Nama project yang panjang memotong judul ("…", nama lengkap di tooltip), bukan mendorong
    // tombol keluar dari toolbar; judul pendek kembali utuh
    auto *title = toolbar->findChild<ElidedLabel *>(QStringLiteral("canvasTitle"));
    QVERIFY(title);
    const QString shortTitle = title->fullText();
    QCOMPARE(shortTitle, QStringLiteral("Kanvas · Demo"));
    QTRY_COMPARE(title->text(), shortTitle);
    title->setFullText(QStringLiteral("Kanvas · ") + QString(200, QLatin1Char('W')));
    QTRY_VERIFY(title->text().endsWith(QChar(0x2026)) && title->text().size() > 12);
    QCOMPARE(title->toolTip(), title->fullText());
    QTest::qWait(100);
    QVERIFY2(overflow().isEmpty(), qPrintable(overflow()));
    title->setFullText(shortTitle);
    QTRY_COMPARE(title->text(), shortTitle);

    // Pin dilepas lalu dipasang lagi selagi kanvas tampil
    pin->click();
    QTRY_VERIFY(!console->isPinned() && rail->minimumWidth() > 0 && rail->minimumWidth() == rail->maximumWidth());
    pin->click();
    QVERIFY(console->isPinned());
    QTRY_VERIFY(rail->maximumWidth() == QWIDGETSIZE_MAX);
    QTest::qWait(100);
    QVERIFY2(overflow().isEmpty(), qPrintable(overflow()));
}

void TestGui::canvasPreviewsMarkdownInDrawer() {
    QAction *canvas = fileAction(QStringLiteral("actionCanvas"));
    QVERIFY(canvas);
    canvas->trigger();
    auto *page = m_window->findChild<CanvasPage *>();
    QVERIFY(page);
    CanvasView *view = page->view();
    auto *splitter = page->findChild<QSplitter *>(QStringLiteral("canvasSplitter"));
    auto *inspector = page->findChild<CanvasInspector *>();
    auto *drawer = page->findChild<CanvasPreviewDrawer *>();
    QVERIFY(view && splitter && inspector && drawer);
    auto *toggle = inspector->findChild<QPushButton *>(QStringLiteral("btnCanvasPreview"));
    auto *rendered = drawer->findChild<MarkdownView *>();
    auto *kind = drawer->findChild<QLabel *>(QStringLiteral("canvasPanelTitle"));
    auto *expand = drawer->findChild<QPushButton *>(QStringLiteral("btnCanvasPreviewExpand"));
    auto *close = drawer->findChild<QPushButton *>(QStringLiteral("btnCanvasPreviewClose"));
    auto *escape = drawer->findChild<QShortcut *>();
    QVERIFY(toggle && rendered && kind && expand && close && escape);
    QTRY_VERIFY(view->isVisible() && inspector->isVisible() && view->width() > 300);

    auto inPage = [page](const QWidget *widget) { return QRect(widget->mapTo(page, QPoint(0, 0)), widget->size()); };
    auto describe = [](const QRect &rect) {
        return QStringLiteral("%1,%2 %3x%4").arg(rect.left()).arg(rect.top()).arg(rect.width()).arg(rect.height());
    };
    // Tempat drawer selama tidak diperluas: menempel di tepi kanan kanvas, setinggi halaman di bawah
    // toolbar. Lebarnya 55% ruang di kiri panel kanan dalam batas 420..720 px, atau seluruh ruang itu
    // bila lebih sempit atau sisanya kurang dari 120 px; pita bayangan (paling lebar 8 px) menambah
    // sisi kirinya bila masih ada ruang.
    auto docked = [&]() -> QString {
        const QRect area = inPage(splitter);
        const QRect canvasRect = inPage(view);
        const QRect rect = drawer->geometry();
        const int available = canvasRect.right() + 1 - area.left();
        int width = qMin(available, qBound(420, available * 55 / 100, 720));
        if (available - width < 120) {
            width = available;
        }
        const int shadow = qMin(8, available - width);
        if (!drawer->isVisible() || rect.right() != canvasRect.right() || rect.top() != area.top()
            || rect.height() != area.height() || rect.width() != width + shadow) {
            return QStringLiteral("drawer %1, kanvas %2, halaman %3, lebar seharusnya %4")
                .arg(describe(rect), describe(canvasRect), describe(area)).arg(width + shadow);
        }
        return QString();
    };

    // Tanpa kartu terpilih tidak ada yang dipratinjau
    QVERIFY(!toggle->isVisible());
    QVERIFY(!drawer->isVisible() && !drawer->isOpen());

    // Catatan berisi Markdown mentah, diketik di editor panel detail
    const QString noteId = view->addNoteAt(view->centerScenePos(), false);
    QTRY_COMPARE(inspector->nodeId(), noteId);
    QPlainTextEdit *editor = nullptr;
    const QList<QPlainTextEdit *> editors = inspector->findChildren<QPlainTextEdit *>(QStringLiteral("canvasInspectorText"));
    for (QPlainTextEdit *candidate : editors) {
        if (candidate->isVisible()) {
            editor = candidate;
        }
    }
    QVERIFY(editor);
    const QString markdown = QStringLiteral("# Cek WSL\n\n- **NAME**: distro yang terpasang\n- `wsl -l -v`\n\n"
                                            "```powershell\nwsl --status\n```\n");
    editor->setPlainText(markdown);
    inspector->commitText();
    QTRY_VERIFY(toggle->isVisible());
    QCOMPARE(toggle->text(), QStringLiteral("Pratinjau"));
    QVERIFY(!toggle->icon().isNull() && !toggle->toolTip().isEmpty());
    QVERIFY(toggle->isCheckable() && !toggle->isChecked());
    QVERIFY(inspector->isAncestorOf(toggle));

    // Tombol Pratinjau: drawer meluncur keluar di tepi kanan kanvas dan menimpanya, tata letak
    // halaman tidak bergeser dan panel detail tetap terlihat
    const QRect canvasBefore = view->geometry();
    const QRect inspectorBefore = inPage(inspector);
    toggle->click();
    QVERIFY(drawer->isOpen() && toggle->isChecked());
    QTRY_VERIFY2(docked().isEmpty(), qPrintable(docked()));
    QCOMPARE(view->geometry(), canvasBefore);
    QCOMPARE(inPage(inspector), inspectorBefore);
    QVERIFY2(!drawer->geometry().intersects(inspectorBefore), qPrintable(describe(drawer->geometry())));
    QVERIFY(drawer->geometry().intersects(inPage(view)));

    // Isinya dalam bentuk jadi: judul, daftar, dan blok kode tanpa tanda Markdown-nya
    QCOMPARE(kind->text(), QStringLiteral("PRATINJAU · CATATAN"));
    QCOMPARE(rendered->markdown(), markdown);
    const QString text = rendered->document()->toPlainText();
    QVERIFY2(text.startsWith(QStringLiteral("Cek WSL")) && text.contains(QStringLiteral("wsl --status")), qPrintable(text));
    QVERIFY2(!text.contains(QLatin1Char('#')) && !text.contains(QStringLiteral("**"))
                 && !text.contains(QStringLiteral("```")), qPrintable(text));
    QCOMPARE(rendered->document()->begin().blockFormat().headingLevel(), 1);

    // Pratinjau mengikuti ketikan di editor, tanpa menunggu catatannya tersimpan
    const QString retyped = QStringLiteral("## Langkah berikutnya\n\nPasang distro Ubuntu.");
    editor->setPlainText(retyped);
    QTRY_COMPARE(rendered->markdown(), retyped);
    QVERIFY(rendered->document()->toPlainText().startsWith(QStringLiteral("Langkah berikutnya")));
    QCOMPARE(rendered->document()->begin().blockFormat().headingLevel(), 2);
    inspector->commitText();

    // Perluas: menutupi seluruh halaman di bawah toolbar; sekali lagi kembali ke tempat semula
    expand->click();
    QVERIFY(drawer->isExpanded());
    QTRY_COMPARE(drawer->geometry(), inPage(splitter));
    expand->click();
    QVERIFY(!drawer->isExpanded());
    QTRY_VERIFY2(docked().isEmpty(), qPrintable(docked()));

    // Tetap menempel di tepi kanan kanvas saat jendela berubah ukuran
    m_window->resize(m_window->width() - 90, m_window->height() - 60);
    QTRY_VERIFY2(docked().isEmpty(), qPrintable(docked()));
    // Ruang di kiri panel detail sempit (kanvas terjepit Pustaka dan panel detail): drawer memakai
    // seluruh ruang itu, Pustaka ikut tertutup, panel detail tidak
    const QList<int> sizes = splitter->sizes();
    QCOMPARE(sizes.size(), 3);
    splitter->setSizes({180, 190, sizes.at(0) + sizes.at(1) + sizes.at(2) - 370});
    QTRY_VERIFY2(docked().isEmpty(), qPrintable(docked()));
    QVERIFY2(inPage(view).right() + 1 - inPage(splitter).left() < 420, qPrintable(describe(inPage(view))));
    QCOMPARE(drawer->geometry().left(), inPage(splitter).left());
    QVERIFY(drawer->geometry().contains(inPage(page->library())));
    QVERIFY(!drawer->geometry().intersects(inPage(inspector)));

    // Pilihan kosong: drawer tetap terbuka dengan petunjuk, tombolnya bertahan supaya bisa menutupnya
    view->selectNodes({});
    QTRY_VERIFY(rendered->markdown().contains(QStringLiteral("Pilih satu kartu")));
    QCOMPARE(kind->text(), QStringLiteral("PRATINJAU"));
    QVERIFY(toggle->isVisible() && toggle->isChecked());
    // Kartu lain terpilih: isinya yang tampil; kembali ke catatan, catatannya lagi
    view->addStepAt(view->centerScenePos());
    QTRY_COMPARE(kind->text(), QStringLiteral("PRATINJAU · LANGKAH AI"));
    QVERIFY2(rendered->markdown().contains(QStringLiteral("Belum ada hasil")), qPrintable(rendered->markdown()));
    view->selectNodes({noteId});
    QTRY_COMPARE(rendered->markdown(), retyped);
    QCOMPARE(kind->text(), QStringLiteral("PRATINJAU · CATATAN"));

    // ✕ menutup drawer dan mematikan tombolnya; tombol membukanya lagi di tempat yang sama
    close->click();
    QVERIFY(!drawer->isOpen() && !toggle->isChecked());
    QTRY_VERIFY(!drawer->isVisible());
    toggle->click();
    QVERIFY(drawer->isOpen() && toggle->isChecked());
    QTRY_VERIFY2(docked().isEmpty(), qPrintable(docked()));
    QCOMPARE(rendered->markdown(), retyped);
    // Esc di dalam drawer menutupnya, juga selagi diperluas; dibuka lagi dengan lebar biasanya
    QCOMPARE(escape->key(), QKeySequence(Qt::Key_Escape));
    expand->click();
    QTRY_COMPARE(drawer->geometry(), inPage(splitter));
    emit escape->activated();
    QVERIFY(!drawer->isOpen() && !drawer->isExpanded() && !toggle->isChecked());
    QTRY_VERIFY(!drawer->isVisible());
    toggle->click();
    QTRY_VERIFY2(docked().isEmpty(), qPrintable(docked()));
    // Tombol yang menyala menutupnya
    toggle->click();
    QVERIFY(!drawer->isOpen());
    QTRY_VERIFY(!drawer->isVisible());

    // Klik kanan catatan > Pratinjau Markdown: catatan itu terpilih dan drawernya terbuka
    view->selectNodes({noteId});
    view->centerOnNode(noteId);
    QCOMPARE(view->scene()->selectedItems().size(), 1);
    const QPoint onNote = view->mapFromScene(view->scene()->selectedItems().first()->sceneBoundingRect().center());
    QVERIFY(view->viewport()->rect().contains(onNote));
    view->selectNodes({});
    QTRY_VERIFY(inspector->nodeId().isEmpty());
    QContextMenuEvent rightClick(QContextMenuEvent::Mouse, onNote, view->viewport()->mapToGlobal(onNote));
    QApplication::sendEvent(view->viewport(), &rightClick);
    QAction *previewAction = nullptr;
    QMenu *contextMenu = nullptr;
    const QList<QMenu *> menus = view->findChildren<QMenu *>(QStringLiteral("canvasContextMenu"));
    for (QMenu *menu : menus) {
        const QList<QAction *> actions = menu->actions();
        for (QAction *action : actions) {
            if (menu->isVisible() && action->text() == QStringLiteral("Pratinjau Markdown")) {
                previewAction = action;
                contextMenu = menu;
            }
        }
    }
    QVERIFY(previewAction && contextMenu);
    QCOMPARE(view->selectedNodeIds(), QStringList{noteId});
    previewAction->trigger();
    contextMenu->close();
    QVERIFY(drawer->isOpen());
    QCOMPARE(inspector->nodeId(), noteId);
    QCOMPARE(rendered->markdown(), retyped);
    QTRY_VERIFY2(docked().isEmpty(), qPrintable(docked()));
    QVERIFY(toggle->isChecked());
    close->click();
    QTRY_VERIFY(!drawer->isVisible());

    // Tanpa kartu terpilih dan tanpa drawer, tombolnya hilang lagi
    view->selectNodes({});
    QTRY_VERIFY(!toggle->isVisible());
}

namespace {

// Sisi keluar dan sisi masuk sebuah garis, mis. "kanan>kiri"
QString sidesOf(const CanvasRoute &route) {
    static const char *const names[] = {"kiri", "atas", "kanan", "bawah"};
    return QStringLiteral("%1>%2").arg(QLatin1String(names[int(route.from)]), QLatin1String(names[int(route.to)]));
}

CanvasNodeItem *canvasNode(const CanvasView &view, const QString &id) {
    const QList<QGraphicsItem *> items = view.scene()->items();
    for (QGraphicsItem *item : items) {
        auto *node = qgraphicsitem_cast<CanvasNodeItem *>(item);
        if (node && node->id() == id) {
            return node;
        }
    }
    return nullptr;
}

CanvasEdgeItem *canvasEdge(const CanvasView &view, const QString &from, const QString &to) {
    const QList<QGraphicsItem *> items = view.scene()->items();
    for (QGraphicsItem *item : items) {
        auto *edge = qgraphicsitem_cast<CanvasEdgeItem *>(item);
        if (edge && edge->from()->id() == from && edge->to()->id() == to) {
            return edge;
        }
    }
    return nullptr;
}

}

void TestGui::canvasEdgesAttachToFacingSides() {
    // Aturan rutenya sendiri. Tujuan di kanan, kiri, bawah, atas: dua sisi yang saling berhadapan
    const QRectF origin(0, 0, 220, 150);
    auto towards = [&origin](const QRectF &target) { return sidesOf(canvasRoute(origin, target)); };
    QCOMPARE(towards(QRectF(500, 20, 220, 150)), QStringLiteral("kanan>kiri"));
    QCOMPARE(towards(QRectF(-500, -20, 220, 150)), QStringLiteral("kiri>kanan"));
    QCOMPARE(towards(QRectF(30, 400, 220, 150)), QStringLiteral("bawah>atas"));
    QCOMPARE(towards(QRectF(-30, -400, 220, 150)), QStringLiteral("atas>bawah"));
    // Serong: lorong yang garisnya lebih lurus; seri = mendatar, arah baca kanvas
    QCOMPARE(towards(QRectF(500, 300, 220, 150)), QStringLiteral("kanan>kiri"));
    QCOMPARE(towards(QRectF(250, 600, 220, 150)), QStringLiteral("bawah>atas"));
    QCOMPARE(towards(QRectF(-470, -600, 220, 150)), QStringLiteral("atas>bawah"));
    QCOMPARE(sidesOf(canvasRoute(QRectF(0, 0, 200, 200), QRectF(400, 400, 200, 200))), QStringLiteral("kanan>kiri"));
    // Titik (kursor selagi garis ditarik) diperlakukan seperti kartu berukuran nol
    QCOMPARE(towards(QRectF(QPointF(110, 500), QSizeF(0, 0))), QStringLiteral("bawah>atas"));
    QCOMPARE(towards(QRectF(QPointF(-300, 60), QSizeF(0, 0))), QStringLiteral("kiri>kanan"));

    // Lorong yang diisi kartu lain dihindari; bila dua-duanya terisi, kembali ke yang lebih lurus
    auto around = [&origin](const QRectF &target, const QList<QRectF> &cards) {
        return sidesOf(canvasRoute(origin, target, [&cards](const QRectF &lane) {
            return std::any_of(cards.cbegin(), cards.cend(), [&lane](const QRectF &card) { return card.intersects(lane); });
        }));
    };
    const QRectF below(250, 600, 220, 150);
    const QRectF inUpright(-20, 300, 220, 150);   // di antara kedua kartu, di kiri lorong mendatarnya
    const QRectF inAcross(225, 80, 20, 60);       // di lorong mendatar, di atas lorong tegaknya
    QCOMPARE(around(below, {}), QStringLiteral("bawah>atas"));
    QCOMPARE(around(below, {inUpright}), QStringLiteral("kanan>kiri"));
    QCOMPARE(around(below, {inUpright, inAcross}), QStringLiteral("bawah>atas"));
    const QRectF beside(500, 300, 220, 150);
    QCOMPARE(around(beside, {QRectF(300, 70, 100, 70)}), QStringLiteral("bawah>atas"));

    // Jalurnya: dari titik tempel ke titik tempel, meninggalkan kartu tegak lurus sisinya, dan tidak
    // melengkung keluar dari lorong di antara kedua kartu, juga bila lorongnya sempit
    const QPainterPath path = canvasConnectorPath(QPointF(220, 75), CanvasSide::Right, QPointF(500, 300), CanvasSide::Left);
    QCOMPARE(path.pointAtPercent(0), QPointF(220, 75));
    QCOMPARE(path.pointAtPercent(1), QPointF(500, 300));
    const QPointF leaving = path.pointAtPercent(0.03) - path.pointAtPercent(0);
    QVERIFY2(leaving.x() > 0 && qAbs(leaving.y()) < leaving.x() * 0.3,
             qPrintable(QStringLiteral("%1,%2").arg(leaving.x()).arg(leaving.y())));
    QVERIFY(path.boundingRect().left() >= 219.5 && path.boundingRect().right() <= 500.5);
    const QRectF tight = canvasConnectorPath(QPointF(220, 75), CanvasSide::Right, QPointF(240, 700), CanvasSide::Left)
                             .boundingRect();
    QVERIFY2(tight.left() >= 219.5 && tight.right() <= 240.5,
             qPrintable(QStringLiteral("%1..%2").arg(tight.left()).arg(tight.right())));
    const QPainterPath down = canvasConnectorPath(QPointF(110, 150), CanvasSide::Bottom, QPointF(360, 600), CanvasSide::Top);
    const QPointF dropping = down.pointAtPercent(0.03) - down.pointAtPercent(0);
    QVERIFY(dropping.y() > 0 && qAbs(dropping.x()) < dropping.y() * 0.3);

    // Di kanvas: garis mengikuti kartunya
    CanvasModel model(QStringLiteral("Demo"));
    CanvasView view(model);
    view.resize(900, 600);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    const QString color = CanvasPalette::defaultNoteColor();
    const QString a = model.addNote(QPointF(0, 0), QStringLiteral("asal"), color);
    const QString b = model.addNote(QPointF(500, 20), QStringLiteral("tujuan"), color);
    QVERIFY(!model.connectNodes(a, b).isEmpty());
    CanvasEdgeItem *edge = canvasEdge(view, a, b);
    QVERIFY(edge && canvasNode(view, a) && canvasNode(view, b));
    QCOMPARE(sidesOf(edge->route()), QStringLiteral("kanan>kiri"));
    QCOMPARE(edge->start(), canvasNode(view, a)->port(CanvasSide::Right));
    QCOMPARE(edge->tip(), canvasNode(view, b)->port(CanvasSide::Left));
    QCOMPARE(edge->tip(), QPointF(500, 95));
    // Tujuan pindah ke bawah, ke kiri, lalu ke atas: sisi tempelnya ikut pindah
    model.moveNodes({{b, QPointF(30, 400)}});
    QCOMPARE(sidesOf(edge->route()), QStringLiteral("bawah>atas"));
    QCOMPARE(edge->start(), QPointF(110, 150));
    QCOMPARE(edge->tip(), QPointF(140, 400));
    // Mata panahnya menghadap ke bawah: badannya di atas ujungnya, di luar kartu tujuan
    // Seluruh mata panah bisa diklik, juga pangkalnya tempat ia bertemu garisnya
    for (qreal back : {1.0, 4.0, 8.0, 9.5, 12.0}) {
        QVERIFY2(edge->shape().contains(edge->tip() - QPointF(0, back)), qPrintable(QString::number(back)));
    }
    QVERIFY(!canvasNode(view, b)->sceneCardRect().contains(edge->tip() - QPointF(0, 4)));
    model.moveNodes({{b, QPointF(-500, -20)}});
    QCOMPARE(sidesOf(edge->route()), QStringLiteral("kiri>kanan"));
    QCOMPARE(edge->tip(), canvasNode(view, b)->port(CanvasSide::Right));
    model.moveNodes({{b, QPointF(-30, -400)}});
    QCOMPARE(sidesOf(edge->route()), QStringLiteral("atas>bawah"));
    // Selagi kartu diseret (posisinya belum dikirim ke model) garisnya sudah ikut
    canvasNode(view, b)->setPos(500, 20);
    QCOMPARE(sidesOf(edge->route()), QStringLiteral("kanan>kiri"));
    QCOMPARE(edge->tip(), QPointF(500, 95));

    // Kipas: langkah AI dengan kolom task di kanannya. Task yang jauh di bawah letaknya lebih tegak
    // daripada mendatar, tetapi lorong tegaknya diisi task-task di atasnya: semua garis tetap masuk
    // dari kiri, tidak terjepit di sela kartu yang berjajar
    const QString step = model.addStep(QPointF(1000, 0), QStringLiteral("Pecah jadi task"), CanvasStepOutput::Tasks);
    QStringList tasks;
    for (int i = 0; i < 6; ++i) {
        const CanvasSource source{QStringLiteral("Demo"), QStringLiteral("k%1").arg(i), QString(), QString()};
        tasks.append(model.addReference(source, QPointF(1420, i * 156), QStringLiteral("Task %1").arg(i + 1),
                                        QStringLiteral("WAITING"), QString()));
        QVERIFY(!model.connectNodes(step, tasks.last()).isEmpty());
    }
    QCoreApplication::processEvents();
    for (const QString &task : std::as_const(tasks)) {
        QVERIFY(canvasEdge(view, step, task));
        QCOMPARE(sidesOf(canvasEdge(view, step, task)->route()), QStringLiteral("kanan>kiri"));
    }
    // Tanpa kolom itu, task terakhir saja disambung dari sisi bawah langkahnya
    QCOMPARE(sidesOf(canvasRoute(QRectF(1000, 0, 300, 200), QRectF(1420, 780, 260, 132))), QStringLiteral("bawah>atas"));

    // Kartu lain yang masuk ke lorong sebuah garis membuat garis itu pindah lorong, dan kembali
    // setelah kartunya pergi: semua garis ditata ulang saat kartu mana pun berpindah
    const QString c = model.addNote(QPointF(0, 1000), QStringLiteral("asal serong"), color);
    const QString d = model.addNote(QPointF(250, 1600), QStringLiteral("tujuan serong"), color);
    QVERIFY(!model.connectNodes(c, d).isEmpty());
    CanvasEdgeItem *diagonal = canvasEdge(view, c, d);
    QVERIFY(diagonal);
    QCOMPARE(sidesOf(diagonal->route()), QStringLiteral("bawah>atas"));
    const QString blocker = model.addNote(QPointF(-20, 1300), QStringLiteral("penghalang"), color);
    QTRY_COMPARE(sidesOf(diagonal->route()), QStringLiteral("kanan>kiri"));
    model.moveNodes({{blocker, QPointF(-600, 1300)}});
    QTRY_COMPARE(sidesOf(diagonal->route()), QStringLiteral("bawah>atas"));
    canvasNode(view, blocker)->setPos(-20, 1300);   // diseret kembali ke lorong, belum dilepas
    QTRY_COMPARE(sidesOf(diagonal->route()), QStringLiteral("kanan>kiri"));
    model.moveNodes({{blocker, QPointF(-20, 1300)}});
    model.removeNodes({blocker});
    QTRY_COMPARE(sidesOf(diagonal->route()), QStringLiteral("bawah>atas"));

    // Undo membangun ulang semua kartu dan garis: rutenya tetap dihitung dari letak kartu
    QVERIFY(model.undo());
    QVERIFY(canvasNode(view, blocker));
    QTRY_VERIFY(canvasEdge(view, c, d));
    QTRY_COMPARE(sidesOf(canvasEdge(view, c, d)->route()), QStringLiteral("kanan>kiri"));
    for (const QString &task : std::as_const(tasks)) {
        QCOMPARE(sidesOf(canvasEdge(view, step, task)->route()), QStringLiteral("kanan>kiri"));
    }
}

void TestGui::canvasConnectsFromAnySide() {
    CanvasModel model(QStringLiteral("Demo"));
    CanvasView view(model);
    view.resize(900, 600);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    view.restoreView(QPointF(0, 0), 1.0);
    const QString color = CanvasPalette::defaultNoteColor();
    const QString a = model.addNote(QPointF(-110, -75), QStringLiteral("asal"), color);
    CanvasNodeItem *origin = canvasNode(view, a);
    QVERIFY(origin);
    QWidget *canvas = view.viewport();

    // Titik sambung di tengah keempat sisi; tengah kartu dan pojoknya bukan titik sambung
    for (CanvasSide side : {CanvasSide::Left, CanvasSide::Top, CanvasSide::Right, CanvasSide::Bottom}) {
        QVERIFY2(origin->hitsPort(origin->port(side), 9.0), qPrintable(QString::number(int(side))));
        // Juga sedikit di luar tepi kartu, tempat separuh titiknya dilukis, dan sedikit di dalamnya
        const QPointF outward = (origin->port(side) - origin->sceneCardRect().center()) * 0.03;
        QVERIFY(origin->shape().contains(origin->mapFromScene(origin->port(side) + outward)));
        QVERIFY(origin->shape().contains(origin->mapFromScene(origin->port(side) - outward)));
    }
    QVERIFY(!origin->hitsPort(origin->sceneCardRect().center(), 9.0));
    QVERIFY(!origin->hitsPort(origin->sceneCardRect().topLeft(), 9.0));

    // Tarik dari titik bawah ke ruang kosong di bawahnya: catatan baru yang tersambung. Garisnya turun
    // dari sisi bawah, dan titik lepasnya menjadi tengah sisi atas catatan baru itu
    const QPoint bottomPort = view.mapFromScene(origin->port(CanvasSide::Bottom));
    const QPoint drop = view.mapFromScene(QPointF(30, 250));
    QTest::mousePress(canvas, Qt::LeftButton, {}, bottomPort);
    QTest::mouseMove(canvas, (bottomPort + drop) / 2);
    QTest::mouseMove(canvas, drop);
    QTest::mouseRelease(canvas, Qt::LeftButton, {}, drop);
    view.finishEditing();
    QCOMPARE(model.board().nodes.size(), 2);
    QCOMPARE(model.board().edges.size(), 1);
    const CanvasNode created = model.board().nodes.last();
    QCOMPARE(model.board().edges.first().from, a);
    QCOMPARE(model.board().edges.first().to, created.id);
    QCOMPARE(created.pos, view.mapToScene(drop) - QPointF(created.size.width() / 2, 0));
    QVERIFY(canvasEdge(view, a, created.id));
    QCOMPARE(sidesOf(canvasEdge(view, a, created.id)->route()), QStringLiteral("bawah>atas"));
    // Kartu asalnya tidak ikut bergeser
    QCOMPARE(model.node(a)->pos, QPointF(-110, -75));

    // Dari titik kiri ke kartu di sebelah kirinya. Selagi ditarik, garisnya sudah menempel di sisi
    // kanan kartu yang ditunjuk; dilepas di sana keduanya tersambung
    const QString left = model.addNote(QPointF(-440, -60), QStringLiteral("kiri"), color);
    CanvasNodeItem *target = canvasNode(view, left);
    QVERIFY(target);
    const QPoint onTarget = view.mapFromScene(target->sceneCardRect().center());
    QTest::mousePress(canvas, Qt::LeftButton, {}, view.mapFromScene(origin->port(CanvasSide::Left)));
    QTest::mouseMove(canvas, view.mapFromScene(QPointF(-200, 10)));
    QTest::mouseMove(canvas, onTarget);
    const QGraphicsPathItem *preview = nullptr;
    const QList<QGraphicsItem *> items = view.scene()->items();
    for (const QGraphicsItem *item : items) {
        if (item->type() == QGraphicsPathItem::Type) {
            preview = static_cast<const QGraphicsPathItem *>(item);
        }
    }
    QVERIFY(preview);
    QCOMPARE(preview->path().pointAtPercent(0), origin->port(CanvasSide::Left));
    QCOMPARE(preview->path().pointAtPercent(1), target->port(CanvasSide::Right));
    QTest::mouseRelease(canvas, Qt::LeftButton, {}, onTarget);
    QVERIFY(model.board().hasEdge(a, left));
    QCOMPARE(model.board().nodes.size(), 3);
    QVERIFY(canvasEdge(view, a, left));
    QCOMPARE(sidesOf(canvasEdge(view, a, left)->route()), QStringLiteral("kiri>kanan"));

    // Menekan di badan kartu tetap menggeser kartunya, bukan menarik garis baru
    const QPoint body = view.mapFromScene(origin->sceneCardRect().center());
    QTest::mousePress(canvas, Qt::LeftButton, {}, body);
    QTest::mouseMove(canvas, body + QPoint(15, 5));
    QTest::mouseMove(canvas, body + QPoint(30, 10));
    QTest::mouseRelease(canvas, Qt::LeftButton, {}, body + QPoint(30, 10));
    QCOMPARE(model.board().edges.size(), 2);
    QCOMPARE(model.node(a)->pos, QPointF(-80, -65));
}

void TestGui::canvasCardsShowWhatFitsAtAnyZoom() {
    CanvasModel model(QStringLiteral("Demo"));
    CanvasView view(model);
    view.resize(900, 600);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    view.restoreView(QPointF(0, 0), 1.0);
    const QString color = CanvasPalette::defaultNoteColor();
    auto shown = [](const QStringList &lines) { return lines.join(QLatin1Char(' ')).simplified(); };
    // Tanpa spasi: baris boleh patah di mana saja, mis. di tengah "C++20"
    auto squeezed = [](QString text) { return text.remove(QLatin1Char(' ')); };
    const QChar ellipsis(0x2026);
    // Baris pertama selalu awal judulnya; judul yang tidak tampil seluruhnya berakhir elipsis
    auto cutCleanly = [&shown, &squeezed, ellipsis](const QStringList &lines, const QString &title) -> QString {
        if (lines.isEmpty()) {
            return QStringLiteral("tidak ada teks");
        }
        QString first = lines.first();
        if (first.endsWith(ellipsis)) {
            first.chop(1);
        }
        if (!squeezed(title).startsWith(squeezed(first))) {
            return QStringLiteral("baris pertama bukan awal judul: ") + lines.first();
        }
        if (!squeezed(shown(lines)).startsWith(squeezed(title)) && !shown(lines).contains(ellipsis)) {
            return QStringLiteral("judul terpotong tanpa elipsis: ") + shown(lines);
        }
        return QString();
    };

    // Kartu task seukuran bawaannya
    const QString title = QStringLiteral("Siapkan lingkungan Linux dan toolchain C++20");
    const QString taskId = model.addReference(
        CanvasSource{QStringLiteral("Demo"), QStringLiteral("k1"), QString(), QString()}, QPointF(-130, -66), title,
        QStringLiteral("WAITING"), QStringLiteral("Kategori: utility\n\n## Tujuan\nMenyiapkan lingkungan belajar."));
    CanvasNodeItem *task = canvasNode(view, taskId);
    QVERIFY(task);
    // Zoom biasa: judul utuh, keterangan, lalu isi tanpa tanda Markdown-nya
    const QStringList full = task->visibleText(1.0);
    QVERIFY2(squeezed(shown(full)).startsWith(squeezed(title)), qPrintable(shown(full)));
    QVERIFY2(full.contains(QStringLiteral("WAITING · Demo")), qPrintable(shown(full)));
    QVERIFY2(shown(full).contains(QStringLiteral("Kategori: utility")), qPrintable(shown(full)));
    QVERIFY2(!shown(full).contains(QLatin1Char('#')), qPrintable(shown(full)));
    // Diperbesar tata letaknya tidak berubah: hurufnya sekadar ikut membesar bersama kartunya
    QCOMPARE(task->visibleText(2.0), full);
    QCOMPARE(task->visibleText(0.9), full);
    // Diperkecil: makin sedikit yang muat, tetapi tidak pernah terpotong diam-diam
    for (qreal scale : {0.75, 0.5, 0.4, 0.3, 0.2}) {
        const QStringList lines = task->visibleText(scale);
        QVERIFY2(cutCleanly(lines, title).isEmpty(),
                 qPrintable(QStringLiteral("skala %1: %2").arg(scale).arg(cutCleanly(lines, title))));
    }
    // Judul yang terlalu panjang untuk kartunya: awalnya saja, berakhir elipsis
    const QString longTitle = QStringLiteral("Kerangka benchmark, integrasi berkelanjutan, jurnal belajar bulanan, "
                                             "dan catatan evaluasi tiap fase untuk enam bulan ke depan beserta "
                                             "daftar bacaan pendukungnya");
    const QString longId = model.addReference(
        CanvasSource{QStringLiteral("Demo"), QStringLiteral("k2"), QString(), QString()}, QPointF(200, -66), longTitle,
        QStringLiteral("WAITING"), QString());
    for (qreal scale : {1.0, 0.5, 0.3}) {
        const QStringList lines = canvasNode(view, longId)->visibleText(scale);
        QVERIFY2(cutCleanly(lines, longTitle).isEmpty(),
                 qPrintable(QStringLiteral("skala %1: %2").arg(scale).arg(cutCleanly(lines, longTitle))));
        QVERIFY2(shown(lines).contains(ellipsis), qPrintable(shown(lines)));
    }

    // Catatan berisi dokumen. Pada zoom biasa teksnya apa adanya, sama seperti di editornya
    const QString document = QStringLiteral("> cara mengeceknya?\n\nSetelah restart, cek lewat PowerShell.\n\n"
                                            "## Cek cepat\n\n```powershell\nwsl --status\n```\n\n"
                                            "Menampilkan versi default dan versi kernel WSL yang terpasang di mesin ini.");
    const QString noteId = model.addNote(QPointF(-300, 200), document, color);
    model.resizeNode(noteId, QSizeF(600, 700));
    CanvasNodeItem *note = canvasNode(view, noteId);
    QVERIFY(note);
    const QStringList raw = note->visibleText(1.0);
    QVERIFY(!raw.isEmpty());
    QCOMPARE(raw.first(), QStringLiteral("> cara mengeceknya?"));
    QVERIFY2(shown(raw).contains(QStringLiteral("```powershell")), qPrintable(shown(raw)));
    // Diperkecil: baris pertamanya menjadi judul dan isinya tetap tampil (dulu tinggal judul di kartu
    // yang kosong), tanpa tanda Markdown
    for (qreal scale : {0.6, 0.3}) {
        const QStringList summary = note->visibleText(scale);
        QVERIFY(!summary.isEmpty());
        QCOMPARE(summary.first(), QStringLiteral("cara mengeceknya?"));
        QVERIFY2(shown(summary).contains(QStringLiteral("Setelah restart")), qPrintable(shown(summary)));
        QVERIFY2(shown(summary).contains(QStringLiteral("wsl --status")), qPrintable(shown(summary)));
        QVERIFY2(!shown(summary).contains(QLatin1Char('#')) && !shown(summary).contains(QStringLiteral("```")),
                 qPrintable(shown(summary)));
    }
    // Paragraf pembuka yang panjang bukan judul: tetap terbaca sebagai isi, dari kata pertamanya
    const QString paragraph = QStringLiteral("Rencana pemelajaran enam bulan ini dirancang khusus untuk memadukan sistem "
                                             "dan jaringan komputer dengan pemanfaatan C++ modern.\n\nBulan pertama.");
    const QString paragraphId = model.addNote(QPointF(400, 200), paragraph, color);
    model.resizeNode(paragraphId, QSizeF(600, 400));
    const QStringList body = canvasNode(view, paragraphId)->visibleText(0.3);
    QVERIFY2(shown(body).startsWith(QStringLiteral("Rencana pemelajaran enam bulan")), qPrintable(shown(body)));
    QVERIFY2(shown(body).contains(QStringLiteral("Bulan pertama")), qPrintable(shown(body)));
    // Catatan kosong menampilkan petunjuknya di zoom berapa pun
    const QString emptyId = model.addNote(QPointF(-600, -200), QString(), color);
    QVERIFY(shown(canvasNode(view, emptyId)->visibleText(1.0)).startsWith(QStringLiteral("Klik dua kali")));
    QVERIFY(shown(canvasNode(view, emptyId)->visibleText(0.4)).startsWith(QStringLiteral("Klik dua kali")));

    // Titik sambung dan daerah klik garis berukuran layar: saat kanvas diperkecil keduanya melebar
    // di kanvas, tidak mengecil sampai tidak bisa dikenai
    const QString farId = model.addNote(QPointF(-700, -75), QStringLiteral("jauh"), color);
    QVERIFY(!model.connectNodes(taskId, farId).isEmpty());
    CanvasEdgeItem *edge = canvasEdge(view, taskId, farId);
    QVERIFY(edge);
    QCOMPARE(sidesOf(edge->route()), QStringLiteral("kiri>kanan"));
    // Garisnya lurus mendatar; titik ini 22 px di bawah tengahnya
    const QPointF beside = (edge->start() + edge->tip()) / 2 + QPointF(0, 22);
    QVERIFY(!edge->shape().contains(beside));
    const qreal reachAtFull = task->boundingRect().right() - task->cardRect().right();
    QVERIFY2(reachAtFull >= 5.0 && reachAtFull < 12.0, qPrintable(QString::number(reachAtFull)));
    view.restoreView(QPointF(0, 0), 0.2);
    QVERIFY(edge->shape().contains(beside));
    const qreal reachZoomedOut = task->boundingRect().right() - task->cardRect().right();
    QVERIFY2(reachZoomedOut >= 4.5 / 0.2, qPrintable(QString::number(reachZoomedOut)));
    // Titik sambung kartu terpilih terlihat: 2 px di kanan tengah sisi kanannya (di luar kartu) sudah
    // bagian dalam titik yang berisi warna permukaan, bukan latar kanvas
    view.selectNodes({taskId});
    const QPoint dot = view.mapFromScene(task->port(CanvasSide::Right)) + QPoint(2, 0);
    const QImage shot = view.grab().toImage();
    QCOMPARE(shot.pixelColor(dot).name(), Theme::fill(0xffffff).name());
    QVERIFY(shot.pixelColor(dot + QPoint(12, 0)).name() != Theme::fill(0xffffff).name());

    // Diperkecil sejauh-jauhnya kartu tinggal belasan piksel, tetapi menekan tengahnya tetap
    // menggesernya: pegangan ubah ukuran dan titik sambung tidak memenuhi seluruh kartu
    view.restoreView(QPointF(0, 0), 0.1);
    const QPoint middle = view.mapFromScene(task->sceneCardRect().center());
    QTest::mousePress(view.viewport(), Qt::LeftButton, {}, middle);
    QTest::mouseMove(view.viewport(), middle + QPoint(5, 2));
    QTest::mouseMove(view.viewport(), middle + QPoint(10, 5));
    QTest::mouseRelease(view.viewport(), Qt::LeftButton, {}, middle + QPoint(10, 5));
    QCOMPARE(model.node(taskId)->size, QSizeF(260, 132));
    QCOMPARE(model.node(taskId)->pos, QPointF(-30, -16));
    QCOMPARE(model.board().edges.size(), 1);
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
    QTest::newRow("api key bermasalah") << int(RuntimeCheck::Status::BadApiKey) << QStringLiteral("API key")
                                        << QString();
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

QDialog *TestGui::integrationsDialog() const {
    const QList<QDialog *> dialogs = m_window->findChildren<QDialog *>(QStringLiteral("integrationsDialog"));
    for (QDialog *dialog : dialogs) {
        if (dialog->isVisible()) {
            return dialog;
        }
    }
    return nullptr;
}

QDialog *TestGui::openIntegrations() {
    QAction *action = fileAction(QStringLiteral("actionIntegrations"));
    if (!action) {
        return nullptr;
    }
    action->trigger();
    return QTest::qWaitFor([this]() { return integrationsDialog() != nullptr; }) ? integrationsDialog() : nullptr;
}

void TestGui::badApiKeyNoticeOpensIntegrations() {
    const QString rejected = QStringLiteral("API key Anthropic ditolak server (401)");
    m_runtime.failureDiagnosis = RuntimeCheck::of(RuntimeCheck::Status::BadApiKey, rejected);

    runButton(QStringLiteral("t1"))->click();
    m_runtime.sessions.last()->finishWith(AgentResult::failure(QStringLiteral("api_key_rejected"), rejected));
    QTRY_VERIFY(runtimeNotice());
    QDialog *notice = runtimeNotice();
    QVERIFY(notice->findChild<QLabel *>(QStringLiteral("runtimeNoticeTitle"))->text().contains(QStringLiteral("API key")));
    QVERIFY(notice->findChild<QLabel *>(QStringLiteral("runtimeNoticeMessage"))->text().contains(rejected));

    // Tidak ada halaman panduan untuk dibuka, tapi tombolnya tetap hidup
    auto *open = notice->findChild<QPushButton *>(QStringLiteral("btnRuntimeNoticeOpen"));
    QVERIFY(open);
    QCOMPARE(open->text(), QStringLiteral("Buka Integrations"));
    QVERIFY(notice->property("helpUrl").toString().isEmpty());
    QVERIFY(open->isEnabled());
    QVERIFY(!integrationsDialog());

    open->click();
    QTRY_VERIFY(!runtimeNotice());
    QTRY_VERIFY(integrationsDialog());
    integrationsDialog()->reject();
}

void TestGui::integrationsSavesApiKeyAndReturnsToLogin() {
#ifndef Q_OS_WIN
    QSKIP("API key disimpan di Windows Credential Manager");
#endif
    // Key test tersimpan sebagai "LassomoirGuiTest/anthropic-api-key", terpisah dari milik aplikasi
    const QString secretName = QStringLiteral("anthropic-api-key");
    const auto restore = qScopeGuard([]() { AgentAccess::save(AgentAccess::Settings()); });
    QVERIFY(AgentAccess::save(AgentAccess::Settings()));
    m_runtime.accountStatus.state = AccountStatus::State::LoggedIn;
    m_runtime.accountStatus.account = QStringLiteral("dev@example.test");
    m_runtime.accountStatus.plan = QStringLiteral("Pro");

    struct Form {
        QRadioButton *useLogin = nullptr;
        QRadioButton *useApiKey = nullptr;
        QLineEdit *apiKey = nullptr;
        QPushButton *reveal = nullptr;
        QLabel *error = nullptr;
        QPushButton *save = nullptr;
        bool complete() const { return useLogin && useApiKey && apiKey && reveal && error && save; }
    };
    auto formOf = [](QDialog *dialog) {
        Form form;
        if (dialog) {
            form.useLogin = dialog->findChild<QRadioButton *>(QStringLiteral("radioAccessLogin"));
            form.useApiKey = dialog->findChild<QRadioButton *>(QStringLiteral("radioAccessApiKey"));
            form.apiKey = dialog->findChild<QLineEdit *>(QStringLiteral("integrationsApiKey"));
            form.reveal = dialog->findChild<QPushButton *>(QStringLiteral("btnIntegrationsReveal"));
            form.error = dialog->findChild<QLabel *>(QStringLiteral("integrationsError"));
            form.save = dialog->findChild<QPushButton *>(QStringLiteral("btnIntegrationsSave"));
        }
        return form;
    };

    QPointer<QDialog> dialog = openIntegrations();
    Form form = formOf(dialog);
    QVERIFY(form.complete());

    // Bawaan: login Claude Code dengan status akun dari backend; kolom key belum bisa diisi
    QVERIFY(form.useLogin->isChecked());
    QVERIFY(!form.apiKey->isEnabled());
    const QString status = dialog->findChild<QLabel *>(QStringLiteral("integrationsLoginStatus"))->text();
    QVERIFY2(status.contains(QStringLiteral("dev@example.test")) && status.contains(QStringLiteral("Claude Pro")),
             qPrintable(status));
    QVERIFY(!form.error->isVisible());

    // API key dipilih tanpa isi: ditolak, dialog tetap terbuka, pilihan lama tetap berlaku
    form.useApiKey->click();
    QVERIFY(form.apiKey->isEnabled());
    QCOMPARE(form.apiKey->echoMode(), QLineEdit::Password);
    form.save->click();
    QVERIFY(dialog && dialog->isVisible());
    QVERIFY(form.error->isVisible());
    QCOMPARE(AgentAccess::load().method, AgentAccess::Method::Login);

    // Key salah tempel (berisi spasi) ditolak
    form.apiKey->setText(QStringLiteral("sk-ant api03"));
    form.save->click();
    QVERIFY(dialog && dialog->isVisible());
    QVERIFY2(form.error->text().contains(QStringLiteral("spasi")), qPrintable(form.error->text()));

    // "Tampilkan" memperlihatkan yang diketik, lalu disembunyikan lagi
    form.reveal->click();
    QCOMPARE(form.apiKey->echoMode(), QLineEdit::Normal);
    form.reveal->click();
    QCOMPARE(form.apiKey->echoMode(), QLineEdit::Password);

    // Key yang benar (spasi di tepi dibuang): tersimpan, dialog tertutup, konsol mencatat tanpa key-nya
    const QString key = QStringLiteral("sk-ant-api03-gui-9876");
    form.apiKey->setText(QStringLiteral("  %1  ").arg(key));
    form.save->click();
    QTRY_VERIFY(!dialog || !dialog->isVisible());
    QCOMPARE(AgentAccess::load().method, AgentAccess::Method::ApiKey);
    QCOMPARE(AgentAccess::load().apiKey, key);
    QCOMPARE(SecretStore::read(secretName), key);
    QVERIFY2(consoleText().contains(QStringLiteral("Akses agent: API key Anthropic")), qPrintable(consoleText()));
    QVERIFY(!consoleText().contains(QStringLiteral("sk-ant")));

    // Dibuka lagi: API key terpilih, kolomnya kosong dan hanya ujung key yang disebut.
    // Simpan tanpa mengetik mempertahankan key itu.
    dialog = openIntegrations();
    form = formOf(dialog);
    QVERIFY(form.complete());
    QVERIFY(form.useApiKey->isChecked());
    QVERIFY(form.apiKey->isEnabled());
    QVERIFY(form.apiKey->text().isEmpty());
    QVERIFY2(form.apiKey->placeholderText().contains(QStringLiteral("…9876")),
             qPrintable(form.apiKey->placeholderText()));
    QVERIFY(!form.apiKey->placeholderText().contains(QStringLiteral("api03")));
    form.save->click();
    QTRY_VERIFY(!dialog || !dialog->isVisible());
    QCOMPARE(AgentAccess::load().apiKey, key);

    // Batal tidak mengubah apa pun
    dialog = openIntegrations();
    form = formOf(dialog);
    QVERIFY(form.complete());
    form.useLogin->click();
    dialog->findChild<QPushButton *>(QStringLiteral("btnIntegrationsCancel"))->click();
    QTRY_VERIFY(!dialog || !dialog->isVisible());
    QCOMPARE(AgentAccess::load().method, AgentAccess::Method::ApiKey);
    QCOMPARE(AgentAccess::load().apiKey, key);

    // Kembali ke login Claude Code: key tersimpan ikut dihapus
    dialog = openIntegrations();
    form = formOf(dialog);
    QVERIFY(form.complete());
    form.useLogin->click();
    QVERIFY(!form.apiKey->isEnabled());
    form.save->click();
    QTRY_VERIFY(!dialog || !dialog->isVisible());
    QCOMPARE(AgentAccess::load().method, AgentAccess::Method::Login);
    QCOMPARE(SecretStore::read(secretName), QString());
    QVERIFY(consoleText().contains(QStringLiteral("Akses agent: login Claude Code")));
}

void TestGui::integrationsRunsAccountLogin() {
    AccountStatus loggedOut;
    loggedOut.state = AccountStatus::State::LoggedOut;
    m_runtime.accountStatus = loggedOut;

    QPointer<QDialog> dialog = openIntegrations();
    QVERIFY(dialog);
    auto *status = dialog->findChild<QLabel *>(QStringLiteral("integrationsLoginStatus"));
    auto *button = dialog->findChild<QPushButton *>(QStringLiteral("btnIntegrationsLogin"));
    auto *error = dialog->findChild<QLabel *>(QStringLiteral("integrationsError"));
    QVERIFY(status && button && error);
    QCOMPARE(int(m_runtime.logins.size()), 1);
    const QPointer<FakeAccountLogin> login = m_runtime.logins.last();
    QVERIFY(login);

    QVERIFY2(status->text().contains(QStringLiteral("Belum login")), qPrintable(status->text()));
    QCOMPARE(button->text(), QStringLiteral("Login lewat browser"));
    QVERIFY(button->isEnabled());

    // Login dimulai: menunggu browser, tombolnya jadi pembatal
    button->click();
    QVERIFY(login->isRunning());
    QVERIFY2(status->text().contains(QStringLiteral("Menunggu login di browser")), qPrintable(status->text()));
    QCOMPARE(button->text(), QStringLiteral("Batal login"));

    // Dibatalkan: kembali ke status semula, tanpa pesan gagal
    button->click();
    QVERIFY(!login->isRunning());
    QVERIFY(status->text().contains(QStringLiteral("Belum login")));
    QCOMPARE(button->text(), QStringLiteral("Login lewat browser"));
    QVERIFY(!error->isVisible());

    // Gagal: alasan dari CLI ditampilkan
    button->click();
    login->finishWith(false, QStringLiteral("Login failed: akun tidak diizinkan"), loggedOut);
    QVERIFY(error->isVisible());
    QVERIFY(error->text().contains(QStringLiteral("akun tidak diizinkan")));
    QCOMPARE(button->text(), QStringLiteral("Login lewat browser"));

    // Dicoba lagi dan berhasil: pesan gagal hilang, akun barunya tampil, tombol menawarkan ganti akun
    button->click();
    QVERIFY(!error->isVisible());
    AccountStatus loggedIn;
    loggedIn.state = AccountStatus::State::LoggedIn;
    loggedIn.account = QStringLiteral("dev@example.test");
    loggedIn.plan = QStringLiteral("Max");
    login->finishWith(true, QString(), loggedIn);
    QVERIFY2(status->text().contains(QStringLiteral("dev@example.test"))
                 && status->text().contains(QStringLiteral("Claude Max")),
             qPrintable(status->text()));
    QCOMPARE(button->text(), QStringLiteral("Ganti akun"));
    QVERIFY(!error->isVisible());

    dialog->findChild<QPushButton *>(QStringLiteral("btnIntegrationsCancel"))->click();
    QTRY_VERIFY(!dialog || !dialog->isVisible());

    // API key di luar aplikasi mengalahkan login: disebutkan supaya tidak mengejutkan
    loggedIn.keySource = QStringLiteral("ANTHROPIC_API_KEY");
    m_runtime.accountStatus = loggedIn;
    dialog = openIntegrations();
    QVERIFY(dialog);
    status = dialog->findChild<QLabel *>(QStringLiteral("integrationsLoginStatus"));
    QVERIFY2(status->text().contains(QStringLiteral("ANTHROPIC_API_KEY")), qPrintable(status->text()));
    dialog->reject();
    QTRY_VERIFY(!dialog || !dialog->isVisible());

    // Claude Code belum terpasang: tidak ada yang bisa dipakai login
    AccountStatus unavailable;
    unavailable.state = AccountStatus::State::Unavailable;
    m_runtime.accountStatus = unavailable;
    dialog = openIntegrations();
    QVERIFY(dialog);
    status = dialog->findChild<QLabel *>(QStringLiteral("integrationsLoginStatus"));
    button = dialog->findChild<QPushButton *>(QStringLiteral("btnIntegrationsLogin"));
    QVERIFY2(status->text().contains(QStringLiteral("belum terpasang")), qPrintable(status->text()));
    QVERIFY(!button->isEnabled());
    dialog->reject();
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

    // Jendela yang sudah ada: mis. swimlane, yang stylesheet-nya dari SwimlaneWidget.ui
    auto *lane = m_window->findChild<SwimlaneWidget *>();
    QVERIFY(lane);
    QVERIFY2(!lane->styleSheet().contains(QStringLiteral("#f7f9f8")), qPrintable(lane->styleSheet()));

    // Kembali ke terang: stylesheet asli dipulihkan
    Theme::setSchemeOverride(Theme::Scheme::Light);
    QVERIFY(Theme::apply(*qApp));
    QCOMPARE(widget.styleSheet(), QStringLiteral("color: #a9743f;"));
    QVERIFY(lane->styleSheet().contains(QStringLiteral("#f7f9f8")));
}

void TestGui::bundledFontsAreRegistered() {
    const QStringList families = AppFonts::registerBundled();
    QVERIFY2(families.size() >= 20, qPrintable(families.join(QStringLiteral(", "))));
    for (const QString &family : {QStringLiteral("Inter"), QStringLiteral("Nunito"), QStringLiteral("Lexend"),
                                  QStringLiteral("Atkinson Hyperlegible")}) {
        QVERIFY2(families.contains(family), qPrintable(family));
        QVERIFY(QFontDatabase::families().contains(family));
    }
    // Dipanggil lagi tidak menambah duplikat
    QCOMPARE(AppFonts::registerBundled(), families);
}

void TestGui::preferencesChangesFont() {
    const auto restore = qScopeGuard([]() {
        AppFonts::save(QString());
        Theme::setUiFontFamily(QString());
        qApp->setStyleSheet(QString());
    });
    AppFonts::save(QString());

    // File > Preferences (di bawah Integrations) membuka pemilih font
    QAction *preferences = m_window->findChild<QAction *>(QStringLiteral("actionPreferences"));
    QVERIFY(preferences);
    auto *fileMenu = qobject_cast<QMenu *>(preferences->parent());
    QVERIFY(fileMenu);
    const QList<QAction *> fileActions = fileMenu->actions();
    const qsizetype at = fileActions.indexOf(preferences);
    QVERIFY(at > 0);
    QCOMPARE(fileActions.at(at - 1)->text(), QStringLiteral("Integrations…"));

    auto openPicker = [&]() -> QDialog * {
        QAction *changeFont = preferences;
        changeFont->trigger();
        QDialog *dialog = nullptr;
        QTest::qWaitFor([&]() {
            dialog = nullptr;
            for (QDialog *candidate : m_window->findChildren<QDialog *>(QStringLiteral("fontPickerDialog"))) {
                if (candidate->isVisible()) {
                    dialog = candidate;
                }
            }
            return dialog != nullptr;
        });
        return dialog;
    };

    QDialog *dialog = openPicker();
    QVERIFY(dialog);
    auto *list = dialog->findChild<QListWidget *>(QStringLiteral("fontList"));
    QVERIFY(list);
    QVERIFY(list->count() >= 21);
    // Pilihan pertama: font yang sekarang, dan itu yang terpilih
    QVERIFY(list->item(0)->text().contains(QStringLiteral("Garamond")));
    QCOMPARE(list->currentRow(), 0);

    // Pilih Inter: pratinjau ikut berubah, lalu Terapkan
    QList<QListWidgetItem *> inter = list->findItems(QStringLiteral("Inter"), Qt::MatchExactly);
    QCOMPARE(inter.size(), 1);
    list->setCurrentItem(inter.first());
    auto *preview = dialog->findChild<QLabel *>(QStringLiteral("fontPreview"));
    QVERIFY(preview);
    QCOMPARE(preview->font().family(), QStringLiteral("Inter"));
    const QPointer<QDialog> first = dialog;
    dialog->findChild<QPushButton *>(QStringLiteral("btnFontApply"))->click();
    QTRY_VERIFY(!first || !first->isVisible());

    QCOMPARE(AppFonts::saved(), QStringLiteral("Inter"));
    QVERIFY2(qApp->styleSheet().contains(QStringLiteral("font-family: \"Inter\"")), "font override missing");

    // Dibuka lagi: Inter yang terpilih; kembali ke bawaan menghapus override
    dialog = openPicker();
    QVERIFY(dialog);
    list = dialog->findChild<QListWidget *>(QStringLiteral("fontList"));
    QCOMPARE(list->currentItem()->text(), QStringLiteral("Inter"));
    list->setCurrentRow(0);
    dialog->findChild<QPushButton *>(QStringLiteral("btnFontApply"))->click();
    QTRY_COMPARE(AppFonts::saved(), QString());
    QVERIFY(!qApp->styleSheet().contains(QStringLiteral("font-family: \"Inter\"")));
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

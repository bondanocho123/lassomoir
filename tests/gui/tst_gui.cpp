#include "CodeMetrics.h"
#include "FakeAgentRuntime.h"
#include "KanbanCardWidget.h"
#include "KanbanColumnWidget.h"
#include "MaintainabilityView.h"
#include "MarkdownView.h"
#include "MermaidRenderer.h"
#include "PromptComposer.h"
#include "ResponseDrawer.h"
#include "StageCatalog.h"
#include "SwarmCoordinator.h"
#include "SwimlaneWidget.h"
#include "TaskManager.h"
#include "WorkspaceDiff.h"
#include "mainwindow.h"

#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QScopeGuard>
#include <QSplitter>
#include <QStandardPaths>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QTimer>
#include <QTreeWidget>
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

    void cardShowsHandCursor();
    void doubleClickEditsTask();
    void cancelledEditKeepsTask();

    // Review gate, drawer, dan viewer
    void specifierReviewApproveFlow();
    void revisionRerunsWithPreviousDocument();
    void dragPastGateIsRejected();
    void drawerShowsLiveOutput();
    void drawerExpandFillsBoardAndRestores();
    void reviewSurvivesRestart();
    void markdownViewRendersMermaid();
    void architectDrawerShowsCodeChanges();
    void maintainabilityViewShowsCSharpMembers();
    void umlTabShowsClassAndAgentDiagrams();

private:
    // Dialog modal membuka event loop sendiri di dalam handler klik; timer ini jalan di loop itu
    // dan mengisi/menutup dialog. m_dialogSeen tetap false bila dialog tidak pernah muncul.
    void driveModalDialog(std::function<void(QDialog *)> action);
    bool m_dialogSeen = false;

    // Rakit TaskManager + MainWindow seperti main.cpp (termasuk runFinished -> recordRun)
    void createWindow();
    // Jalankan SPECIFIER task t3 dengan runtime palsu sampai menunggu review
    void finishSpecifierRun(const QString &document);

    KanbanCardWidget *card(const QString &id) const;
    QPushButton *runButton(const QString &id) const;
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

QString TestGui::consoleText() const {
    auto *log = m_window->findChild<QPlainTextEdit *>(QStringLiteral("textBrowserLog"));
    return log ? log->toPlainText() : QString();
}

ResponseDrawer *TestGui::drawer() const {
    return m_window->findChild<ResponseDrawer *>();
}

void TestGui::finishSpecifierRun(const QString &document) {
    const int before = int(m_runtime.sessions.size());
    runButton(QStringLiteral("t3"))->click();
    QCOMPARE(int(m_runtime.sessions.size()), before + 1);
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
    QCOMPARE(workingDirButton->text(), QStringLiteral("Folder: %1").arg(QDir(m_workDir.path()).dirName()));

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

void TestGui::cardShowsHandCursor() {
    KanbanCardWidget *coder = card(QStringLiteral("t1"));
    QVERIFY(coder);
    QCOMPARE(coder->cursor().shape(), Qt::PointingHandCursor);

    // Anak kartu (label judul) ikut mewarisi, jadi seluruh area kartu berkursor tangan
    auto *title = coder->findChild<QLabel *>(QStringLiteral("labelTitle"));
    QVERIFY(title);
    QCOMPARE(title->cursor().shape(), Qt::PointingHandCursor);
}

void TestGui::doubleClickEditsTask() {
    KanbanCardWidget *coder = card(QStringLiteral("t1"));
    QVERIFY(coder);

    driveModalDialog([](QDialog *dialog) {
        QCOMPARE(dialog->windowTitle(), QStringLiteral("Edit Task"));

        // Field terisi data kartu; urutan objectName "taskFormInput": judul lalu subtext
        const QList<QLineEdit *> inputs = dialog->findChildren<QLineEdit *>(QStringLiteral("taskFormInput"));
        QCOMPARE(inputs.size(), 2);
        QCOMPARE(inputs[0]->text(), QStringLiteral("Tulis hello"));

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
        inputs[1]->setText(QStringLiteral("PIC: Budi"));

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

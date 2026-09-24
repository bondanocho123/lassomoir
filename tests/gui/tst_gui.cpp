#include "FakeAgentRuntime.h"
#include "KanbanCardWidget.h"
#include "KanbanColumnWidget.h"
#include "MarkdownView.h"
#include "MermaidRenderer.h"
#include "PromptComposer.h"
#include "ResponseDrawer.h"
#include "StageCatalog.h"
#include "SwarmCoordinator.h"
#include "SwimlaneWidget.h"
#include "TaskManager.h"
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
#include <QPushButton>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QTimer>
#include <QtTest>

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
    void reviewSurvivesRestart();
    void markdownViewRendersMermaid();

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
        const QList<QComboBox *> combos = dialog->findChildren<QComboBox *>(QStringLiteral("taskFormCombo"));
        QCOMPARE(combos.size(), 2);
        QCOMPARE(combos[1]->currentText(), QStringLiteral("CODER"));
        QVERIFY(!combos[1]->isEnabled());

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

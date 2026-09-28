#include "CodeMetrics.h"
#include "FakeAgentRuntime.h"
#include "GitSandbox.h"
#include "KanbanCardWidget.h"
#include "KanbanColumnWidget.h"
#include "MaintainabilityView.h"
#include "MarkdownView.h"
#include "MermaidRenderer.h"
#include "PromptComposer.h"
#include "PromptEditor.h"
#include "ResponseDrawer.h"
#include "StageCatalog.h"
#include "SwarmCoordinator.h"
#include "SwimlaneWidget.h"
#include "TaskAttachments.h"
#include "TaskManager.h"
#include "WorkspaceDiff.h"
#include "mainwindow.h"

#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QScopeGuard>
#include <QScrollArea>
#include <QSet>
#include <QSplitter>
#include <QStandardPaths>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
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

    void cardShowsHandCursor();
    void workingDirButtonShowsIconAndCaption();
    void doubleClickEditsTask();
    void cancelledEditKeepsTask();
    void deleteTaskFromCardMenu();

    // Review gate, drawer, dan viewer
    void specifierReviewApproveFlow();
    void revisionRerunsWithPreviousDocument();
    void dragPastGateIsRejected();
    void drawerShowsLiveOutput();
    void drawerExpandFillsBoardAndRestores();
    void reviewSurvivesRestart();
    void markdownViewRendersMermaid();
    void architectDrawerShowsCodeChanges();
    void coderDrawerShowsCodeChanges();
    // Alur git: CODER di worktree -> QA (commit + push) -> dikembalikan -> QA lagi -> merge
    void qaFlowCommitsPushesAndMerges();
    void maintainabilityViewShowsCSharpMembers();
    void umlTabShowsClassAndAgentDiagrams();

    // Kotak prompt berfoto, lampiran Excel/Word/CSV, dan folder referensi
    void promptEditorAttachesPhotosAndDocuments();
    void newTaskWithAttachmentsFeedsRun();
    void referenceFoldersFeedRuns();

private:
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

    // Dicoba lagi dan sukses: worktree sudah terpasang, jadi run langsung jalan di sana; kartu maju
    // ke CLEANER, dan drawer yang mengikutinya tidak punya tab diff
    runButton(QStringLiteral("t1"))->click();
    QCOMPARE(int(m_runtime.sessions.size()), 2);
    QCOMPARE(m_runtime.sessions.last()->launch().workingDirectory, workPath);
    m_runtime.sessions.last()->finishWith(successResult(QStringLiteral("Selesai")));
    QCOMPARE(m_tasks->task(QStringLiteral("t1"))->stage, QStringLiteral("CLEANER"));
    QVERIFY(tabs->isHidden());
}

void TestGui::qaFlowCommitsPushesAndMerges() {
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

    // Drawer QA: tombol setuju sekaligus merge; QA menolak lewat "Kembalikan ke CODER"
    runButton(QStringLiteral("t5"))->click();   // 📋
    ResponseDrawer *panel = drawer();
    QVERIFY(panel && !panel->isHidden());
    QCOMPARE(panel->taskId(), QStringLiteral("t5"));
    auto *approve = panel->findChild<QPushButton *>(QStringLiteral("btnDrawerApprove"));
    QCOMPARE(approve->text(), QStringLiteral("Setujui && merge ke main"));
    auto *branchLabel = panel->findChild<QLabel *>(QStringLiteral("drawerBranch"));
    QCOMPARE(branchLabel->text(), QStringLiteral("Branch lassomoir/t5-tambah-login dari main"));
    panel->findChild<QPlainTextEdit *>(QStringLiteral("drawerNote"))->setPlainText(QStringLiteral("Tambah validasi"));
    QCOMPARE(panel->findChild<QComboBox *>(QStringLiteral("drawerSendBackTarget"))->currentText(), QStringLiteral("CODER"));
    panel->findChild<QPushButton *>(QStringLiteral("btnDrawerSendBack"))->click();
    QCOMPARE(m_tasks->task(QStringLiteral("t5"))->stage, QStringLiteral("CODER"));
    QCOMPARE(card(QStringLiteral("t5"))->badge(), QStringLiteral("✓ 1 · ↺ 1"));

    // Kartu yang sama dikerjakan lagi di worktree yang sama; sukses langsung kembali ke QA
    runButton(QStringLiteral("t5"))->click();
    QCOMPARE(int(m_runtime.sessions.size()), 3);   // worktree sudah terpasang: tanpa menunggu git
    QCOMPARE(m_runtime.sessions.last()->launch().workingDirectory, worktree);
    QVERIFY(m_runtime.sessions.last()->launch().prompt.contains(QStringLiteral("# Hasil stage sebelumnya (QA)")));
    QVERIFY(writeFile(worktree + QStringLiteral("/login.txt"), "form\nvalidasi\n"));
    m_runtime.sessions.last()->finishWith(successResult(QStringLiteral("Validasi ditambah")));
    QCOMPARE(m_tasks->task(QStringLiteral("t5"))->stage, QStringLiteral("QA"));

    // ▶ QA putaran 2: commit kedua, dan QA melihat laporannya yang dulu
    runButton(QStringLiteral("t5"))->click();
    QTRY_COMPARE_WITH_TIMEOUT(int(m_runtime.sessions.size()), 4, kGitTimeoutMs);
    QVERIFY(m_runtime.sessions.last()->launch().prompt.contains(
        QStringLiteral("# Hasil sebelumnya di stage ini (sebelum dikembalikan)\n\nKriteria 1 gagal")));
    QVERIFY(GitSandbox::output(worktree, {QStringLiteral("log"), QStringLiteral("-1"), QStringLiteral("--format=%b")})
                .contains(QStringLiteral("putaran 2")));
    m_runtime.sessions.last()->finishWith(successResult(QStringLiteral("Semua kriteria lulus")));

    // Setujui & merge: kode masuk main di folder kerja project, origin terbarui, worktree dibuang → DONE
    runButton(QStringLiteral("t5"))->click();   // 📋
    QVERIFY(!panel->isHidden());
    QVERIFY(approve->isVisibleTo(panel));
    approve->click();
    QTRY_COMPARE_WITH_TIMEOUT(m_tasks->task(QStringLiteral("t5"))->stage, QStringLiteral("DONE"), kGitTimeoutMs);
    QCOMPARE(readFile(dir.filePath(QStringLiteral("login.txt"))), QByteArray("form\nvalidasi\n"));
    QCOMPARE(GitSandbox::output(origin, {QStringLiteral("rev-parse"), QStringLiteral("main")}),
             GitSandbox::output(dir.path(), {QStringLiteral("rev-parse"), QStringLiteral("HEAD")}));
    QVERIFY(!QFileInfo::exists(worktree));
    const TaskBranch merged = m_tasks->task(QStringLiteral("t5"))->branch;
    QVERIFY(!merged.mergedCommit.isEmpty());
    QVERIFY(!merged.hasWorktree());
    QCOMPARE(card(QStringLiteral("t5"))->badge(), QStringLiteral("✓ 2 · ↺ 1"));
    QVERIFY(consoleText().contains(QStringLiteral("[GIT] Demo/Tambah login merge lassomoir/t5-tambah-login → main")));
    QVERIFY(consoleText().contains(QStringLiteral("[GATE] Demo/Tambah login QA disetujui → DONE")));
    QVERIFY(branchLabel->text().startsWith(QStringLiteral("Di-merge ke main @ ") + merged.mergedCommit));

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

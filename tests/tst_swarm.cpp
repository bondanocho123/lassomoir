#include "ClassDiagram.h"
#include "ClaudeCli.h"
#include "ClaudeCodeRuntime.h"
#include "CodeMetrics.h"
#include "DocumentText.h"
#include "EdgeMermaidRenderer.h"
#include "FakeAgentRuntime.h"
#include "FileManager.h"
#include "MermaidRenderer.h"
#include "PromptComposer.h"
#include "RunLogFormatter.h"
#include "StageCatalog.h"
#include "StreamJsonParser.h"
#include "SwarmCoordinator.h"
#include "TaskAttachments.h"
#include "TaskItem.h"
#include "TaskManager.h"
#include "WorkspaceDiff.h"
#include "WorkspaceGuard.h"

// Fixture .xlsx/.docx dibuat di test dengan penulis ZIP milik Qt (QtCore privat, lihat tests.pro)
#include <QtCore/private/qzipwriter_p.h>

#include <QBuffer>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRandomGenerator>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>

#include <cmath>
#include <cstdio>
#include <memory>

#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#endif

namespace {

// Bila variabel ini di-set, exe test ini berperan sebagai `claude` palsu (lihat runFakeClaude)
constexpr char kFakeClaudeEnv[] = "LASSOMOIR_FAKE_CLAUDE";

QByteArray readResource(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QByteArray();
    }
    return file.readAll();
}

QList<AgentEvent> parseFixture(const QString &name) {
    QList<AgentEvent> events;
    const QList<QByteArray> lines = readResource(QStringLiteral(":/fixtures/%1").arg(name)).split('\n');
    for (const QByteArray &line : lines) {
        events += StreamJsonParser::parseLine(line);
    }
    return events;
}

TaskItem makeTask(const QString &id, const QString &stage, const QString &projectId = QStringLiteral("P")) {
    TaskItem task;
    task.id = id;
    task.projectId = projectId;
    task.stage = stage;
    task.title = QStringLiteral("Task %1").arg(id);
    task.category = QStringLiteral("utility");
    return task;
}

AgentResult successResult() {
    AgentResult result;
    result.success = true;
    result.outcome = QStringLiteral("success");
    return result;
}

StageRun stageRun(const QString &stage, bool success, const QString &message,
                  const QString &outcome = QString()) {
    AgentResult result;
    result.success = success;
    result.outcome = !outcome.isEmpty() ? outcome
                                        : (success ? QStringLiteral("success") : QStringLiteral("error_during_execution"));
    result.message = message;
    return StageRun::finished(stage, result);
}

QString argValue(const QStringList &args, const QString &flag) {
    const qsizetype index = args.indexOf(flag);
    return index >= 0 && index + 1 < args.size() ? args.at(index + 1) : QString();
}

// Session palsu milik task tertentu, dikenali dari judul di prompt-nya
FakeAgentSession *sessionFor(const FakeAgentRuntime &runtime, const QString &taskId) {
    const QString marker = QStringLiteral("Judul: Task %1\n").arg(taskId);
    for (const QPointer<FakeAgentSession> &session : runtime.sessions) {
        if (session && session->launch().prompt.contains(marker)) {
            return session;
        }
    }
    return nullptr;
}

AgentLaunch fakeLaunch(const QString &workingDirectory, int timeoutMs = 15000) {
    AgentLaunch launch;
    launch.agent = *StageCatalog::standard().profile(QStringLiteral("CODER"))->agent();
    launch.agent.timeoutMs = timeoutMs;
    launch.prompt = QStringLiteral("# Task\nJudul: Uji \u2713 \"kutip\" & <tag>\n");
    launch.workingDirectory = workingDirectory;
    return launch;
}

// Dijalankan sebagai proses anak oleh ClaudeCodeSession
int runFakeClaude(const QString &mode) {
#ifdef Q_OS_WIN
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    // Seperti claude -p: baca prompt dari stdin sampai EOF
    QByteArray prompt;
    char buffer[4096];
    size_t count;
    while ((count = std::fread(buffer, 1, sizeof buffer, stdin)) > 0) {
        prompt.append(buffer, qsizetype(count));
    }

    if (mode.startsWith(QLatin1String("print:"))) {
        QByteArray data = readResource(QStringLiteral(":/fixtures/%1").arg(mode.mid(6)));
        // Baris terakhir sengaja tanpa '\n': menguji pembacaan sisa buffer saat proses selesai
        if (data.endsWith('\n')) {
            data.chop(1);
        }
        std::fwrite(data.constData(), 1, size_t(data.size()), stdout);
        return 0;
    }
    if (mode.startsWith(QLatin1String("echo:"))) {
        QJsonObject echo;
        echo[QStringLiteral("args")] = QJsonArray::fromStringList(QCoreApplication::arguments().mid(1));
        echo[QStringLiteral("stdin")] = QString::fromUtf8(prompt);
        QFile file(mode.mid(5));
        if (file.open(QIODevice::WriteOnly)) {
            file.write(QJsonDocument(echo).toJson());
        }
        const char result[] = R"({"type":"result","subtype":"success","is_error":false,"result":"ok"})" "\n";
        std::fwrite(result, 1, sizeof result - 1, stdout);
        return 0;
    }
    if (mode == QLatin1String("hang")) {
        QThread::sleep(60);   // dihentikan lewat cancel atau timeout jauh sebelum ini
        return 0;
    }
    if (mode == QLatin1String("fail")) {
        std::fputs("Error: belum login\n", stderr);
        return 3;
    }
    return 1;
}

// Mengaktifkan mode claude palsu selama objek ini hidup
class FakeClaudeMode {
public:
    explicit FakeClaudeMode(const QString &mode) { qputenv(kFakeClaudeEnv, mode.toUtf8()); }
    ~FakeClaudeMode() { qunsetenv(kFakeClaudeEnv); }
};

struct SessionRecorder {
    QList<AgentEvent> events;
    QList<AgentResult> results;

    void attach(AgentSession *session) {
        QObject::connect(session, &AgentSession::eventReceived, session,
                         [this](const AgentEvent &event) { events.append(event); });
        QObject::connect(session, &AgentSession::finished, session,
                         [this](const AgentResult &result) { results.append(result); });
    }

    QStringList toolNames() const {
        QStringList names;
        for (const AgentEvent &event : events) {
            if (event.kind == AgentEvent::Kind::ToolUse) {
                names.append(event.toolName);
            }
        }
        return names;
    }
};

// Coordinator dengan katalog asli dan runtime palsu, plus pencatat sinyalnya
struct SwarmFixture {
    StageCatalog catalog = StageCatalog::standard();
    FakeAgentRuntime runtime;
    TaskPromptComposer composer;
    SwarmCoordinator swarm{catalog, runtime, composer};
    QTemporaryDir dir;

    int queued = 0;
    int started = 0;
    QStringList finishedIds;
    QList<AgentResult> results;

    SwarmFixture() {
        QObject::connect(&swarm, &SwarmCoordinator::runQueued, &swarm,
                         [this](const TaskItem &) { ++queued; });
        QObject::connect(&swarm, &SwarmCoordinator::runStarted, &swarm,
                         [this](const TaskItem &, const AgentLaunch &) { ++started; });
        QObject::connect(&swarm, &SwarmCoordinator::runFinished, &swarm,
                         [this](const TaskItem &task, const AgentResult &result) {
            finishedIds.append(task.id);
            results.append(result);
        });
    }

    bool run(const QString &id, const QString &stage, const QString &projectId = QStringLiteral("P"),
             QString *reason = nullptr) {
        return swarm.run(makeTask(id, stage, projectId), dir.path(), reason);
    }
};

// git sungguhan untuk menyiapkan repository uji; identitas dan signing dipaksa supaya tidak
// bergantung pada konfigurasi global mesin
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
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(content) == content.size();
}

const TypeMetrics *findType(const QList<TypeMetrics> &types, const QString &name) {
    for (const TypeMetrics &type : types) {
        if (type.name == name) {
            return &type;
        }
    }
    return nullptr;
}

const MemberMetrics *findMember(const TypeMetrics &type, const QString &name) {
    for (const MemberMetrics &member : type.members) {
        if (member.name == name) {
            return &member;
        }
    }
    return nullptr;
}

QStringList memberNames(const TypeMetrics &type) {
    QStringList names;
    for (const MemberMetrics &member : type.members) {
        names.append(member.name);
    }
    return names;
}

QStringList diffPaths(const WorkspaceDiff &diff) {
    QStringList paths;
    for (const FileDiff &file : diff.files) {
        paths.append(file.path);
    }
    return paths;
}

// Arsip ZIP berisi bagian Office yang dibaca DocumentText; bagian lain tidak perlu ada
bool writeZip(const QString &path, const QList<QPair<QString, QByteArray>> &parts) {
    QZipWriter zip(path);
    zip.setCompressionPolicy(QZipWriter::AlwaysCompress);
    for (const QPair<QString, QByteArray> &part : parts) {
        zip.addFile(part.first, part.second);
    }
    zip.close();
    return zip.status() == QZipWriter::NoError;
}

QByteArray imageBytes(const QImage &image, const char *format) {
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, format);
    return bytes;
}

QByteArray pngBytes(const QSize &size, const QColor &color) {
    QImage image(size, QImage::Format_RGB32);
    image.fill(color);
    return imageBytes(image, "PNG");
}

}

class TestSwarm : public QObject {
    Q_OBJECT

private slots:
    // Konfigurasi stage
    void catalogKeepsPipelineOrder();
    void catalogGivesAgentsOnlyToWorkingStages();
    void approvalGateNeedsApproval();

    // Parser, argumen CLI, prompt
    void parserReadsPlainAnswer();
    void parserReadsToolCalls();
    void parserReportsDeniedTools();
    void parserIgnoresNoise();
    void cliArgumentsFollowAgentDefinition();
    void catalogDefaultsAreFrugal();
    void taskTuningOverridesStageDefaults();
    void promptComposerSkipsEmptyFields();

    // Gerombolan
    void workspaceGuardAllowsOneWriterPerFolder();
    void swarmRespectsMaxConcurrent();
    void writersInSameFolderTakeTurns();
    void cancelRemovesQueuedAndStopsRunning();
    void runRejectsInvalidRequests();
    void cancelProjectOnlyTouchesThatProject();
    void sessionFinishingInsideStartIsSafe();

    // Tampilan konsol
    void logFormatterSummaries();

    // Proses claude (palsu)
    void claudeSessionStreamsEvents();
    void claudeSessionSendsPromptThroughStdin();
    void claudeSessionCancelStopsProcess();
    void claudeSessionTimesOut();
    void claudeSessionReportsStderrWithoutResult();
    void claudeSessionWithoutProgramFinishesOnce();

    // Review gate & serah-terima antar stage
    void taskManagerAdvancesAndGates();
    void taskManagerReviewDecisions();
    void manualMoveRespectsGate();
    void composerBuildsHandoffPrompt();
    void fileManagerRoundTripsRunsAndState();

    // Mermaid (fungsi murni)
    void mermaidHelpers();

    // Perubahan kode untuk stage peninjauan (parser murni + git sungguhan)
    void gitDiffParsesUnifiedDiff();
    void gitDiffCollectsWorkspaceChanges();
    void gitDiffHandlesMissingRepositoryAndFirstCommit();
    void codeMetricsFormulaAndRating();
    void codeMetricsCountsBraceLanguages();
    void codeMetricsCountsPython();
    void gitDiffMeasuresMaintainability();
    void csharpMetricsParsesTypesAndMembers();
    void csharpMetricsHandlesStringsAndNullable();
    void csharpMetricsResolvesInheritance();
    void gitDiffMeasuresCSharpTypes();
    void gitDiffCanSkipMetrics();

    // Diagram kelas UML (Mermaid) dari tipe C# yang berubah
    void csharpMetricsRecordsOutline();
    void classDiagramDrawsChangedTypes();
    void classDiagramLimitsSize();

    // Lampiran task (foto, Excel/Word/CSV) dan folder referensi project
    void documentTextReadsCsv();
    void documentTextReadsDocx();
    void documentTextReadsXlsx();
    void documentTextRejectsUnsupportedFiles();
    void attachmentNamesAreSafeAndUnique();
    void attachmentsSaveAndRemoveFiles();
    void imagesAreNormalizedForClaude();
    void materialsReadAttachmentsAndReferences();
    void composerAddsInstructionsAttachmentsAndReferences();
    void cliGrantsReadOnlyFoldersAndStreamInput();
    void userMessageCarriesImages();
    void claudeSessionSendsImagesAsStreamJson();
    void swarmPassesMaterialsToLaunch();

    // Claude Code sungguhan; hanya jalan bila LASSOMOIR_REAL_CLAUDE=1 (memakai token)
    void realClaudeRunsCoderTask();
    void realClaudeReadsAttachmentsAndReferences();
    // Edge sungguhan; hanya jalan bila LASSOMOIR_REAL_MERMAID=1 (tanpa token)
    void realEdgeRendersMermaid();
};

void TestSwarm::catalogKeepsPipelineOrder() {
    const StageCatalog catalog = StageCatalog::standard();
    QCOMPARE(catalog.keys(), QStringList({"WAITING", "SPECIFIER", "CODER", "CLEANER",
                                          "ARCHITECT", "HARDENER", "QA", "DONE"}));
    QVERIFY(!catalog.profile(QStringLiteral("TIDAK_ADA")));
}

void TestSwarm::catalogGivesAgentsOnlyToWorkingStages() {
    const StageCatalog catalog = StageCatalog::standard();
    QVERIFY(!catalog.profile(QStringLiteral("WAITING"))->agent());
    QVERIFY(!catalog.profile(QStringLiteral("DONE"))->agent());

    const QList<const StageProfile *> agentStages = catalog.agentStages();
    QCOMPARE(int(agentStages.size()), 6);
    for (const StageProfile *profile : agentStages) {
        // Instruksi peran terbaca dari resource dan menyebut stage-nya sendiri
        QVERIFY2(profile->agent()->rolePrompt.contains(profile->key()), qPrintable(profile->key()));
    }

    QVERIFY(catalog.profile(QStringLiteral("SPECIFIER"))->exitPolicy().isGated());
    QVERIFY(catalog.profile(QStringLiteral("ARCHITECT"))->exitPolicy().isGated());
    QVERIFY(catalog.profile(QStringLiteral("QA"))->exitPolicy().isGated());
    QVERIFY(!catalog.profile(QStringLiteral("CODER"))->exitPolicy().isGated());

    QVERIFY(!catalog.profile(QStringLiteral("SPECIFIER"))->agent()->writesWorkspace());
    QVERIFY(!catalog.profile(QStringLiteral("ARCHITECT"))->agent()->writesWorkspace());
    QVERIFY(catalog.profile(QStringLiteral("CODER"))->agent()->writesWorkspace());
    QCOMPARE(catalog.profile(QStringLiteral("SPECIFIER"))->agent()->maxConcurrent, 3);
}

void TestSwarm::approvalGateNeedsApproval() {
    const ApprovalGate gate;
    TaskItem task = makeTask(QStringLiteral("1"), QStringLiteral("SPECIFIER"));
    QString reason;
    QVERIFY(!gate.canLeave(task, &reason));   // belum pernah run
    QVERIFY(reason.contains(QStringLiteral("SPECIFIER")));

    StageRun run = stageRun(QStringLiteral("SPECIFIER"), true, QStringLiteral("spek"));
    task.runs = {run};
    QVERIFY(!gate.canLeave(task, nullptr));   // sudah run, belum disetujui

    run.decision = ReviewDecision::Approved;
    task.runs = {run};
    QVERIFY(gate.canLeave(task, nullptr));
    QCOMPARE(task.approvedGates(), 1);

    // Persetujuan di SPECIFIER tidak ikut meloloskan gate QA
    task.stage = QStringLiteral("QA");
    QVERIFY(!gate.canLeave(task, nullptr));
    QVERIFY(AutoAdvance().canLeave(TaskItem(), nullptr));
}

void TestSwarm::parserReadsPlainAnswer() {
    const QList<AgentEvent> events = parseFixture(QStringLiteral("text.jsonl"));
    QCOMPARE(int(events.size()), 2);
    QVERIFY(events.at(0).kind == AgentEvent::Kind::Text);
    QCOMPARE(events.at(0).text, QStringLiteral("Halo"));

    QVERIFY(events.at(1).kind == AgentEvent::Kind::Result);
    const AgentResult result = events.at(1).result;
    QVERIFY(result.success);
    QCOMPARE(result.totalTokens, qint64(2910));
    QCOMPARE(result.durationMs, qint64(5782));
    QCOMPARE(result.sessionId, QStringLiteral("105e3a30-578a-41e7-9b57-b5de8ce25466"));
    QVERIFY(result.deniedTools.isEmpty());
}

void TestSwarm::parserReadsToolCalls() {
    const QList<AgentEvent> events = parseFixture(QStringLiteral("tool.jsonl"));
    QStringList tools;
    for (const AgentEvent &event : events) {
        if (event.kind == AgentEvent::Kind::ToolUse) {
            tools.append(event.toolName);
        }
    }
    QCOMPARE(tools, QStringList({"Read", "Write", "Bash"}));
    QVERIFY(events.first().toolDetail.endsWith(QStringLiteral("a.txt")));
    QVERIFY(events.last().kind == AgentEvent::Kind::Result);
    QVERIFY(events.last().result.success);
    QVERIFY(events.last().result.deniedTools.isEmpty());
}

void TestSwarm::parserReportsDeniedTools() {
    const QList<AgentEvent> events = parseFixture(QStringLiteral("deny.jsonl"));
    QVERIFY(events.last().kind == AgentEvent::Kind::Result);
    // Denial tidak membuat run gagal; hanya dicatat
    QVERIFY(events.last().result.success);
    QCOMPARE(events.last().result.deniedTools, QStringList({"Bash"}));
}

void TestSwarm::parserIgnoresNoise() {
    QVERIFY(StreamJsonParser::parseLine(QByteArray()).isEmpty());
    QVERIFY(StreamJsonParser::parseLine("bukan json").isEmpty());
    QVERIFY(StreamJsonParser::parseLine(R"({"type":"assistant","message":{)").isEmpty());
    QVERIFY(StreamJsonParser::parseLine(R"({"type":"system","subtype":"init"})").isEmpty());
    QVERIFY(StreamJsonParser::parseLine(R"({"type":"rate_limit_event"})").isEmpty());
    QVERIFY(StreamJsonParser::parseLine(
                R"({"type":"assistant","message":{"content":[{"type":"thinking","thinking":"..."},{"type":"text","text":"  "}]}})")
                .isEmpty());

    // Satu pesan assistant dengan dua blok menghasilkan dua event
    const QList<AgentEvent> both = StreamJsonParser::parseLine(
        R"({"type":"assistant","message":{"content":[{"type":"text","text":"Cari dulu"},{"type":"tool_use","name":"Grep","input":{"pattern":"TODO"}}]}})");
    QCOMPARE(int(both.size()), 2);
    QCOMPARE(both.at(0).text, QStringLiteral("Cari dulu"));
    QCOMPARE(both.at(1).toolName, QStringLiteral("Grep"));
    QCOMPARE(both.at(1).toolDetail, QStringLiteral("TODO"));
}

void TestSwarm::cliArgumentsFollowAgentDefinition() {
    const StageCatalog catalog = StageCatalog::standard();

    const QStringList coder = ClaudeCli::arguments(*catalog.profile(QStringLiteral("CODER"))->agent());
    QCOMPARE(coder.first(), QStringLiteral("-p"));
    QCOMPARE(argValue(coder, QStringLiteral("--output-format")), QStringLiteral("stream-json"));
    QVERIFY(coder.contains(QStringLiteral("--verbose")));
    QCOMPARE(argValue(coder, QStringLiteral("--tools")), QStringLiteral("Read,Grep,Glob,Edit,Write,Bash"));
    QCOMPARE(argValue(coder, QStringLiteral("--allowedTools")), QStringLiteral("Bash(git *)"));
    QCOMPARE(argValue(coder, QStringLiteral("--permission-mode")), QStringLiteral("acceptEdits"));
    QCOMPARE(argValue(coder, QStringLiteral("--permission-prompts")), QStringLiteral("none"));
    QVERIFY(coder.contains(QStringLiteral("--strict-mcp-config")));
    QCOMPARE(argValue(coder, QStringLiteral("--model")), QStringLiteral("sonnet"));
    QCOMPARE(argValue(coder, QStringLiteral("--effort")), QStringLiteral("medium"));
    QVERIFY(argValue(coder, QStringLiteral("--append-system-prompt")).startsWith(QStringLiteral("Kamu adalah agen CODER")));

    const QStringList specifier = ClaudeCli::arguments(*catalog.profile(QStringLiteral("SPECIFIER"))->agent());
    QVERIFY(!specifier.contains(QStringLiteral("--allowedTools")));
    QCOMPARE(argValue(specifier, QStringLiteral("--tools")), QStringLiteral("Read,Grep,Glob"));

    // Model kosong = CLI memakai model bawaannya, jadi flag tidak dikirim
    AgentDefinition bare = *catalog.profile(QStringLiteral("CODER"))->agent();
    bare.model.clear();
    bare.effort.clear();
    const QStringList bareArgs = ClaudeCli::arguments(bare);
    QVERIFY(!bareArgs.contains(QStringLiteral("--model")));
    QVERIFY(!bareArgs.contains(QStringLiteral("--effort")));
}

void TestSwarm::catalogDefaultsAreFrugal() {
    const StageCatalog catalog = StageCatalog::standard();
    auto tuningOf = [&catalog](const char *stage) {
        const AgentDefinition &agent = *catalog.profile(QString::fromLatin1(stage))->agent();
        return QStringList{agent.model, agent.effort};
    };
    QCOMPARE(tuningOf("SPECIFIER"), QStringList({"sonnet", "medium"}));
    QCOMPARE(tuningOf("CODER"), QStringList({"sonnet", "medium"}));
    QCOMPARE(tuningOf("CLEANER"), QStringList({"haiku", "low"}));
    QCOMPARE(tuningOf("ARCHITECT"), QStringList({"sonnet", "medium"}));
    QCOMPARE(tuningOf("HARDENER"), QStringList({"sonnet", "medium"}));
    QCOMPARE(tuningOf("QA"), QStringList({"haiku", "low"}));
}

void TestSwarm::taskTuningOverridesStageDefaults() {
    SwarmFixture f;
    auto launchOf = [&f](const QString &id) { return sessionFor(f.runtime, id)->launch().agent; };

    // Tanpa pilihan pengguna: bawaan stage
    QVERIFY(f.run(QStringLiteral("plain"), QStringLiteral("SPECIFIER")));
    QCOMPARE(launchOf(QStringLiteral("plain")).model, QStringLiteral("sonnet"));
    QCOMPARE(launchOf(QStringLiteral("plain")).effort, QStringLiteral("medium"));

    // Model dan effort ditimpa; pilihan untuk stage lain tidak ikut terbawa
    TaskItem heavy = makeTask(QStringLiteral("heavy"), QStringLiteral("CODER"));
    heavy.tuning.insert(QStringLiteral("CODER"), {QStringLiteral("opus"), QStringLiteral("high")});
    heavy.tuning.insert(QStringLiteral("SPECIFIER"), {QStringLiteral("haiku"), QStringLiteral("low")});
    QVERIFY(f.swarm.run(heavy, f.dir.path()));
    QCOMPARE(launchOf(QStringLiteral("heavy")).model, QStringLiteral("opus"));
    QCOMPARE(launchOf(QStringLiteral("heavy")).effort, QStringLiteral("high"));

    // Field kosong dibiarkan: hanya effort yang ditimpa, model tetap bawaan
    TaskItem effortOnly = makeTask(QStringLiteral("effort"), QStringLiteral("SPECIFIER"));
    effortOnly.tuning.insert(QStringLiteral("SPECIFIER"), {QString(), QStringLiteral("max")});
    QVERIFY(f.swarm.run(effortOnly, f.dir.path()));
    QCOMPARE(launchOf(QStringLiteral("effort")).model, QStringLiteral("sonnet"));
    QCOMPARE(launchOf(QStringLiteral("effort")).effort, QStringLiteral("max"));

    // Bawaan katalog tidak ikut berubah, dan pilihan sampai ke argumen CLI
    QCOMPARE(f.catalog.profile(QStringLiteral("CODER"))->agent()->model, QStringLiteral("sonnet"));
    const QStringList args = ClaudeCli::arguments(launchOf(QStringLiteral("heavy")));
    QCOMPARE(argValue(args, QStringLiteral("--model")), QStringLiteral("opus"));
    QCOMPARE(argValue(args, QStringLiteral("--effort")), QStringLiteral("high"));

    // Form edit menyimpan pilihan lewat TaskManager tanpa menyentuh stage, status, atau riwayat run
    TaskManager manager(f.catalog);
    manager.addTask(makeTask(QStringLiteral("m1"), QStringLiteral("CODER")));
    TaskItem details = makeTask(QStringLiteral("m1"), QStringLiteral("QA"));
    details.title = QStringLiteral("Baru");
    details.category = QStringLiteral("bug");
    details.subtext = QStringLiteral("catatan");
    details.tuning = heavy.tuning;
    details.attachments = QStringList({"mockup.png", "data.xlsx"});
    details.state = TaskState::Failed;
    details.runs = {stageRun(QStringLiteral("QA"), false, QStringLiteral("x"))};
    QVERIFY(manager.updateDetails(details));
    const std::optional<TaskItem> updated = manager.task(QStringLiteral("m1"));
    QVERIFY(updated);
    QCOMPARE(updated->title, QStringLiteral("Baru"));
    QCOMPARE(updated->subtext, QStringLiteral("catatan"));
    QCOMPARE(updated->attachments, QStringList({"mockup.png", "data.xlsx"}));
    QCOMPARE(updated->stage, QStringLiteral("CODER"));
    QVERIFY(updated->state == TaskState::Idle);
    QVERIFY(updated->runs.isEmpty());
    QCOMPARE(updated->tuning.value(QStringLiteral("CODER")).model, QStringLiteral("opus"));
    QVERIFY(!manager.updateDetails(makeTask(QStringLiteral("tidak-ada"), QStringLiteral("CODER"))));
}

void TestSwarm::promptComposerSkipsEmptyFields() {
    TaskItem task = makeTask(QStringLiteral("1"), QStringLiteral("CODER"));
    task.title = QStringLiteral("  Buat hello.txt ");
    task.subtext.clear();
    QCOMPARE(TaskPromptComposer().compose(task),
             QStringLiteral("# Task\nJudul: Buat hello.txt\nKategori: utility\n"));

    task.subtext = QStringLiteral("PIC: Budi");
    QVERIFY(TaskPromptComposer().compose(task).endsWith(QStringLiteral("Catatan: PIC: Budi\n")));
}

void TestSwarm::workspaceGuardAllowsOneWriterPerFolder() {
    WorkspaceGuard guard;
    QVERIFY(guard.tryAcquire(QStringLiteral("C:/repo"), true));
    QVERIFY(!guard.tryAcquire(QStringLiteral("C:/repo/"), true));   // folder sama setelah dinormalisasi
#ifdef Q_OS_WIN
    QVERIFY(!guard.tryAcquire(QStringLiteral("c:/REPO"), true));    // Windows tidak peka huruf besar/kecil
#endif
    QVERIFY(guard.isLocked(QStringLiteral("C:/repo")));
    QVERIFY(guard.tryAcquire(QStringLiteral("C:/repo"), false));    // pembaca tidak ditahan
    QVERIFY(guard.tryAcquire(QStringLiteral("C:/lain"), true));

    guard.release(QStringLiteral("C:/repo"), true);
    QVERIFY(!guard.isLocked(QStringLiteral("C:/repo")));
    QVERIFY(guard.tryAcquire(QStringLiteral("C:/repo"), true));
}

void TestSwarm::swarmRespectsMaxConcurrent() {
    SwarmFixture f;
    for (int i = 1; i <= 4; ++i) {
        QVERIFY(f.run(QString::number(i), QStringLiteral("SPECIFIER")));
    }
    // SPECIFIER maksimal 3 agent sekaligus
    QCOMPARE(f.started, 3);
    QCOMPARE(f.queued, 1);
    QVERIFY(f.swarm.state(QStringLiteral("4")) == RunState::Queued);

    sessionFor(f.runtime, QStringLiteral("1"))->finishWith(successResult());
    QCOMPARE(f.finishedIds, QStringList({"1"}));
    QCOMPARE(f.started, 4);
    QVERIFY(f.swarm.state(QStringLiteral("4")) == RunState::Running);
    QVERIFY(f.swarm.state(QStringLiteral("1")) == RunState::Idle);
}

void TestSwarm::writersInSameFolderTakeTurns() {
    SwarmFixture f;
    QVERIFY(f.run(QStringLiteral("c1"), QStringLiteral("CODER")));
    QVERIFY(f.run(QStringLiteral("k1"), QStringLiteral("CLEANER")));
    QVERIFY(f.swarm.state(QStringLiteral("c1")) == RunState::Running);
    QVERIFY(f.swarm.state(QStringLiteral("k1")) == RunState::Queued);    // folder sedang ditulis CODER

    QVERIFY(f.run(QStringLiteral("s1"), QStringLiteral("SPECIFIER")));
    QVERIFY(f.swarm.state(QStringLiteral("s1")) == RunState::Running);   // pembaca tidak ditahan

    QTemporaryDir other;
    QVERIFY(f.swarm.run(makeTask(QStringLiteral("c2"), QStringLiteral("CODER")), other.path()));
    QVERIFY(f.swarm.state(QStringLiteral("c2")) == RunState::Running);   // folder lain boleh paralel

    sessionFor(f.runtime, QStringLiteral("c1"))->finishWith(successResult());
    QVERIFY(f.swarm.state(QStringLiteral("k1")) == RunState::Running);   // giliran pindah lintas stage
}

void TestSwarm::cancelRemovesQueuedAndStopsRunning() {
    SwarmFixture f;
    QVERIFY(f.run(QStringLiteral("k1"), QStringLiteral("CLEANER")));
    QVERIFY(f.run(QStringLiteral("k2"), QStringLiteral("CLEANER")));
    QVERIFY(f.swarm.state(QStringLiteral("k2")) == RunState::Queued);

    f.swarm.cancel(QStringLiteral("k2"));
    QVERIFY(f.swarm.state(QStringLiteral("k2")) == RunState::Idle);
    QCOMPARE(f.finishedIds, QStringList({"k2"}));
    QCOMPARE(f.results.last().outcome, QStringLiteral("cancelled"));
    QCOMPARE(int(f.runtime.sessions.size()), 1);   // run yang antre tidak pernah membuat session

    f.swarm.cancel(QStringLiteral("k1"));
    QVERIFY(f.swarm.state(QStringLiteral("k1")) == RunState::Idle);
    QCOMPARE(f.finishedIds, QStringList({"k2", "k1"}));
    QCOMPARE(f.results.last().outcome, QStringLiteral("cancelled"));
}

void TestSwarm::runRejectsInvalidRequests() {
    SwarmFixture f;
    QString reason;

    QVERIFY(!f.run(QStringLiteral("w"), QStringLiteral("WAITING"), QStringLiteral("P"), &reason));
    QVERIFY(reason.contains(QStringLiteral("WAITING")));

    QVERIFY(f.run(QStringLiteral("a"), QStringLiteral("SPECIFIER")));
    QVERIFY(!f.run(QStringLiteral("a"), QStringLiteral("SPECIFIER"), QStringLiteral("P"), &reason));
    QVERIFY(reason.contains(QStringLiteral("sudah berjalan")));

    QVERIFY(!f.swarm.run(makeTask(QStringLiteral("b"), QStringLiteral("SPECIFIER")),
                         f.dir.filePath(QStringLiteral("tidak-ada")), &reason));
    QVERIFY(reason.contains(QStringLiteral("folder kerja")));

    f.runtime.available = false;
    QVERIFY(!f.run(QStringLiteral("c"), QStringLiteral("SPECIFIER"), QStringLiteral("P"), &reason));
    QCOMPARE(reason, QStringLiteral("runtime palsu dimatikan"));

    QCOMPARE(f.started, 1);
}

void TestSwarm::cancelProjectOnlyTouchesThatProject() {
    SwarmFixture f;
    QVERIFY(f.run(QStringLiteral("a1"), QStringLiteral("SPECIFIER"), QStringLiteral("A")));
    QVERIFY(f.run(QStringLiteral("b1"), QStringLiteral("SPECIFIER"), QStringLiteral("B")));
    QVERIFY(f.run(QStringLiteral("a2"), QStringLiteral("CODER"), QStringLiteral("A")));
    QVERIFY(f.run(QStringLiteral("a3"), QStringLiteral("CLEANER"), QStringLiteral("A")));
    QVERIFY(f.swarm.state(QStringLiteral("a3")) == RunState::Queued);

    f.swarm.cancelProject(QStringLiteral("A"));
    QVERIFY(f.swarm.state(QStringLiteral("a1")) == RunState::Idle);
    QVERIFY(f.swarm.state(QStringLiteral("a2")) == RunState::Idle);
    QVERIFY(f.swarm.state(QStringLiteral("a3")) == RunState::Idle);
    QVERIFY(f.swarm.state(QStringLiteral("b1")) == RunState::Running);
    // a3 dibuang dari antrean sebelum a2 berhenti, jadi tidak sempat jalan
    QCOMPARE(f.started, 3);
    QCOMPARE(int(f.finishedIds.size()), 3);
}

void TestSwarm::sessionFinishingInsideStartIsSafe() {
    SwarmFixture f;
    f.runtime.finishOnStart = true;
    QVERIFY(f.run(QStringLiteral("x"), QStringLiteral("CODER")));
    QCOMPARE(f.started, 1);
    QCOMPARE(f.queued, 0);
    QCOMPARE(f.finishedIds, QStringList({"x"}));
    QVERIFY(f.swarm.state(QStringLiteral("x")) == RunState::Idle);

    // Kunci folder sudah dilepas: penulis berikutnya langsung jalan
    f.runtime.finishOnStart = false;
    QVERIFY(f.run(QStringLiteral("y"), QStringLiteral("CLEANER")));
    QVERIFY(f.swarm.state(QStringLiteral("y")) == RunState::Running);
}

void TestSwarm::logFormatterSummaries() {
    QCOMPARE(RunLogFormatter::formatDuration(5782), QStringLiteral("5.8s"));
    QCOMPARE(RunLogFormatter::formatDuration(72000), QStringLiteral("1m 12s"));
    QCOMPARE(RunLogFormatter::formatTokens(950), QStringLiteral("950"));
    QCOMPARE(RunLogFormatter::formatTokens(2910), QStringLiteral("2.9k"));
    QCOMPARE(RunLogFormatter::formatTokens(107000), QStringLiteral("107k"));
    QCOMPARE(RunLogFormatter::formatTokens(1200000), QStringLiteral("1.2M"));

    const TaskItem task = makeTask(QStringLiteral("1"), QStringLiteral("CODER"));
    AgentResult result = successResult();
    result.durationMs = 5782;
    result.totalTokens = 2910;
    result.costUsd = 0.029;
    result.sessionId = QStringLiteral("abc");
    result.deniedTools = QStringList({"Bash", "Bash"});
    QCOMPARE(RunLogFormatter::finishLine(task, result),
             QStringLiteral("[AGENT:CODER] ✓ selesai · 5.8s · 2.9k tok · $0.03 · sesi abc · 2 aksi ditolak (Bash)"));
    // Literal UTF-8 di source terbaca sebagai karakter yang benar, bukan hanya sama-sama salah
    QVERIFY(RunLogFormatter::finishLine(task, result).contains(QChar(0x2713)));   // ✓
    QCOMPARE(RunLogFormatter::finishLine(task, AgentResult::failure(QStringLiteral("timeout"),
                                                                    QStringLiteral("melewati batas 20 menit"))),
             QStringLiteral("[AGENT:CODER] ✗ timeout: melewati batas 20 menit"));

    AgentEvent tool;
    tool.kind = AgentEvent::Kind::ToolUse;
    tool.toolName = QStringLiteral("Write");
    tool.toolDetail = QStringLiteral("C:/work/demo/src/hello.txt");
    QCOMPARE(RunLogFormatter::eventLine(task, tool, QStringLiteral("C:/work/demo")),
             QStringLiteral("[AGENT:CODER] → Write src/hello.txt"));
#ifdef Q_OS_WIN
    tool.toolDetail = QStringLiteral("C:\\work\\demo\\src\\hello.txt");
    QCOMPARE(RunLogFormatter::eventLine(task, tool, QStringLiteral("C:/work/demo")),
             QStringLiteral("[AGENT:CODER] → Write src/hello.txt"));
#endif
    tool.toolDetail = QString(300, QLatin1Char('x'));
    QCOMPARE(int(RunLogFormatter::eventLine(task, tool, QString()).size()),
             int(QStringLiteral("[AGENT:CODER] → Write ").size()) + 100);
}

void TestSwarm::claudeSessionStreamsEvents() {
    const FakeClaudeMode mode(QStringLiteral("print:tool.jsonl"));
    QTemporaryDir dir;
    ClaudeCodeRuntime runtime(QCoreApplication::applicationFilePath());
    QVERIFY(runtime.isAvailable(nullptr));

    SessionRecorder recorder;
    std::unique_ptr<AgentSession> session(runtime.createSession(fakeLaunch(dir.path()), nullptr));
    recorder.attach(session.get());
    session->start();

    QTRY_COMPARE_WITH_TIMEOUT(int(recorder.results.size()), 1, 15000);
    // Event result ada di baris terakhir yang tidak diakhiri '\n'
    QVERIFY(recorder.results.first().success);
    QCOMPARE(recorder.toolNames(), QStringList({"Read", "Write", "Bash"}));
    QVERIFY(!session->isRunning());
}

void TestSwarm::claudeSessionSendsPromptThroughStdin() {
    QTemporaryDir dir;
    const QString echoPath = dir.filePath(QStringLiteral("echo.json"));
    const FakeClaudeMode mode(QStringLiteral("echo:") + echoPath);
    ClaudeCodeRuntime runtime(QCoreApplication::applicationFilePath());
    const AgentLaunch launch = fakeLaunch(dir.path());

    SessionRecorder recorder;
    std::unique_ptr<AgentSession> session(runtime.createSession(launch, nullptr));
    recorder.attach(session.get());
    session->start();
    QTRY_COMPARE_WITH_TIMEOUT(int(recorder.results.size()), 1, 15000);

    QFile file(echoPath);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonObject echo = QJsonDocument::fromJson(file.readAll()).object();
    QStringList args;
    const QJsonArray argArray = echo.value(QStringLiteral("args")).toArray();
    for (const QJsonValue &value : argArray) {
        args.append(value.toString());
    }

    // Prompt (UTF-8, kutip, &, <>) sampai utuh lewat stdin, tidak lewat argumen
    QCOMPARE(echo.value(QStringLiteral("stdin")).toString(), launch.prompt);
    QVERIFY(!args.join(QLatin1Char(' ')).contains(QStringLiteral("Judul")));
    // Argumen, termasuk instruksi peran multi-baris dan "Bash(git *)", sampai tanpa berubah
    QCOMPARE(args, ClaudeCli::arguments(launch.agent));
}

void TestSwarm::claudeSessionCancelStopsProcess() {
    const FakeClaudeMode mode(QStringLiteral("hang"));
    QTemporaryDir dir;
    ClaudeCodeRuntime runtime(QCoreApplication::applicationFilePath());

    SessionRecorder recorder;
    std::unique_ptr<AgentSession> session(runtime.createSession(fakeLaunch(dir.path()), nullptr));
    recorder.attach(session.get());
    session->start();
    QVERIFY(session->isRunning());
    QTest::qWait(300);

    QElapsedTimer timer;
    timer.start();
    session->cancel();
    session->cancel();   // idempoten
    QTRY_COMPARE_WITH_TIMEOUT(int(recorder.results.size()), 1, 5000);
    QVERIFY(timer.elapsed() < 2000);
    QCOMPARE(recorder.results.first().outcome, QStringLiteral("cancelled"));

    QTest::qWait(200);
    QCOMPARE(int(recorder.results.size()), 1);
}

void TestSwarm::claudeSessionTimesOut() {
    const FakeClaudeMode mode(QStringLiteral("hang"));
    QTemporaryDir dir;
    ClaudeCodeRuntime runtime(QCoreApplication::applicationFilePath());

    SessionRecorder recorder;
    std::unique_ptr<AgentSession> session(runtime.createSession(fakeLaunch(dir.path(), 500), nullptr));
    recorder.attach(session.get());
    session->start();

    QTRY_COMPARE_WITH_TIMEOUT(int(recorder.results.size()), 1, 5000);
    QCOMPARE(recorder.results.first().outcome, QStringLiteral("timeout"));
    QVERIFY(!recorder.results.first().success);
}

void TestSwarm::claudeSessionReportsStderrWithoutResult() {
    const FakeClaudeMode mode(QStringLiteral("fail"));
    QTemporaryDir dir;
    ClaudeCodeRuntime runtime(QCoreApplication::applicationFilePath());

    SessionRecorder recorder;
    std::unique_ptr<AgentSession> session(runtime.createSession(fakeLaunch(dir.path()), nullptr));
    recorder.attach(session.get());
    session->start();

    QTRY_COMPARE_WITH_TIMEOUT(int(recorder.results.size()), 1, 15000);
    QCOMPARE(recorder.results.first().outcome, QStringLiteral("no_result"));
    QCOMPARE(recorder.results.first().message, QStringLiteral("Error: belum login"));
    QVERIFY(!recorder.events.isEmpty());
    QVERIFY(recorder.events.last().kind == AgentEvent::Kind::Stderr);
}

void TestSwarm::claudeSessionWithoutProgramFinishesOnce() {
    QTemporaryDir dir;
    {
        ClaudeCodeRuntime runtime{QString()};
        QString reason;
        QVERIFY(!runtime.isAvailable(&reason));
        QVERIFY(!reason.isEmpty());

        SessionRecorder recorder;
        std::unique_ptr<AgentSession> session(runtime.createSession(fakeLaunch(dir.path()), nullptr));
        recorder.attach(session.get());
        session->start();
        QCOMPARE(int(recorder.results.size()), 1);   // langsung di dalam start()
        QCOMPARE(recorder.results.first().outcome, QStringLiteral("failed_to_start"));
    }
    {
        ClaudeCodeRuntime runtime(dir.filePath(QStringLiteral("tidak-ada.exe")));
        SessionRecorder recorder;
        std::unique_ptr<AgentSession> session(runtime.createSession(fakeLaunch(dir.path()), nullptr));
        recorder.attach(session.get());
        session->start();
        QTRY_COMPARE_WITH_TIMEOUT(int(recorder.results.size()), 1, 5000);
        QCOMPARE(recorder.results.first().outcome, QStringLiteral("failed_to_start"));
        QTest::qWait(200);
        QCOMPARE(int(recorder.results.size()), 1);
    }
}

void TestSwarm::realClaudeRunsCoderTask() {
    if (qEnvironmentVariableIsEmpty("LASSOMOIR_REAL_CLAUDE")) {
        QSKIP("Set LASSOMOIR_REAL_CLAUDE=1 untuk menjalankan claude sungguhan (memakai token)");
    }

    QTemporaryDir dir;
    QProcess git;
    git.setWorkingDirectory(dir.path());
    git.start(QStringLiteral("git"), {QStringLiteral("init"), QStringLiteral("-q")});
    QVERIFY(git.waitForFinished(15000));

    const StageCatalog catalog = StageCatalog::standard();
    ClaudeCodeRuntime runtime;
    QString reason;
    QVERIFY2(runtime.isAvailable(&reason), qPrintable(reason));
    TaskPromptComposer composer;
    SwarmCoordinator swarm(catalog, runtime, composer);

    QList<AgentResult> results;
    connect(&swarm, &SwarmCoordinator::runStarted, this, [](const TaskItem &task, const AgentLaunch &launch) {
        for (const QString &line : RunLogFormatter::startLines(task, launch)) {
            qInfo().noquote() << line;
        }
    });
    connect(&swarm, &SwarmCoordinator::runEvent, this, [&dir](const TaskItem &task, const AgentEvent &event) {
        qInfo().noquote() << RunLogFormatter::eventLine(task, event, dir.path());
    });
    connect(&swarm, &SwarmCoordinator::runFinished, this, [&results](const TaskItem &task, const AgentResult &result) {
        qInfo().noquote() << RunLogFormatter::finishLine(task, result);
        results.append(result);
    });

    TaskItem task = makeTask(QStringLiteral("real"), QStringLiteral("CODER"));
    task.title = QStringLiteral("Buat file hello.txt berisi teks 'halo dari agent', lalu jalankan git add hello.txt");
    QVERIFY2(swarm.run(task, dir.path(), &reason), qPrintable(reason));
    QTRY_COMPARE_WITH_TIMEOUT(int(results.size()), 1, 300000);

    const AgentResult result = results.first();
    QVERIFY2(result.success, qPrintable(result.outcome + QStringLiteral(": ") + result.message));
    QVERIFY(QFile::exists(dir.filePath(QStringLiteral("hello.txt"))));
    // "Bash(git *)" di --allowedTools terbaca CLI: git add tidak ditolak
    QVERIFY2(result.deniedTools.isEmpty(), qPrintable(result.deniedTools.join(QStringLiteral(", "))));

    git.start(QStringLiteral("git"), {QStringLiteral("diff"), QStringLiteral("--cached"), QStringLiteral("--name-only")});
    QVERIFY(git.waitForFinished(15000));
    QVERIFY(QString::fromUtf8(git.readAllStandardOutput()).contains(QStringLiteral("hello.txt")));
}

void TestSwarm::taskManagerAdvancesAndGates() {
    const StageCatalog catalog = StageCatalog::standard();
    TaskManager tasks(catalog);
    QStringList moves;
    connect(&tasks, &TaskManager::taskMoved, &tasks, [&moves](const TaskItem &task, const QString &from) {
        moves.append(QStringLiteral("%1:%2->%3").arg(task.id, from, task.stage));
    });

    // Sukses tanpa gate: maju sendiri ke stage berikutnya (run tetap manual)
    tasks.addTask(makeTask(QStringLiteral("c"), QStringLiteral("CODER")));
    tasks.recordRun(QStringLiteral("c"), stageRun(QStringLiteral("CODER"), true, QStringLiteral("Selesai")));
    QCOMPARE(tasks.task(QStringLiteral("c"))->stage, QStringLiteral("CLEANER"));
    QVERIFY(tasks.task(QStringLiteral("c"))->state == TaskState::Idle);
    QCOMPARE(moves, QStringList({"c:CODER->CLEANER"}));

    // Sukses di stage ber-gate: berhenti untuk review
    tasks.addTask(makeTask(QStringLiteral("s"), QStringLiteral("SPECIFIER")));
    tasks.recordRun(QStringLiteral("s"), stageRun(QStringLiteral("SPECIFIER"), true, QStringLiteral("# Spesifikasi")));
    QCOMPARE(tasks.task(QStringLiteral("s"))->stage, QStringLiteral("SPECIFIER"));
    QVERIFY(tasks.task(QStringLiteral("s"))->state == TaskState::AwaitingReview);

    // Gagal dan dibatalkan tetap di stage yang sama
    tasks.addTask(makeTask(QStringLiteral("f"), QStringLiteral("CLEANER")));
    tasks.recordRun(QStringLiteral("f"), stageRun(QStringLiteral("CLEANER"), false, QStringLiteral("melewati batas"),
                                                  QStringLiteral("timeout")));
    QVERIFY(tasks.task(QStringLiteral("f"))->state == TaskState::Failed);
    QCOMPARE(tasks.task(QStringLiteral("f"))->stage, QStringLiteral("CLEANER"));

    tasks.addTask(makeTask(QStringLiteral("x"), QStringLiteral("HARDENER")));
    tasks.recordRun(QStringLiteral("x"), stageRun(QStringLiteral("HARDENER"), false, QStringLiteral("dibatalkan"),
                                                  QStringLiteral("cancelled")));
    QVERIFY(tasks.task(QStringLiteral("x"))->state == TaskState::Idle);
    QCOMPARE(int(tasks.task(QStringLiteral("x"))->runs.size()), 1);

    QCOMPARE(tasks.nextStage(QStringLiteral("QA")), QStringLiteral("DONE"));
    QVERIFY(tasks.nextStage(QStringLiteral("DONE")).isEmpty());
}

void TestSwarm::taskManagerReviewDecisions() {
    const StageCatalog catalog = StageCatalog::standard();
    TaskManager tasks(catalog);
    QString reason;

    tasks.addTask(makeTask(QStringLiteral("s"), QStringLiteral("SPECIFIER")));
    tasks.recordRun(QStringLiteral("s"), stageRun(QStringLiteral("SPECIFIER"), true, QStringLiteral("Spek v1")));

    // Revisi wajib pakai catatan
    QVERIFY(!tasks.requestRevision(QStringLiteral("s"), QStringLiteral("  "), &reason));
    QVERIFY(reason.contains(QStringLiteral("catatan")));
    QVERIFY(tasks.requestRevision(QStringLiteral("s"), QStringLiteral("Kalkulator jangan dihapus"), &reason));
    TaskItem task = *tasks.task(QStringLiteral("s"));
    QVERIFY(task.state == TaskState::Idle);
    QCOMPARE(task.stage, QStringLiteral("SPECIFIER"));
    QCOMPARE(task.runs.last().decision, ReviewDecision::Revise);
    QCOMPARE(task.runs.last().reviewNote, QStringLiteral("Kalkulator jangan dihapus"));

    // Tidak sedang review: keputusan ditolak
    QVERIFY(!tasks.approve(QStringLiteral("s"), QString(), &reason));

    tasks.recordRun(QStringLiteral("s"), stageRun(QStringLiteral("SPECIFIER"), true, QStringLiteral("Spek v2")));
    QVERIFY(tasks.approve(QStringLiteral("s"), QStringLiteral("Newsletter tetap"), &reason));
    task = *tasks.task(QStringLiteral("s"));
    QCOMPARE(task.stage, QStringLiteral("CODER"));
    QVERIFY(task.state == TaskState::Idle);
    QCOMPARE(task.approvedGates(), 1);
    QCOMPARE(task.runs.last().decision, ReviewDecision::Approved);

    // ARCHITECT menunggu persetujuan; run sukses tidak langsung maju ke HARDENER
    tasks.addTask(makeTask(QStringLiteral("a"), QStringLiteral("ARCHITECT")));
    tasks.recordRun(QStringLiteral("a"), stageRun(QStringLiteral("ARCHITECT"), true, QStringLiteral("Temuan")));
    QCOMPARE(tasks.task(QStringLiteral("a"))->stage, QStringLiteral("ARCHITECT"));
    QVERIFY(tasks.task(QStringLiteral("a"))->state == TaskState::AwaitingReview);
    QVERIFY(!tasks.moveTask(QStringLiteral("a"), QStringLiteral("HARDENER"), &reason));
    QVERIFY(tasks.approve(QStringLiteral("a"), QString(), &reason));
    QCOMPARE(tasks.task(QStringLiteral("a"))->stage, QStringLiteral("HARDENER"));

    // QA dikembalikan ke CODER dengan catatan
    tasks.addTask(makeTask(QStringLiteral("q"), QStringLiteral("QA")));
    tasks.recordRun(QStringLiteral("q"), stageRun(QStringLiteral("QA"), true, QStringLiteral("Kriteria 3 gagal")));
    QCOMPARE(tasks.sendBackTargets(QStringLiteral("QA")),
             QStringList({"SPECIFIER", "CODER", "CLEANER", "ARCHITECT", "HARDENER"}));
    QVERIFY(tasks.sendBackTargets(QStringLiteral("SPECIFIER")).isEmpty());
    QVERIFY(!tasks.sendBack(QStringLiteral("q"), QStringLiteral("DONE"), QStringLiteral("x"), &reason));
    QVERIFY(!tasks.sendBack(QStringLiteral("q"), QStringLiteral("CODER"), QString(), &reason));
    QVERIFY(tasks.sendBack(QStringLiteral("q"), QStringLiteral("CODER"), QStringLiteral("Perbaiki anchor footer"), &reason));
    task = *tasks.task(QStringLiteral("q"));
    QCOMPARE(task.stage, QStringLiteral("CODER"));
    QVERIFY(task.state == TaskState::Idle);
    QCOMPARE(task.runs.last().decision, ReviewDecision::SentBack);
}

void TestSwarm::manualMoveRespectsGate() {
    const StageCatalog catalog = StageCatalog::standard();
    TaskManager tasks(catalog);
    QStringList rejected;
    connect(&tasks, &TaskManager::taskMoveRejected, &tasks,
            [&rejected](const QString &, const QString &, const QString &reason) { rejected.append(reason); });
    QString reason;

    tasks.addTask(makeTask(QStringLiteral("s"), QStringLiteral("SPECIFIER")));
    QVERIFY(!tasks.moveTask(QStringLiteral("s"), QStringLiteral("CODER"), &reason));   // maju melewati gate
    QVERIFY(reason.contains(QStringLiteral("SPECIFIER")));
    QCOMPARE(int(rejected.size()), 1);
    QVERIFY(tasks.moveTask(QStringLiteral("s"), QStringLiteral("WAITING"), &reason));  // mundur selalu boleh
    QVERIFY(tasks.moveTask(QStringLiteral("s"), QStringLiteral("SPECIFIER"), &reason)); // WAITING tanpa gate

    tasks.recordRun(QStringLiteral("s"), stageRun(QStringLiteral("SPECIFIER"), true, QStringLiteral("spek")));
    QVERIFY(!tasks.moveTask(QStringLiteral("s"), QStringLiteral("WAITING"), &reason));  // menunggu review: lewat drawer
    QVERIFY(reason.contains(QStringLiteral("review")));

    QVERIFY(tasks.approve(QStringLiteral("s"), QString()));
    QVERIFY(tasks.moveTask(QStringLiteral("s"), QStringLiteral("SPECIFIER"), &reason));  // CODER -> SPECIFIER
    QVERIFY(tasks.moveTask(QStringLiteral("s"), QStringLiteral("QA"), &reason));         // gate SPECIFIER sudah lolos
    QVERIFY(!tasks.moveTask(QStringLiteral("s"), QStringLiteral("DONE"), &reason));      // gate QA belum
}

void TestSwarm::composerBuildsHandoffPrompt() {
    const TaskPromptComposer composer;
    TaskItem task = makeTask(QStringLiteral("1"), QStringLiteral("SPECIFIER"));

    // Putaran revisi SPECIFIER
    StageRun spec1 = stageRun(QStringLiteral("SPECIFIER"), true, QStringLiteral("Spek v1"));
    spec1.decision = ReviewDecision::Revise;
    spec1.reviewNote = QStringLiteral("Kalkulator jangan dihapus");
    task.runs = {spec1};
    QString prompt = composer.compose(task);
    QVERIFY(prompt.startsWith(QStringLiteral("# Task\nJudul: Task 1\n")));
    QVERIFY(prompt.contains(QStringLiteral("# Dokumen sebelumnya (untuk direvisi)\n\nSpek v1")));
    QVERIFY(prompt.contains(QStringLiteral("Catatan revisi:\nKalkulator jangan dihapus")));
    QVERIFY(!prompt.contains(QStringLiteral("Spesifikasi yang disetujui")));

    // CODER setelah spesifikasi disetujui
    StageRun spec2 = stageRun(QStringLiteral("SPECIFIER"), true, QStringLiteral("Spek v2"));
    spec2.decision = ReviewDecision::Approved;
    spec2.reviewNote = QStringLiteral("Newsletter tetap");
    task.runs = {spec1, spec2};
    task.stage = QStringLiteral("CODER");
    prompt = composer.compose(task);
    QVERIFY(prompt.contains(QStringLiteral("# Spesifikasi yang disetujui (SPECIFIER)\n\nSpek v2")));
    QVERIFY(prompt.contains(QStringLiteral("Catatan saat disetujui:\nNewsletter tetap")));
    QVERIFY(!prompt.contains(QStringLiteral("Spek v1")));
    QVERIFY(!prompt.contains(QStringLiteral("Hasil stage sebelumnya")));

    // CLEANER melihat hasil CODER
    const StageRun coder = stageRun(QStringLiteral("CODER"), true, QStringLiteral("Ubah welcome_message.php"));
    task.runs = {spec1, spec2, coder};
    task.stage = QStringLiteral("CLEANER");
    prompt = composer.compose(task);
    QVERIFY(prompt.contains(QStringLiteral("# Spesifikasi yang disetujui (SPECIFIER)")));
    QVERIFY(prompt.contains(QStringLiteral("# Hasil stage sebelumnya (CODER)\n\nUbah welcome_message.php")));

    // HARDENER setelah review ARCHITECT disetujui: spesifikasi tetap dibawa bersama hasil review
    StageRun architect = stageRun(QStringLiteral("ARCHITECT"), true, QStringLiteral("Temuan: pisahkan repository"));
    architect.decision = ReviewDecision::Approved;
    architect.reviewNote = QStringLiteral("Lanjut");
    task.runs = {spec1, spec2, coder, architect};
    task.stage = QStringLiteral("HARDENER");
    prompt = composer.compose(task);
    QVERIFY(prompt.contains(QStringLiteral("# Spesifikasi yang disetujui (SPECIFIER)\n\nSpek v2")));
    QVERIFY(prompt.contains(QStringLiteral("# Hasil review yang disetujui (ARCHITECT)\n\nTemuan: pisahkan repository")));
    QVERIFY(!prompt.contains(QStringLiteral("Hasil stage sebelumnya")));

    // CODER setelah dikembalikan QA
    StageRun qa = stageRun(QStringLiteral("QA"), true, QStringLiteral("Kriteria 3 gagal"));
    qa.decision = ReviewDecision::SentBack;
    qa.reviewNote = QStringLiteral("Perbaiki anchor footer");
    task.runs = {spec1, spec2, coder, qa};
    task.stage = QStringLiteral("CODER");
    prompt = composer.compose(task);
    QVERIFY(prompt.contains(QStringLiteral("# Spesifikasi yang disetujui (SPECIFIER)")));
    QVERIFY(prompt.contains(QStringLiteral("# Hasil stage sebelumnya (QA)\n\nKriteria 3 gagal")));
    QVERIFY(prompt.contains(QStringLiteral("Catatan saat dikembalikan:\nPerbaiki anchor footer")));
}

void TestSwarm::fileManagerRoundTripsRunsAndState() {
    // Test mode QStandardPaths: tidak menyentuh AppData asli
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QVERIFY2(base.contains(QStringLiteral("qttest"), Qt::CaseInsensitive), qPrintable(base));

    TaskItem task = makeTask(QStringLiteral("r1"), QStringLiteral("SPECIFIER"), QStringLiteral("RoundTrip"));
    task.state = TaskState::AwaitingReview;
    StageRun run = stageRun(QStringLiteral("SPECIFIER"), true, QStringLiteral("# Judul\n\n| a | b |\n|---|---|"));
    run.result.sessionId = QStringLiteral("sesi-1");
    run.result.durationMs = 84000;
    run.result.totalTokens = 302000;
    run.result.costUsd = 0.87;
    run.result.deniedTools = QStringList({"Bash"});
    run.decision = ReviewDecision::Revise;
    run.reviewNote = QStringLiteral("catatan");
    task.runs = {run};
    task.tuning.insert(QStringLiteral("CODER"), {QStringLiteral("opus"), QStringLiteral("high")});
    task.tuning.insert(QStringLiteral("SPECIFIER"), {QString(), QStringLiteral("low")});
    task.tuning.insert(QStringLiteral("QA"), AgentTuning());   // entri kosong tidak ditulis
    task.attachments = QStringList({"mockup.png", "Data Penjualan.xlsx"});

    QString error;
    {
        FileManager writer;
        writer.setWorkingDirectory(QStringLiteral("RoundTrip"), QStringLiteral("C:/repo"));
        writer.setReferenceDirectories(QStringLiteral("RoundTrip"), {QStringLiteral("D:/lib"), QStringLiteral("E:/docs")});
        QVERIFY2(writer.saveTasks(QStringLiteral("RoundTrip"), {task}, &error), qPrintable(error));
    }

    FileManager reader;
    const QList<TaskItem> loaded = reader.loadTasks(QStringLiteral("RoundTrip"), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(int(loaded.size()), 1);
    const TaskItem &back = loaded.first();
    QVERIFY(back.state == TaskState::AwaitingReview);
    QCOMPARE(int(back.runs.size()), 1);
    const StageRun &restored = back.runs.first();
    QCOMPARE(restored.stage, QStringLiteral("SPECIFIER"));
    QVERIFY(restored.result.success);
    QCOMPARE(restored.result.message, run.result.message);
    QCOMPARE(restored.result.sessionId, QStringLiteral("sesi-1"));
    QCOMPARE(restored.result.durationMs, qint64(84000));
    QCOMPARE(restored.result.totalTokens, qint64(302000));
    QCOMPARE(restored.result.costUsd, 0.87);
    QCOMPARE(restored.result.deniedTools, QStringList({"Bash"}));
    QCOMPARE(restored.decision, ReviewDecision::Revise);
    QCOMPARE(restored.reviewNote, QStringLiteral("catatan"));
    QCOMPARE(restored.finishedAt.toSecsSinceEpoch(), run.finishedAt.toSecsSinceEpoch());
    QCOMPARE(reader.workingDirectory(QStringLiteral("RoundTrip")), QStringLiteral("C:/repo"));
    QCOMPARE(int(back.tuning.size()), 2);
    QCOMPARE(back.tuning.value(QStringLiteral("CODER")).model, QStringLiteral("opus"));
    QCOMPARE(back.tuning.value(QStringLiteral("CODER")).effort, QStringLiteral("high"));
    QVERIFY(back.tuning.value(QStringLiteral("SPECIFIER")).model.isEmpty());
    QCOMPARE(back.tuning.value(QStringLiteral("SPECIFIER")).effort, QStringLiteral("low"));
    QCOMPARE(back.attachments, QStringList({"mockup.png", "Data Penjualan.xlsx"}));
    QCOMPARE(reader.referenceDirectories(QStringLiteral("RoundTrip")), QStringList({"D:/lib", "E:/docs"}));

    // Folder lampiran per task di dalam folder project, jadi ikut terhapus bersama project
    const QString attachmentDir = reader.attachmentDirectory(QStringLiteral("RoundTrip"), QStringLiteral("r1"));
    QCOMPARE(attachmentDir, base + QStringLiteral("/projects/RoundTrip/attachments/r1"));
    QVERIFY(reader.attachmentDirectory(QStringLiteral("RoundTrip"), QString()).isEmpty());
    QVERIFY(writeFile(attachmentDir + QStringLiteral("/mockup.png"), pngBytes(QSize(2, 2), Qt::red)));
    {
        FileManager remover;
        QVERIFY(remover.saveTasks(QStringLiteral("Hapus"), {}, &error));
        QVERIFY(writeFile(remover.attachmentDirectory(QStringLiteral("Hapus"), QStringLiteral("x")) + QStringLiteral("/a.csv"), "a"));
        QVERIFY(remover.deleteProject(QStringLiteral("Hapus"), &error));
        QVERIFY(!QFileInfo::exists(base + QStringLiteral("/projects/Hapus")));
    }

    // Tanpa folder referensi dan lampiran: field-nya tidak ditulis
    {
        FileManager writer;
        writer.setReferenceDirectories(QStringLiteral("Polos"), {});
        QVERIFY(writer.saveTasks(QStringLiteral("Polos"), {makeTask(QStringLiteral("p1"), QStringLiteral("CODER"))}, &error));
        QFile plain(writer.projectFilePath(QStringLiteral("Polos")));
        QVERIFY(plain.open(QIODevice::ReadOnly));
        const QByteArray json = plain.readAll();
        QVERIFY(!json.contains("referenceDirectories"));
        QVERIFY(!json.contains("attachments"));
    }

    // File lama tanpa "state"/"runs" (dan dengan "badge") tetap terbaca
    QVERIFY(QDir().mkpath(base + QStringLiteral("/projects/Legacy")));
    QFile legacy(base + QStringLiteral("/projects/Legacy/session.json"));
    QVERIFY(legacy.open(QIODevice::WriteOnly));
    legacy.write(R"({"schemaVersion":1,"projectId":"Legacy","tasks":[{"id":"old","projectId":"Legacy",)"
                 R"("stage":"CODER","category":"x","title":"Lama","subtext":"","badge":"2"}]})");
    legacy.close();
    const QList<TaskItem> old = reader.loadTasks(QStringLiteral("Legacy"), &error);
    QCOMPARE(int(old.size()), 1);
    QVERIFY(old.first().state == TaskState::Idle);
    QVERIFY(old.first().runs.isEmpty());
    QVERIFY(old.first().tuning.isEmpty());
    QVERIFY(old.first().attachments.isEmpty());
    QVERIFY(reader.referenceDirectories(QStringLiteral("Legacy")).isEmpty());

    QDir(base + QStringLiteral("/projects")).removeRecursively();
}

void TestSwarm::mermaidHelpers() {
    const QString code = QStringLiteral("graph TD; A-->B");
    QCOMPARE(MermaidRenderer::keyFor(code), MermaidRenderer::keyFor(QStringLiteral("  graph TD; A-->B\n")));
    QVERIFY(MermaidRenderer::keyFor(code) != MermaidRenderer::keyFor(QStringLiteral("graph TD; A-->C")));
    QCOMPARE(int(MermaidRenderer::keyFor(code).size()), 40);

    // Tepi transparan dipotong, sisakan margin 4 piksel
    QImage canvas(200, 100, QImage::Format_ARGB32);
    canvas.fill(Qt::transparent);
    for (int y = 20; y < 60; ++y) {
        for (int x = 50; x < 80; ++x) {
            canvas.setPixelColor(x, y, Qt::red);
        }
    }
    QCOMPARE(EdgeMermaidRenderer::trimTransparent(canvas, 4).size(), QSize(30 + 8, 40 + 8));
    QImage empty(10, 10, QImage::Format_ARGB32);
    empty.fill(Qt::transparent);
    QVERIFY(EdgeMermaidRenderer::trimTransparent(empty).isNull());

    // Kode diagram tidak bisa menyisipkan tag, dan "%2" di dalamnya tidak diganti konfigurasi
    const QString page = EdgeMermaidRenderer::pageHtml(QStringLiteral("graph TD\n A-->B</pre><script>alert(1)</script>"));
    QVERIFY(!page.contains(QStringLiteral("<script>alert(1)")));
    QVERIFY(page.contains(QStringLiteral("&lt;/pre&gt;&lt;script&gt;")));
    QVERIFY(page.contains(QStringLiteral("securityLevel: 'strict'")));
    QVERIFY(EdgeMermaidRenderer::pageHtml(QStringLiteral("graph TD; A[%2]-->B")).contains(QStringLiteral("A[%2]")));
}

void TestSwarm::gitDiffParsesUnifiedDiff() {
    const QString patch = QStringList{
        QStringLiteral("diff --git a/src/board.cpp b/src/board.cpp"),
        QStringLiteral("index 1111111..2222222 100644"),
        QStringLiteral("--- a/src/board.cpp"),
        QStringLiteral("+++ b/src/board.cpp"),
        QStringLiteral("@@ -10,4 +10,5 @@ void Board::reset()"),
        QStringLiteral(" int x = 0;"),
        // Baris "-- komentar" yang dihapus: di dalam hunk bukan header "--- a/..."
        QStringLiteral("--- dulu komentar"),
        QStringLiteral("+-- sekarang komentar"),
        QStringLiteral("+int y = 2;"),
        // Baris konteks kosong tanpa spasi (diff.suppressBlankEmpty)
        QString(),
        QStringLiteral(" return x;"),
        QStringLiteral("diff --git a/docs/catatan baru.md b/docs/catatan baru.md"),
        QStringLiteral("new file mode 100644"),
        QStringLiteral("index 0000000..3333333"),
        QStringLiteral("--- /dev/null"),
        QStringLiteral("+++ b/docs/catatan baru.md\t"),
        QStringLiteral("@@ -0,0 +1,2 @@"),
        QStringLiteral("+# Catatan"),
        QStringLiteral("+baris dua"),
        QStringLiteral("\\ No newline at end of file"),
        QStringLiteral("diff --git a/old.txt b/old.txt"),
        QStringLiteral("deleted file mode 100644"),
        QStringLiteral("index 4444444..0000000"),
        QStringLiteral("--- a/old.txt"),
        QStringLiteral("+++ /dev/null"),
        QStringLiteral("@@ -1 +0,0 @@"),
        QStringLiteral("-hapus saya"),
        QStringLiteral("diff --git a/a.h b/b.h"),
        QStringLiteral("similarity index 90%"),
        QStringLiteral("rename from a.h"),
        QStringLiteral("rename to b.h"),
        QStringLiteral("index 5555555..6666666 100644"),
        QStringLiteral("--- a/a.h"),
        QStringLiteral("+++ b/b.h"),
        QStringLiteral("@@ -1,2 +1,2 @@"),
        QStringLiteral("-#ifndef A_H"),
        QStringLiteral("+#ifndef B_H"),
        QStringLiteral(" #define X"),
        QStringLiteral("diff --git a/logo.png b/logo.png"),
        QStringLiteral("index 7777777..8888888 100644"),
        QStringLiteral("Binary files a/logo.png and b/logo.png differ"),
        QStringLiteral("diff --git a/run.sh b/run.sh"),
        QStringLiteral("old mode 100644"),
        QStringLiteral("new mode 100755"),
        QString(),
    }.join(QLatin1Char('\n'));

    // Keluaran git dengan CRLF (mis. lewat pipe di Windows) harus terbaca sama persis
    for (const QString &text : {patch, QString(patch).replace(QStringLiteral("\n"), QStringLiteral("\r\n"))}) {
        const QList<FileDiff> files = GitDiff::parse(text);
        QCOMPARE(int(files.size()), 6);

        const FileDiff &board = files.at(0);
        QCOMPARE(board.path, QStringLiteral("src/board.cpp"));
        QVERIFY(board.status == FileDiff::Status::Modified);
        QCOMPARE(board.added, 2);
        QCOMPARE(board.removed, 1);
        QCOMPARE(int(board.lines.size()), 7);
        QVERIFY(board.lines.at(0).kind == DiffLine::Kind::Hunk);
        QCOMPARE(board.lines.at(0).text, QStringLiteral("@@ -10,4 +10,5 @@ void Board::reset()"));
        QVERIFY(board.lines.at(2).kind == DiffLine::Kind::Removed);
        QCOMPARE(board.lines.at(2).text, QStringLiteral("-- dulu komentar"));
        QCOMPARE(board.lines.at(2).oldLine, 11);
        QCOMPARE(board.lines.at(2).newLine, 0);
        QVERIFY(board.lines.at(4).kind == DiffLine::Kind::Added);
        QCOMPARE(board.lines.at(4).newLine, 12);
        QVERIFY(board.lines.at(5).kind == DiffLine::Kind::Context);
        QVERIFY(board.lines.at(5).text.isEmpty());
        QCOMPARE(board.lines.at(6).text, QStringLiteral("return x;"));
        QCOMPARE(board.lines.at(6).oldLine, 13);
        QCOMPARE(board.lines.at(6).newLine, 14);

        const FileDiff &notes = files.at(1);
        QCOMPARE(notes.path, QStringLiteral("docs/catatan baru.md"));
        QVERIFY(notes.status == FileDiff::Status::Added);
        QVERIFY(!notes.untracked);
        QCOMPARE(notes.added, 2);
        QVERIFY(notes.lines.last().kind == DiffLine::Kind::Note);
        QCOMPARE(notes.lines.last().text, QStringLiteral("No newline at end of file"));

        QCOMPARE(files.at(2).path, QStringLiteral("old.txt"));
        QVERIFY(files.at(2).status == FileDiff::Status::Deleted);
        QCOMPARE(files.at(2).removed, 1);
        QCOMPARE(files.at(2).lines.last().oldLine, 1);

        QVERIFY(files.at(3).status == FileDiff::Status::Renamed);
        QCOMPARE(files.at(3).oldPath, QStringLiteral("a.h"));
        QCOMPARE(files.at(3).path, QStringLiteral("b.h"));
        QCOMPARE(files.at(3).added, 1);
        QCOMPARE(files.at(3).removed, 1);

        QCOMPARE(files.at(4).path, QStringLiteral("logo.png"));
        QVERIFY(files.at(4).note.contains(QStringLiteral("biner")));
        QVERIFY(files.at(4).lines.isEmpty());

        QCOMPARE(files.at(5).path, QStringLiteral("run.sh"));
        QCOMPARE(files.at(5).note, QStringLiteral("Mode file berubah (100644 → 100755)"));
    }

    QVERIFY(GitDiff::parse(QString()).isEmpty());
}

void TestSwarm::gitDiffCollectsWorkspaceChanges() {
    if (QStandardPaths::findExecutable(QStringLiteral("git")).isEmpty()) {
        QSKIP("git tidak ada di PATH");
    }

    QTemporaryDir repo;
    const QDir dir(repo.path());
    QVERIFY(runGit(dir.path(), {QStringLiteral("init"), QStringLiteral("-q")}));
    QVERIFY(dir.mkpath(QStringLiteral("sub")));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("keep.txt")), "satu\ndua\ntiga\n"));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("gone.txt")), "hapus\n"));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("sub/inner.txt")), "lama\n"));
    QVERIFY(writeFile(dir.filePath(QStringLiteral(".gitignore")), "build/\n"));
    QVERIFY(runGit(dir.path(), {QStringLiteral("add"), QStringLiteral("-A")}));
    QVERIFY(runGit(dir.path(), {QStringLiteral("commit"), QStringLiteral("-q"), QStringLiteral("-m"), QStringLiteral("awal")}));

    // Perubahan seperti hasil CODER: ubah, hapus, file baru (teks & biner), dan file yang di-ignore
    QVERIFY(writeFile(dir.filePath(QStringLiteral("keep.txt")), "satu\nDUA\ntiga\n"));
    QVERIFY(QFile::remove(dir.filePath(QStringLiteral("gone.txt"))));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("sub/inner.txt")), "baru\n"));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("Baru.txt")), "halo\r\ndunia\r\n"));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("gambar.bin")), QByteArray("PNG\0\0data", 9)));
    QVERIFY(dir.mkpath(QStringLiteral("build")));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("build/out.o")), "objek\n"));

    const WorkspaceDiff diff = GitDiff::collect(dir.path());
    QVERIFY2(diff.error.isEmpty(), qPrintable(diff.error));
    QCOMPARE(int(diff.baseCommit.size()), 7);
    QCOMPARE(diffPaths(diff), QStringList({"Baru.txt", "gambar.bin", "gone.txt", "keep.txt", "sub/inner.txt"}));

    const FileDiff &created = diff.files.at(0);
    QVERIFY(created.status == FileDiff::Status::Added);
    QVERIFY(created.untracked);
    QCOMPARE(created.added, 2);
    QCOMPARE(created.lines.at(1).text, QStringLiteral("dunia"));
    QCOMPARE(created.lines.at(1).newLine, 2);

    QVERIFY(diff.files.at(1).untracked);
    QVERIFY(diff.files.at(1).note.contains(QStringLiteral("biner")));
    QVERIFY(diff.files.at(1).lines.isEmpty());

    QVERIFY(diff.files.at(2).status == FileDiff::Status::Deleted);
    QCOMPARE(diff.files.at(2).removed, 1);

    const FileDiff &kept = diff.files.at(3);
    QVERIFY(kept.status == FileDiff::Status::Modified);
    QCOMPARE(kept.added, 1);
    QCOMPARE(kept.removed, 1);
    bool sawRemoved = false;
    bool sawAdded = false;
    for (const DiffLine &line : kept.lines) {
        sawRemoved |= line.kind == DiffLine::Kind::Removed && line.text == QLatin1String("dua") && line.oldLine == 2;
        sawAdded |= line.kind == DiffLine::Kind::Added && line.text == QLatin1String("DUA") && line.newLine == 2;
    }
    QVERIFY(sawRemoved && sawAdded);
    QCOMPARE(diff.added(), 2 + 1 + 1);
    QCOMPARE(diff.removed(), 1 + 1 + 1);

    // Folder kerja = subfolder repository: hanya isinya, dengan path relatif terhadap subfolder
    const WorkspaceDiff inner = GitDiff::collect(dir.filePath(QStringLiteral("sub")));
    QVERIFY2(inner.error.isEmpty(), qPrintable(inner.error));
    QCOMPARE(diffPaths(inner), QStringList({"inner.txt"}));

    // Hanya membaca: tidak ada yang masuk index
    QProcess status;
    status.setWorkingDirectory(dir.path());
    status.start(QStandardPaths::findExecutable(QStringLiteral("git")),
                 {QStringLiteral("diff"), QStringLiteral("--cached"), QStringLiteral("--name-only")});
    QVERIFY(status.waitForFinished(20000));
    QVERIFY(status.readAllStandardOutput().trimmed().isEmpty());
}

void TestSwarm::gitDiffHandlesMissingRepositoryAndFirstCommit() {
    if (QStandardPaths::findExecutable(QStringLiteral("git")).isEmpty()) {
        QSKIP("git tidak ada di PATH");
    }

    // Git tidak boleh naik ke folder induk yang kebetulan repository
    QTemporaryDir plain;
    qputenv("GIT_CEILING_DIRECTORIES", QFileInfo(plain.path()).absolutePath().toUtf8());
    auto restoreEnv = qScopeGuard([]() { qunsetenv("GIT_CEILING_DIRECTORIES"); });

    const WorkspaceDiff missing = GitDiff::collect(plain.path());
    QVERIFY(missing.error.startsWith(QStringLiteral("Folder kerja bukan repository git")));
    QVERIFY(missing.files.isEmpty());

    // Repository baru tanpa commit: file di index dan file baru sama-sama tampil sebagai file baru
    QTemporaryDir fresh;
    const QDir dir(fresh.path());
    QVERIFY(runGit(dir.path(), {QStringLiteral("init"), QStringLiteral("-q")}));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("staged.txt")), "a\nb\n"));
    QVERIFY(runGit(dir.path(), {QStringLiteral("add"), QStringLiteral("staged.txt")}));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("untracked.txt")), "c\n"));

    const WorkspaceDiff first = GitDiff::collect(dir.path());
    QVERIFY2(first.error.isEmpty(), qPrintable(first.error));
    QVERIFY(first.baseCommit.isEmpty());
    QCOMPARE(diffPaths(first), QStringList({"staged.txt", "untracked.txt"}));
    QVERIFY(first.files.at(0).status == FileDiff::Status::Added);
    QVERIFY(!first.files.at(0).untracked);
    QCOMPARE(first.files.at(0).added, 2);
    QVERIFY(first.files.at(1).untracked);
    QCOMPARE(first.added(), 3);
}

void TestSwarm::codeMetricsFormulaAndRating() {
    // 171 − 5,2·ln(1000) − 0,23·5 − 16,2·ln(50) = 70,55 → 41,3 pada skala 0–100
    QCOMPARE(CodeMetrics::maintainabilityIndex(1000.0, 5, 50, 1), 41);
    // Dirata-rata per fungsi: volume 200, kompleksitas 1, 10 baris per fungsi
    QCOMPARE(CodeMetrics::maintainabilityIndex(1000.0, 5, 50, 5), 62);
    QCOMPARE(CodeMetrics::maintainabilityIndex(0.0, 1, 0, 0), 100);
    QCOMPARE(CodeMetrics::maintainabilityIndex(1e7, 500, 5000, 1), 0);

    QVERIFY(maintainabilityRating(20) == MaintainabilityRating::Good);
    QVERIFY(maintainabilityRating(19) == MaintainabilityRating::Moderate);
    QVERIFY(maintainabilityRating(10) == MaintainabilityRating::Moderate);
    QVERIFY(maintainabilityRating(9) == MaintainabilityRating::Low);

    QVERIFY(CodeMetrics::supports(QStringLiteral("src/board.cpp")));
    QVERIFY(CodeMetrics::supports(QStringLiteral("tools/Run.PY")));
    QVERIFY(CodeMetrics::supports(QStringLiteral("app/Views/welcome.php")));
    QVERIFY(!CodeMetrics::supports(QStringLiteral("README.md")));
    QVERIFY(!CodeMetrics::supports(QStringLiteral("package.json")));
    QVERIFY(!CodeMetrics::supports(QStringLiteral("Makefile")));
}

void TestSwarm::codeMetricsCountsBraceLanguages() {
    // Konstruktor dengan initializer list, method, dan lambda = 3 fungsi; if/for/struct bukan fungsi.
    // Keputusan: if, ||, for, &&, ?: = 5
    const QString cpp = QStringLiteral(
        "#include \"Board.h\"\n"
        "#include <array>\n"
        "\n"
        "// Komentar tidak dihitung\n"
        "Board::Board() : m_turn(Player::X) {\n"
        "    reset();\n"
        "}\n"
        "\n"
        "/* blok\n"
        "   komentar */\n"
        "bool Board::place(int index) {\n"
        "    if (index < 0 || index >= 9) {\n"
        "        return false;\n"
        "    }\n"
        "    for (int i = 0; i < 3; ++i) {\n"
        "        m_seen = m_seen && check(i);\n"
        "    }\n"
        "    auto pick = [this](int x) { return x > 0 ? x : -x; };\n"
        "    return pick(index) != 0;\n"
        "}\n"
        "\n"
        "struct Point { int x; int y; };\n");
    const CodeMetrics board = CodeMetrics::measure(QStringLiteral("Board.cpp"), cpp);
    QCOMPARE(board.sloc, 16);
    QCOMPARE(board.functions, 3);
    QCOMPARE(board.complexity, 3 + 5);
    QVERIFY(board.volume > 0.0);
    QCOMPARE(board.maintainability,
             CodeMetrics::maintainabilityIndex(board.volume, board.complexity, board.sloc, board.functions));

    // function, arrow function, constructor, method, fungsi anonim = 5; "else {" dan "class {" bukan
    const QString js = QStringLiteral(
        "import { a } from './a.js';\n"
        "\n"
        "function add(x, y) {\n"
        "  return x + y;\n"
        "}\n"
        "\n"
        "const twice = (f) => {\n"
        "  return (v) => f(f(v));\n"
        "};\n"
        "\n"
        "class Counter {\n"
        "  constructor() { this.n = 0; }\n"
        "  inc() {\n"
        "    if (this.n > 10 && !this.locked) { this.n = 0; } else { this.n++; }\n"
        "  }\n"
        "}\n"
        "\n"
        "setTimeout(function () { console.log(`done ${add(1, 2)}`); }, 10);\n");
    const CodeMetrics counter = CodeMetrics::measure(QStringLiteral("counter.js"), js);
    QCOMPARE(counter.sloc, 14);
    QCOMPARE(counter.functions, 5);
    QCOMPARE(counter.complexity, 5 + 2);

    // Receiver + banyak nilai kembali, closure defer; "if strings.HasPrefix(...) {" tanpa kurung bukan fungsi
    const QString go = QStringLiteral(
        "package main\n"
        "\n"
        "import \"strings\"\n"
        "\n"
        "func (s *Server) Handle(path string) (int, error) {\n"
        "\tif strings.HasPrefix(path, \"/api\") {\n"
        "\t\treturn 200, nil\n"
        "\t}\n"
        "\tfor _, r := range s.routes {\n"
        "\t\tif r == path || r == \"*\" {\n"
        "\t\t\treturn 200, nil\n"
        "\t\t}\n"
        "\t}\n"
        "\treturn 404, nil\n"
        "}\n"
        "\n"
        "func helper() {\n"
        "\tdefer func() {\n"
        "\t\trecover()\n"
        "\t}()\n"
        "}\n");
    const CodeMetrics server = CodeMetrics::measure(QStringLiteral("server.go"), go);
    QCOMPARE(server.sloc, 18);
    QCOMPARE(server.functions, 3);
    QCOMPARE(server.complexity, 3 + 4);

    // Hanya isi <?php ?> yang kode; "?string" tipe nullable, bukan ternary
    const QString php = QStringLiteral(
        "<html>\n"
        "<body>\n"
        "<?php if ($user): ?>\n"
        "  <p>Halo <?= htmlspecialchars($user->name) ?></p>\n"
        "<?php endif; ?>\n"
        "<?php\n"
        "# komentar hash\n"
        "function greet(?string $name): string {\n"
        "    return $name ? \"Halo $name\" : \"Halo\";\n"
        "}\n"
        "?>\n"
        "</body>\n"
        "</html>\n");
    const CodeMetrics view = CodeMetrics::measure(QStringLiteral("welcome.php"), php);
    QCOMPARE(view.sloc, 6);
    QCOMPARE(view.functions, 1);
    QCOMPARE(view.complexity, 1 + 2);

    // Bahasa tak dikenal tidak diukur
    QCOMPARE(CodeMetrics::measure(QStringLiteral("notes.md"), QStringLiteral("# if (x) { y(); }")).sloc, 0);
}

void TestSwarm::codeMetricsCountsPython() {
    // Docstring dan komentar bukan baris kode; "#" di dalam string bukan komentar.
    // Keputusan: for, if, and, if (ekspresi), except = 5
    const QString python = QStringLiteral(
        "\"\"\"Modul contoh.\"\"\"\n"
        "import os\n"
        "\n"
        "\n"
        "def main(args):\n"
        "    \"\"\"Docstring fungsi.\"\"\"\n"
        "    # komentar\n"
        "    total = 0\n"
        "    for a in args:\n"
        "        if a and not a.startswith(\"#\"):\n"
        "            total += 1\n"
        "    return total if total else None\n"
        "\n"
        "\n"
        "class Runner:\n"
        "    def run(self):\n"
        "        try:\n"
        "            return main([])\n"
        "        except ValueError:\n"
        "            return 0\n");
    const CodeMetrics metrics = CodeMetrics::measure(QStringLiteral("main.py"), python);
    QCOMPARE(metrics.sloc, 13);
    QCOMPARE(metrics.functions, 2);
    QCOMPARE(metrics.complexity, 2 + 5);
}

void TestSwarm::gitDiffMeasuresMaintainability() {
    if (QStandardPaths::findExecutable(QStringLiteral("git")).isEmpty()) {
        QSKIP("git tidak ada di PATH");
    }

    QTemporaryDir repo;
    const QDir dir(repo.path());
    QVERIFY(runGit(dir.path(), {QStringLiteral("init"), QStringLiteral("-q")}));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("calc.py")), "def hitung(x):\n    return x * 2\n"));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("lama.js")), "function lama() {\n  return 1;\n}\n"));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("util.js")),
                      "export function a(x) {\n  return x + 1;\n}\n\nexport function b(y) {\n  return y * 2;\n}\n"));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("README.md")), "# Proyek\n"));
    QVERIFY(runGit(dir.path(), {QStringLiteral("add"), QStringLiteral("-A")}));
    QVERIFY(runGit(dir.path(), {QStringLiteral("commit"), QStringLiteral("-q"), QStringLiteral("-m"), QStringLiteral("awal")}));

    // Fungsi jadi lebih bercabang, satu file dihapus, satu diganti nama sambil diubah, satu file baru
    QVERIFY(writeFile(dir.filePath(QStringLiteral("calc.py")),
                      "def hitung(x):\n    if x > 10 and x < 100:\n        return x * 2\n"
                      "    for i in range(x):\n        if i % 2:\n            x += i\n    return x\n"));
    QVERIFY(QFile::remove(dir.filePath(QStringLiteral("lama.js"))));
    QVERIFY(runGit(dir.path(), {QStringLiteral("mv"), QStringLiteral("util.js"), QStringLiteral("helpers.js")}));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("helpers.js")),
                      "export function a(x) {\n  return x > 0 ? x + 1 : 0;\n}\n\nexport function b(y) {\n  return y * 2;\n}\n"));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("baru.go")), "package main\n\nfunc main() {\n\tprintln(\"halo\")\n}\n"));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("README.md")), "# Proyek\n\nKeterangan.\n"));

    const WorkspaceDiff diff = GitDiff::collect(dir.path());
    QVERIFY2(diff.error.isEmpty(), qPrintable(diff.error));
    QCOMPARE(diffPaths(diff), QStringList({"baru.go", "calc.py", "helpers.js", "lama.js", "README.md"}));

    const FileDiff &created = diff.files.at(0);
    QVERIFY(!created.before);
    QVERIFY(created.after);
    QCOMPARE(created.after->functions, 1);

    const FileDiff &calc = diff.files.at(1);
    QVERIFY(calc.before && calc.after);
    QCOMPARE(calc.before->complexity, 1);
    QCOMPARE(calc.after->complexity, 1 + 4);
    QVERIFY(calc.after->maintainability < calc.before->maintainability);

    // Sisi "sebelum" file yang diganti nama dibaca dari nama lamanya
    const FileDiff &helpers = diff.files.at(2);
    QVERIFY(helpers.status == FileDiff::Status::Renamed);
    QCOMPARE(helpers.oldPath, QStringLiteral("util.js"));
    QVERIFY(helpers.before && helpers.after);
    QCOMPARE(helpers.before->functions, 2);
    QCOMPARE(helpers.after->complexity, helpers.before->complexity + 1);

    const FileDiff &deleted = diff.files.at(3);
    QVERIFY(deleted.before);
    QVERIFY(!deleted.after);

    QVERIFY(!diff.files.at(4).before && !diff.files.at(4).after);

    // Rata-rata tertimbang baris kode dari file yang terukur di tiap sisi
    const std::optional<int> before = diff.maintainabilityBefore();
    const std::optional<int> after = diff.maintainabilityAfter();
    QVERIFY(before && after);
    const auto weighted = [](std::initializer_list<CodeMetrics> files) {
        double sum = 0;
        int lines = 0;
        for (const CodeMetrics &metrics : files) {
            sum += metrics.maintainability * metrics.sloc;
            lines += metrics.sloc;
        }
        return int(std::lround(sum / lines));
    };
    QCOMPARE(*before, weighted({*calc.before, *helpers.before, *deleted.before}));
    QCOMPARE(*after, weighted({*created.after, *calc.after, *helpers.after}));
}

void TestSwarm::csharpMetricsParsesTypesAndMembers() {
    const QString source = QStringLiteral(R"cs(using System;
using System.Collections.Generic;
using System.Linq;

namespace Shop.Orders;

[Serializable]
public class OrderService : ServiceBase, IOrderService
{
    private readonly IRepository<Order> _repository;
    private static readonly string Prefix = "ORD";

    public int Count { get; private set; }

    public decimal Total => _repository.All().Sum(o => o.Amount);

    public string Label
    {
        get { return Count > 0 ? $"{Prefix}-{(Count > 1 ? "x" : "y")}" : Prefix; }
        set { Count = value?.Length ?? 0; }
    }

    public OrderService(IRepository<Order> repository)
    {
        _repository = repository ?? throw new ArgumentNullException(nameof(repository));
    }

    public Order Place(Customer customer, int quantity)
    {
        if (customer == null || quantity <= 0)
        {
            throw new InvalidOperationException("Pesanan tidak valid");
        }
        else if (quantity > 100 && !customer.IsVip)
        {
            quantity = 100;
        }

        var order = new Order(customer, quantity);
        foreach (var item in customer.Cart)
        {
            order.Add(item);
        }
        try
        {
            _repository.Save(order);
        }
        catch (TimeoutException ex) when (ex.Message.Length > 0)
        {
            Log(ex);
        }
        return order;
    }

    public static string Describe(Order order) => order.Status switch
    {
        OrderStatus.New => "baru",
        OrderStatus.Paid or OrderStatus.Shipped => "diproses",
        _ => "lainnya",
    };

    private void Log(Exception ex)
    {
        Console.WriteLine(@"C:\logs\" + ex.Message);
        int Twice(int x) => x * 2;
        var n = Twice(Count);
    }

    public class Snapshot
    {
        public DateTime TakenAt { get; init; }
    }
}

public struct Money
{
    public decimal Amount;
    public static Money operator +(Money a, Money b) => new Money { Amount = a.Amount + b.Amount };
}

public interface IOrderService
{
    Order Place(Customer customer, int quantity);
    int Count { get; }
}

public enum OrderStatus { New, Paid, Shipped }

public record Receipt(Guid Id, decimal Amount);
)cs");
    const CodeMetrics metrics = CodeMetrics::measure(QStringLiteral("Services/OrderService.cs"), source);
    const QList<TypeMetrics> &types = metrics.types;

    // Urutan sumber; tipe bersarang punya entri sendiri
    QStringList names;
    for (const TypeMetrics &type : types) {
        names.append(type.kind + QLatin1Char(' ') + type.fullName());
    }
    QCOMPARE(names, QStringList({"class Shop.Orders.OrderService", "class Shop.Orders.OrderService.Snapshot",
                                 "struct Shop.Orders.Money", "interface Shop.Orders.IOrderService",
                                 "enum Shop.Orders.OrderStatus", "record Shop.Orders.Receipt"}));

    // Hanya member berkode: auto-property, field, dan tipe bersarang bukan member
    const TypeMetrics *service = findType(types, QStringLiteral("OrderService"));
    QVERIFY(service);
    QCOMPARE(memberNames(*service), QStringList({"Total", "Label", "OrderService(IRepository<Order>)",
                                                 "Place(Customer, int)", "Describe(Order)", "Log(Exception)"}));
    QCOMPARE(service->baseClass, QStringLiteral("ServiceBase"));

    // Kompleksitas: ternary vs "?" nullable, ?. dan ??, else if, filter when, arm switch expression
    QCOMPARE(findMember(*service, QStringLiteral("Total"))->complexity, 1);
    QCOMPARE(findMember(*service, QStringLiteral("Label"))->complexity, 1 + 3);
    QCOMPARE(findMember(*service, QStringLiteral("OrderService(IRepository<Order>)"))->complexity, 1 + 1);
    QCOMPARE(findMember(*service, QStringLiteral("Place(Customer, int)"))->complexity, 1 + 7);
    QCOMPARE(findMember(*service, QStringLiteral("Describe(Order)"))->complexity, 1 + 3);
    QCOMPARE(findMember(*service, QStringLiteral("Log(Exception)"))->complexity, 1);
    QCOMPARE(service->complexity, 1 + 4 + 2 + 8 + 4 + 1);

    // Class coupling: tipe di parameter, return, new, catch, akses statis, generic, atribut, base list
    QCOMPARE(findMember(*service, QStringLiteral("Place(Customer, int)"))->coupling, 4);
    QCOMPARE(findMember(*service, QStringLiteral("OrderService(IRepository<Order>)"))->coupling, 3);
    QCOMPARE(findMember(*service, QStringLiteral("Describe(Order)"))->coupling, 2);
    QCOMPARE(findMember(*service, QStringLiteral("Log(Exception)"))->coupling, 2);
    QCOMPARE(findMember(*service, QStringLiteral("Label"))->coupling, 0);
    QCOMPARE(service->coupling, 12);

    // Baris kode member: baris kosong tidak dihitung
    QCOMPARE(findMember(*service, QStringLiteral("Total"))->lines, 1);
    QCOMPARE(findMember(*service, QStringLiteral("OrderService(IRepository<Order>)"))->lines, 4);
    QCOMPARE(findMember(*service, QStringLiteral("Place(Customer, int)"))->lines, 25);
    const MemberMetrics *place = findMember(*service, QStringLiteral("Place(Customer, int)"));
    QCOMPARE(place->maintainability,
             CodeMetrics::maintainabilityIndex(place->volume, place->complexity, place->lines, 1));

    // DIT: kelas dasar di luar file ini tidak dikenal → minimal
    QCOMPARE(service->inheritanceDepth, 2);
    QVERIFY(service->inheritanceOpen);

    const TypeMetrics *snapshot = findType(types, QStringLiteral("OrderService.Snapshot"));
    QVERIFY(snapshot && snapshot->members.isEmpty());
    QCOMPARE(snapshot->coupling, 1);   // DateTime
    QCOMPARE(snapshot->maintainability, 100);
    QCOMPARE(snapshot->inheritanceDepth, 1);

    const TypeMetrics *money = findType(types, QStringLiteral("Money"));
    QCOMPARE(memberNames(*money), QStringList({"operator +(Money, Money)"}));
    QCOMPARE(money->coupling, 0);   // hanya dirinya sendiri
    QCOMPARE(money->inheritanceDepth, 2);

    const TypeMetrics *contract = findType(types, QStringLiteral("IOrderService"));
    QVERIFY(contract->members.isEmpty());
    QCOMPARE(contract->coupling, 2);   // Order, Customer
    QCOMPARE(contract->inheritanceDepth, 0);

    QCOMPARE(findType(types, QStringLiteral("OrderStatus"))->inheritanceDepth, 3);
    QCOMPARE(findType(types, QStringLiteral("Receipt"))->coupling, 1);   // Guid
    QCOMPARE(findType(types, QStringLiteral("Receipt"))->inheritanceDepth, 1);

    // Tingkat file memakai member hasil pengurai C#
    QCOMPARE(metrics.functions, 6 + 1);
    QCOMPARE(metrics.complexity, 20 + 1);
}

void TestSwarm::csharpMetricsHandlesStringsAndNullable() {
    // String verbatim berakhiran '\', interpolasi berisi kutip, dan raw string tidak boleh merusak
    // pemisahan member; "int?" bukan ternary
    const QString source = QStringLiteral(R"cs(class Paths
{
    string A() => @"C:\dir\" + "x";

    string B(bool ok)
    {
        var s = $"{(ok ? "ya" : "tidak")} {{literal}}";
        if (ok) { return s; }
        return @"say ""hi""";
    }

    string C() => """
        raw "quoted" { not a brace }
        """;

    int D(int? x, List<Dictionary<string, int>> map) => x ?? map.Count;

    int this[int index] => index;
}
)cs");
    const QList<TypeMetrics> types = CodeMetrics::measure(QStringLiteral("Paths.cs"), source).types;
    QCOMPARE(int(types.size()), 1);
    const TypeMetrics &paths = types.first();
    QCOMPARE(memberNames(paths), QStringList({"A()", "B(bool)", "C()", "D(int?, List<Dictionary<string, int>>)",
                                              "this[int]"}));
    QCOMPARE(findMember(paths, QStringLiteral("A()"))->complexity, 1);
    QCOMPARE(findMember(paths, QStringLiteral("B(bool)"))->complexity, 2);
    QCOMPARE(findMember(paths, QStringLiteral("C()"))->complexity, 1);
    QCOMPARE(findMember(paths, QStringLiteral("D(int?, List<Dictionary<string, int>>)"))->complexity, 2);
    QCOMPARE(findMember(paths, QStringLiteral("B(bool)"))->lines, 6);
    QCOMPARE(findMember(paths, QStringLiteral("C()"))->lines, 3);
}

void TestSwarm::csharpMetricsResolvesInheritance() {
    // Peta kelas dasar dari teks sumber: interface/struct tidak punya kelas dasar, partial digabung
    const QHash<QString, QString> bases = CSharpMetrics::declaredBases(QStringLiteral(
        "namespace Shop;\n"
        "public abstract class ServiceBase : Controller, IDisposable { }\n"
        "public partial class Widget { }\n"
        "public partial class Widget : Component { }\n"
        "public interface IThing : IDisposable { }\n"
        "public record Point(int X, int Y) : Shape(X);\n"
        "public struct Size : IEquatable<Size> { }\n"
        "public class Box<T> where T : class { }\n"
        "public class Repo<T> : Base.Repository<T> where T : Entity { }\n"));
    QCOMPARE(bases.value(QStringLiteral("ServiceBase")), QStringLiteral("Controller"));
    QCOMPARE(bases.value(QStringLiteral("Widget")), QStringLiteral("Component"));
    QVERIFY(bases.contains(QStringLiteral("IThing")) && bases.value(QStringLiteral("IThing")).isEmpty());
    QCOMPARE(bases.value(QStringLiteral("Point")), QStringLiteral("Shape"));
    QVERIFY(bases.value(QStringLiteral("Size")).isEmpty());
    QVERIFY(bases.contains(QStringLiteral("Box")) && bases.value(QStringLiteral("Box")).isEmpty());
    QCOMPARE(bases.value(QStringLiteral("Repo")), QStringLiteral("Repository"));

    QList<TypeMetrics> types(4);
    types[0].kind = QStringLiteral("class");
    types[0].baseClass = QStringLiteral("ServiceBase");
    types[1].kind = QStringLiteral("class");
    types[1].baseClass = QStringLiteral("MonoBehaviour");
    types[2].kind = QStringLiteral("class");
    types[2].baseClass = QStringLiteral("ThirdPartyBase");
    types[3].kind = QStringLiteral("class");
    types[3].baseClass = QStringLiteral("Loop");
    const QHash<QString, QString> project = {{"ServiceBase", "Controller"}, {"Loop", "Cycle"}, {"Cycle", "Loop"}};
    CSharpMetrics::resolveInheritance(&types, project);

    // OrderService → ServiceBase → Controller → ControllerBase → Object
    QCOMPARE(types[0].inheritanceDepth, 4);
    QVERIFY(!types[0].inheritanceOpen);
    // MonoBehaviour → Behaviour → Component → UnityEngine.Object → Object
    QCOMPARE(types[1].inheritanceDepth, 5);
    QVERIFY(!types[1].inheritanceOpen);
    // Kelas dasar tak dikenal: minimal dirinya + kelas dasar itu
    QCOMPARE(types[2].inheritanceDepth, 2);
    QVERIFY(types[2].inheritanceOpen);
    // Rantai melingkar tetap berhenti
    QVERIFY(types[3].inheritanceOpen);
    QVERIFY(types[3].inheritanceDepth < 40);
}

void TestSwarm::gitDiffMeasuresCSharpTypes() {
    if (QStandardPaths::findExecutable(QStringLiteral("git")).isEmpty()) {
        QSKIP("git tidak ada di PATH");
    }

    QTemporaryDir repo;
    const QDir dir(repo.path());
    QVERIFY(runGit(dir.path(), {QStringLiteral("init"), QStringLiteral("-q")}));
    // Kelas dasar ada di file lain yang tidak berubah: DIT tetap terbaca dari seluruh project
    QVERIFY(writeFile(dir.filePath(QStringLiteral("Services/ServiceBase.cs")),
                      "namespace Shop;\npublic abstract class ServiceBase : Controller { }\n"));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("Services/OrderService.cs")),
                      "namespace Shop;\n"
                      "public class OrderService : ServiceBase\n"
                      "{\n"
                      "    public int Place(int quantity)\n"
                      "    {\n"
                      "        return quantity;\n"
                      "    }\n"
                      "}\n"));
    QVERIFY(runGit(dir.path(), {QStringLiteral("add"), QStringLiteral("-A")}));
    QVERIFY(runGit(dir.path(), {QStringLiteral("commit"), QStringLiteral("-q"), QStringLiteral("-m"), QStringLiteral("awal")}));

    QVERIFY(writeFile(dir.filePath(QStringLiteral("Services/OrderService.cs")),
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
                      "}\n"));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("Models/Order.cs")),
                      "namespace Shop;\npublic class Order : Entity\n{\n    public void Close() { }\n}\n"));

    const WorkspaceDiff diff = GitDiff::collect(dir.path());
    QVERIFY2(diff.error.isEmpty(), qPrintable(diff.error));
    QCOMPARE(diffPaths(diff), QStringList({"Models/Order.cs", "Services/OrderService.cs"}));

    const FileDiff &order = diff.files.at(0);
    QVERIFY(order.after && !order.before);
    QCOMPARE(int(order.after->types.size()), 1);
    QCOMPARE(order.after->types.first().inheritanceDepth, 2);
    QVERIFY(order.after->types.first().inheritanceOpen);

    const FileDiff &service = diff.files.at(1);
    QVERIFY(service.before && service.after);
    const TypeMetrics *before = findType(service.before->types, QStringLiteral("OrderService"));
    const TypeMetrics *after = findType(service.after->types, QStringLiteral("OrderService"));
    QVERIFY(before && after);
    // OrderService → ServiceBase (file lain) → Controller → ControllerBase → Object
    QCOMPARE(after->inheritanceDepth, 4);
    QVERIFY(!after->inheritanceOpen);
    QCOMPARE(findMember(*before, QStringLiteral("Place(int)"))->complexity, 1);
    QCOMPARE(findMember(*after, QStringLiteral("Place(int)"))->complexity, 1 + 3);
    QVERIFY(!findMember(*before, QStringLiteral("Cancel(Order)")));
    QCOMPARE(findMember(*after, QStringLiteral("Cancel(Order)"))->coupling, 1);
}

void TestSwarm::gitDiffCanSkipMetrics() {
    if (QStandardPaths::findExecutable(QStringLiteral("git")).isEmpty()) {
        QSKIP("git tidak ada di PATH");
    }

    QTemporaryDir repo;
    const QDir dir(repo.path());
    QVERIFY(runGit(dir.path(), {QStringLiteral("init"), QStringLiteral("-q")}));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("Order.cs")),
                      "namespace Shop;\npublic class Order : Entity\n{\n    public void Close() { }\n}\n"));
    QVERIFY(runGit(dir.path(), {QStringLiteral("add"), QStringLiteral("-A")}));
    QVERIFY(runGit(dir.path(), {QStringLiteral("commit"), QStringLiteral("-q"), QStringLiteral("-m"), QStringLiteral("awal")}));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("Order.cs")),
                      "namespace Shop;\npublic class Order : Entity\n{\n    public void Close() { if (true) { } }\n}\n"));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("hitung.py")), "def hitung(x):\n    return x * 2\n"));

    // Bawaan: file kode diukur dan tipe C# project terbaca
    const WorkspaceDiff measured = GitDiff::collect(dir.path());
    QVERIFY2(measured.error.isEmpty(), qPrintable(measured.error));
    QCOMPARE(diffPaths(measured), QStringList({"hitung.py", "Order.cs"}));
    QVERIFY(measured.files.at(0).after);
    QVERIFY(measured.files.at(1).before && measured.files.at(1).after);
    QVERIFY(!measured.csharpTypes.isEmpty());
    QVERIFY(measured.maintainabilityAfter());

    // Tanpa pengukuran: diff-nya sama persis, hanya metrik yang kosong
    const WorkspaceDiff plain = GitDiff::collect(dir.path(), false);
    QVERIFY2(plain.error.isEmpty(), qPrintable(plain.error));
    QCOMPARE(diffPaths(plain), diffPaths(measured));
    QCOMPARE(plain.baseCommit, measured.baseCommit);
    QCOMPARE(plain.added(), measured.added());
    QCOMPARE(plain.removed(), measured.removed());
    for (const FileDiff &file : plain.files) {
        QVERIFY(!file.before);
        QVERIFY(!file.after);
    }
    QVERIFY(plain.csharpTypes.isEmpty());
    QVERIFY(!plain.maintainabilityBefore());
    QVERIFY(!plain.maintainabilityAfter());
}

namespace {

// "method +abstract Place(Customer, int) : Order"
QString outlineEntry(const TypeMember &member) {
    static const QStringList kinds = {"field", "property", "event", "ctor", "method", "value"};
    QString text = kinds.at(int(member.kind)) + QLatin1Char(' ');
    if (!member.visibility.isNull()) {
        text += member.visibility;
    }
    if (member.isStatic) {
        text += QStringLiteral("static ");
    }
    if (member.isAbstract) {
        text += QStringLiteral("abstract ");
    }
    text += member.name;
    if (member.kind == TypeMember::Kind::Method || member.kind == TypeMember::Kind::Constructor
        || member.name == QLatin1String("this")) {
        text += QStringLiteral("(%1)").arg(member.parameters);
    }
    if (!member.type.isEmpty()) {
        text += QStringLiteral(" : %1").arg(member.type);
    }
    return text;
}

QStringList outlineOf(const TypeMetrics &type) {
    QStringList entries;
    for (const TypeMember &member : type.outline) {
        entries.append(outlineEntry(member));
    }
    return entries;
}

}

void TestSwarm::csharpMetricsRecordsOutline() {
    const QString source = QStringLiteral(R"cs(namespace Shop;

public abstract class OrderService : ServiceBase, IOrderService, IDisposable
{
    private readonly IRepository<Order> _repository;
    public const int MaxItems = 100;
    private int _a, _b;
    public event EventHandler Changed;
    protected internal decimal Total { get; private set; }
    public string this[int index] => index.ToString();

    public OrderService(IRepository<Order> repository) { _repository = repository; }
    static OrderService() { }
    public abstract Order Place(Customer customer, int quantity);
    public static List<Order> Filter(IEnumerable<Order> orders) => orders.ToList();
    void IDisposable.Dispose() { }
    ~OrderService() { }
    public static Money operator +(Money a, Money b) => a;
}

public record Receipt(Guid Id, [property: Required] decimal Amount = 0);

public enum OrderStatus { New = 1, [Description("x")] Paid, Shipped }

public interface IRepository<T> where T : class
{
    void Save(T item);
}
)cs");
    const QList<TypeMetrics> types = CodeMetrics::measure(QStringLiteral("OrderService.cs"), source).types;

    // Semua anggota untuk diagram kelas, termasuk field dan tanpa-badan; operator dan destructor tidak
    const TypeMetrics *service = findType(types, QStringLiteral("OrderService"));
    QVERIFY(service);
    QVERIFY(service->isAbstract);
    QCOMPARE(service->baseClass, QStringLiteral("ServiceBase"));
    QCOMPARE(service->interfaces, QStringList({"IOrderService", "IDisposable"}));
    QCOMPARE(outlineOf(*service), QStringList({
        "field -_repository : IRepository<Order>",
        "field +static MaxItems : int",
        "field -_a : int",
        "field -_b : int",
        "event +Changed : EventHandler",
        "property #Total : decimal",
        "property +this(int) : string",
        "ctor +OrderService(IRepository<Order>)",
        "ctor -static OrderService()",
        "method +abstract Place(Customer, int) : Order",
        "method +static Filter(IEnumerable<Order>) : List<Order>",
        "method -Dispose() : void",
    }));

    // Parameter record posisional = property publik
    QCOMPARE(outlineOf(*findType(types, QStringLiteral("Receipt"))),
             QStringList({"property +Id : Guid", "property +Amount : decimal"}));
    QCOMPARE(outlineOf(*findType(types, QStringLiteral("OrderStatus"))),
             QStringList({"value New", "value Paid", "value Shipped"}));

    // Anggota interface tanpa modifier = public
    const TypeMetrics *repository = findType(types, QStringLiteral("IRepository"));
    QCOMPARE(repository->genericParameters, QStringList({"T"}));
    QCOMPARE(outlineOf(*repository), QStringList({"method +Save(T) : void"}));
}

void TestSwarm::classDiagramDrawsChangedTypes() {
    const QString servicePath = QStringLiteral("Services/OrderService.cs");
    FileDiff service;
    service.path = servicePath;
    service.before = CodeMetrics::measure(servicePath, QStringLiteral(
        "namespace Shop;\n"
        "public class OrderService : ServiceBase, IOrderService\n"
        "{\n"
        "    private readonly IRepository<Order> _repository;\n"
        "    public Order Place(Customer customer) { return new Order(customer); }\n"
        "}\n"
        "public class Unchanged { public int Value; }\n"));
    service.after = CodeMetrics::measure(servicePath, QStringLiteral(
        "namespace Shop;\n"
        "public class OrderService : ServiceBase, IOrderService\n"
        "{\n"
        "    private readonly IRepository<Order> _repository;\n"
        "    public Order Place(Customer customer, int quantity) { if (quantity > 0) { return new Order(customer); } return null; }\n"
        "    public static string Describe(Order order) => order.Status.ToString();\n"
        "\n"
        "    public class Snapshot { public DateTime TakenAt { get; init; } }\n"
        "}\n"
        "public class Unchanged { public int Value; }\n"));

    const QString orderPath = QStringLiteral("Models/Order.cs");
    FileDiff order;
    order.path = orderPath;
    order.status = FileDiff::Status::Added;
    order.after = CodeMetrics::measure(orderPath, QStringLiteral(
        "namespace Shop;\n"
        "public class Order : Entity\n"
        "{\n"
        "    public OrderStatus Status { get; private set; }\n"
        "    public List<OrderLine> Lines { get; } = new();\n"
        "    public Dictionary<string, List<OrderLine>> ByCode { get; } = new();\n"
        "    public void Add(OrderLine line) { Lines.Add(line); }\n"
        "}\n"
        "public enum OrderStatus { New, Paid }\n"));

    WorkspaceDiff diff;
    diff.files = {order, service};
    // Tipe yang dideklarasikan di project (hasil pindai seluruh file .cs)
    diff.csharpTypes = {{"OrderService", "ServiceBase"}, {"ServiceBase", "ControllerBase"}, {"IOrderService", ""},
                        {"Customer", ""}, {"OrderLine", ""}, {"Order", "Entity"}, {"OrderStatus", ""}};

    const ClassDiagram diagram = ClassDiagram::fromDiff(diff);
    const QString code = diagram.code;
    QVERIFY(code.startsWith(QStringLiteral("classDiagram\n")));
    QCOMPARE(diagram.drawnTypes, 5);
    QCOMPARE(diagram.omittedTypes, 0);

    // Kelas yang berubah digambar lengkap; generic memakai ~T~, static ditandai $
    QVERIFY(code.contains(QStringLiteral("    class OrderService {\n")));
    QVERIFY(code.contains(QStringLiteral("        -IRepository~Order~ _repository\n")));
    QVERIFY(code.contains(QStringLiteral("        +Place(Customer, int) Order\n")));
    QVERIFY(code.contains(QStringLiteral("        +Describe(Order)$ string\n")));
    QVERIFY(code.contains(QStringLiteral("    class OrderService_Snapshot[\"OrderService.Snapshot\"] {\n")));
    QVERIFY(code.contains(QStringLiteral("        +List~OrderLine~ Lines\n")));
    // Generic bersarang: hanya tingkat terluar memakai ~ (Mermaid salah membaca ~ bersarang)
    QVERIFY(code.contains(QStringLiteral("        +Dictionary~string, List‹OrderLine›~ ByCode\n")));
    QVERIFY(code.contains(QStringLiteral("    class OrderStatus {\n        <<enumeration>>\n        New\n        Paid\n")));

    // Relasi: pewarisan (termasuk kelas dasar di luar project), implementasi, asosiasi, dependensi
    QVERIFY(code.contains(QStringLiteral("    ServiceBase <|-- OrderService\n")));
    QVERIFY(code.contains(QStringLiteral("    IOrderService <|.. OrderService\n")));
    QVERIFY(code.contains(QStringLiteral("    Entity <|-- Order\n")));
    QVERIFY(code.contains(QStringLiteral("    Order --> OrderStatus : Status\n")));
    QVERIFY(code.contains(QStringLiteral("    Order --> \"*\" OrderLine : Lines\n")));
    QVERIFY(code.contains(QStringLiteral("    OrderService ..> Customer\n")));
    QVERIFY(code.contains(QStringLiteral("    OrderService ..> Order\n")));
    QVERIFY(!code.contains(QStringLiteral("Order ..> OrderLine")));   // sudah ada asosiasinya
    QVERIFY(!code.contains(QStringLiteral("IRepository <")));        // bukan tipe project
    QVERIFY(code.contains(QStringLiteral("    class IOrderService {\n        <<interface>>\n    }\n")));

    // Warna: baru hijau, berubah kuning, tidak berubah tanpa gaya, konteks abu-abu
    QVERIFY(code.contains(QStringLiteral("    style Order fill:#e6ffec")));
    QVERIFY(code.contains(QStringLiteral("    style OrderService_Snapshot fill:#e6ffec")));
    QVERIFY(code.contains(QStringLiteral("    style OrderService fill:#fbeccf")));
    QVERIFY(!code.contains(QStringLiteral("style Unchanged ")));
    QVERIFY(code.contains(QStringLiteral("    style ServiceBase fill:#f4f1ec")));

    // Tanpa tipe C#, tidak ada diagram
    WorkspaceDiff python;
    FileDiff script;
    script.path = QStringLiteral("hitung.py");
    script.after = CodeMetrics::measure(script.path, QStringLiteral("def f(x):\n    return x\n"));
    python.files = {script};
    QVERIFY(ClassDiagram::fromDiff(python).code.isEmpty());
}

void TestSwarm::classDiagramLimitsSize() {
    QString source = QStringLiteral("namespace Big;\n");
    for (int i = 0; i < 15; ++i) {
        source += QStringLiteral("public class Part%1 { public int Value%1; }\n").arg(i);
    }
    FileDiff file;
    file.path = QStringLiteral("Parts.cs");
    file.status = FileDiff::Status::Added;
    file.after = CodeMetrics::measure(file.path, source);
    WorkspaceDiff diff;
    diff.files = {file};

    const ClassDiagram diagram = ClassDiagram::fromDiff(diff);
    QCOMPARE(diagram.drawnTypes, 12);
    QCOMPARE(diagram.omittedTypes, 3);
    QVERIFY(diagram.code.contains(QStringLiteral("class Part11 {")));
    QVERIFY(!diagram.code.contains(QStringLiteral("class Part12 {")));
}

void TestSwarm::realEdgeRendersMermaid() {
    if (qEnvironmentVariableIsEmpty("LASSOMOIR_REAL_MERMAID")) {
        QSKIP("Set LASSOMOIR_REAL_MERMAID=1 untuk merender dengan Edge sungguhan (tanpa token)");
    }

    QTemporaryDir cache;
    EdgeMermaidRenderer renderer(EdgeMermaidRenderer::findBrowser(), cache.path());
    QString reason;
    QVERIFY2(renderer.isAvailable(&reason), qPrintable(reason));

    QList<QImage> images;
    QStringList errors;
    connect(&renderer, &MermaidRenderer::rendered, this,
            [&images](const QString &, const QImage &image) { images.append(image); });
    connect(&renderer, &MermaidRenderer::failed, this,
            [&errors](const QString &, const QString &error) { errors.append(error); });

    const QString code = QStringLiteral("graph TD\n  A[SPECIFIER] -->|Setujui| B[CODER]\n  B --> C[QA]");
    QElapsedTimer timer;
    timer.start();
    renderer.render(code);
    QTRY_VERIFY_WITH_TIMEOUT(!images.isEmpty() || !errors.isEmpty(), 60000);
    QVERIFY2(errors.isEmpty(), qPrintable(errors.join(QStringLiteral(", "))));
    qInfo("render pertama: %lld ms", timer.elapsed());

    const QImage image = images.first();
    QVERIFY(image.width() > 50 && image.height() > 50);
    QVERIFY(image.width() < 2800);   // kanvas 2800x4800 sudah dipotong ke isi diagram
    QCOMPARE(image.devicePixelRatio(), 2.0);
    QVERIFY(QFileInfo::exists(cache.filePath(MermaidRenderer::keyFor(code) + QStringLiteral(".png"))));
    const QString out = qEnvironmentVariable("LASSOMOIR_REAL_MERMAID_OUT");
    if (!out.isEmpty()) {
        image.save(out);
    }

    // Render kedua langsung dari cache
    timer.restart();
    renderer.render(code);
    QTRY_COMPARE_WITH_TIMEOUT(int(images.size()), 2, 5000);
    QVERIFY(timer.elapsed() < 1000);
}

void TestSwarm::documentTextReadsCsv() {
    QTemporaryDir dir;

    // BOM UTF-8 dan akhir baris Windows: BOM dibuang, CRLF jadi LF
    const QString utf8 = dir.filePath(QStringLiteral("data.csv"));
    QVERIFY(writeFile(utf8, "\xEF\xBB\xBFnama,kota\r\nBudi,S\xC3\xA9marang\r\n"));
    QString error;
    QCOMPARE(DocumentText::extract(utf8, &error), QStringLiteral("nama,kota\nBudi,S\u00e9marang\n"));
    QVERIFY(error.isEmpty());

    // "CSV" Excel lama (code page Windows, bukan UTF-8 yang sah) tetap terbaca tanpa karakter rusak
    const QString ansi = dir.filePath(QStringLiteral("ansi.csv"));
    QVERIFY(writeFile(ansi, "nama;kota\nJos\xE9;Caf\xE9\n"));
    const QString decoded = DocumentText::extract(ansi, &error);
    QVERIFY(decoded.startsWith(QStringLiteral("nama;kota\nJos")));
    QVERIFY(!decoded.contains(QChar::ReplacementCharacter));

    // TSV: tab tetap tab
    const QString tsv = dir.filePath(QStringLiteral("data.tsv"));
    QVERIFY(writeFile(tsv, "a\tb\n1\t2\n"));
    QCOMPARE(DocumentText::extract(tsv), QStringLiteral("a\tb\n1\t2\n"));
    QVERIFY(DocumentText::check(tsv));
}

void TestSwarm::documentTextReadsDocx() {
    const QByteArray styles = R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<w:styles xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">
  <w:style w:type="paragraph" w:styleId="Judul1"><w:name w:val="heading 1"/></w:style>
  <w:style w:type="paragraph" w:styleId="Subjudul"><w:name w:val="Subjudul Saya"/><w:pPr><w:outlineLvl w:val="1"/></w:pPr></w:style>
</w:styles>)";
    // Word berbahasa Indonesia: styleId judul "Judul1", tapi nama gaya bawaannya tetap "heading 1"
    const QByteArray document = R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main" xmlns:mc="http://schemas.openxmlformats.org/markup-compatibility/2006">
<w:body>
  <w:p><w:pPr><w:pStyle w:val="Judul1"/></w:pPr><w:r><w:t>Spesifikasi Login</w:t></w:r></w:p>
  <w:p><w:r><w:t xml:space="preserve">Halaman </w:t></w:r><w:r><w:rPr><w:b/></w:rPr><w:t>login</w:t></w:r><w:del w:id="1"><w:r><w:delText>lama</w:delText></w:r></w:del><w:r><w:tab/><w:t>baru</w:t><w:br/><w:t>baris dua</w:t></w:r></w:p>
  <w:p/>
  <w:p><w:pPr><w:pStyle w:val="Subjudul"/></w:pPr><w:r><w:t>Kriteria</w:t></w:r></w:p>
  <w:p><w:pPr><w:numPr><w:ilvl w:val="0"/><w:numId w:val="3"/></w:numPr></w:pPr><w:r><w:t>Email wajib</w:t></w:r></w:p>
  <w:p><w:pPr><w:numPr><w:ilvl w:val="1"/><w:numId w:val="3"/></w:numPr></w:pPr><w:r><w:t>Format valid</w:t></w:r></w:p>
  <w:tbl>
    <w:tr><w:tc><w:p><w:r><w:t>Field</w:t></w:r></w:p></w:tc><w:tc><w:p><w:r><w:t>Aturan</w:t></w:r></w:p></w:tc></w:tr>
    <w:tr><w:tc><w:p><w:r><w:t>Password</w:t></w:r></w:p></w:tc><w:tc><w:p><w:r><w:t>min 8 | huruf</w:t></w:r></w:p><w:p><w:r><w:t>angka</w:t></w:r></w:p></w:tc></w:tr>
  </w:tbl>
  <w:p><w:r><mc:AlternateContent><mc:Choice Requires="wps"><w:drawing><w:txbxContent><w:p><w:r><w:t>Catatan kotak</w:t></w:r></w:p></w:txbxContent></w:drawing></mc:Choice><mc:Fallback><w:pict><w:txbxContent><w:p><w:r><w:t>Catatan kotak</w:t></w:r></w:p></w:txbxContent></w:pict></mc:Fallback></mc:AlternateContent></w:r><w:r><w:t>Selesai</w:t></w:r></w:p>
  <w:sectPr/>
</w:body>
</w:document>)";

    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("spesifikasi.docx"));
    QVERIFY(writeZip(path, {{QStringLiteral("word/document.xml"), document}, {QStringLiteral("word/styles.xml"), styles}}));
    QVERIFY(DocumentText::check(path));

    QString error;
    const QString text = DocumentText::extract(path, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    // Teks yang dihapus lewat track changes dan salinan mc:Fallback tidak ikut
    QCOMPARE(text, QStringLiteral("# Spesifikasi Login\n"
                                  "Halaman login\tbaru\n"
                                  "baris dua\n"
                                  "\n"
                                  "## Kriteria\n"
                                  "- Email wajib\n"
                                  "  - Format valid\n"
                                  "\n"
                                  "| Field | Aturan |\n"
                                  "| --- | --- |\n"
                                  "| Password | min 8 \\| huruf angka |\n"
                                  "\n"
                                  "Catatan kotak\n"
                                  "Selesai"));
}

void TestSwarm::documentTextReadsXlsx() {
    const QByteArray workbook = R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<workbook xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships">
  <workbookPr/>
  <sheets>
    <sheet name="Data" sheetId="1" r:id="rId1"/>
    <sheet name="Rahasia" sheetId="2" state="hidden" r:id="rId2"/>
    <sheet name="Grafik" sheetId="3" r:id="rId3"/>
  </sheets>
</workbook>)";
    // Target relatif terhadap xl/ atau absolut dari akar arsip; chartsheet bukan lembar data
    const QByteArray rels = R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet1.xml"/>
  <Relationship Id="rId2" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="/xl/worksheets/sheet2.xml"/>
  <Relationship Id="rId3" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/chartsheet" Target="chartsheets/sheet1.xml"/>
</Relationships>)";
    const QByteArray shared = R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<sst xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main">
  <si><t>Nama</t></si>
  <si><t>Tanggal</t></si>
  <si><r><t>Budi</t></r><r><rPr><b/></rPr><t xml:space="preserve"> Santoso</t></r><rPh><t>budi</t></rPh></si>
  <si><t>Kota, "Besar"</t></si>
</sst>)";
    // cellXfs: 0 General, 1 tanggal bawaan (14), 2 tanggal buatan sendiri, 3 angka (#,##0.00),
    // 4 angka dengan teks berkutip yang memuat huruf "h". cellStyleXfs sengaja bertanggal: bukan gaya sel.
    const QByteArray styles = R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<styleSheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main">
  <numFmts count="2"><numFmt numFmtId="164" formatCode="dd/mm/yyyy;@"/><numFmt numFmtId="165" formatCode="0&quot; hari&quot;"/></numFmts>
  <cellStyleXfs count="1"><xf numFmtId="14"/></cellStyleXfs>
  <cellXfs count="5"><xf numFmtId="0"/><xf numFmtId="14"/><xf numFmtId="164"/><xf numFmtId="4"/><xf numFmtId="165"/></cellXfs>
</styleSheet>)";
    const QByteArray sheet1 = R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<worksheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main"><sheetData>
  <row r="1"><c r="A1" t="s"><v>0</v></c><c r="B1" t="s"><v>1</v></c><c r="C1" t="inlineStr"><is><t>Aktif</t></is></c><c r="D1" t="inlineStr"><is><t>Total</t></is></c></row>
  <row r="2"><c r="A2" t="s"><v>2</v></c><c r="B2" s="1"><v>46293</v></c><c r="C2" t="b"><v>1</v></c><c r="D2" s="3"><f>SUM(E2:F2)</f><v>1234.5</v></c></row>
  <row r="3"><c r="A3" t="s"><v>3</v></c><c r="B3" s="2"><v>46293.5625</v></c><c r="D3" t="e"><v>#DIV/0!</v></c></row>
  <row r="4"><c r="A4" s="0"/></row>
  <row r="6"><c r="C6"><v>7</v></c><c r="D6" s="4"><v>5</v></c></row>
</sheetData></worksheet>)";
    const QByteArray sheet2 = R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<worksheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main"><sheetData>
  <row r="1"><c r="A1" t="inlineStr"><is><t>kode</t></is></c><c r="B1"><v>42</v></c></row>
</sheetData></worksheet>)";

    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("laporan.xlsx"));
    QVERIFY(writeZip(path, {{QStringLiteral("xl/workbook.xml"), workbook},
                            {QStringLiteral("xl/_rels/workbook.xml.rels"), rels},
                            {QStringLiteral("xl/sharedStrings.xml"), shared},
                            {QStringLiteral("xl/styles.xml"), styles},
                            {QStringLiteral("xl/worksheets/sheet1.xml"), sheet1},
                            {QStringLiteral("xl/worksheets/sheet2.xml"), sheet2}}));

    QString error;
    const QString text = DocumentText::extract(path, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    // Seri 46293 = 28 September 2026; kolom yang terlewat jadi sel kosong; teks fonetik dibuang;
    // rumus diwakili hasilnya; baris tanpa isi dilewati
    QCOMPARE(text, QStringLiteral("[Sheet: Data]\n"
                                  "Nama,Tanggal,Aktif,Total\n"
                                  "Budi Santoso,2026-09-28,TRUE,1234.5\n"
                                  "\"Kota, \"\"Besar\"\"\",2026-09-28 13:30,,#DIV/0!\n"
                                  ",,7,5\n"
                                  "\n"
                                  "[Sheet: Rahasia (tersembunyi)]\n"
                                  "kode,42"));

    // Workbook bertanggal 1904 (Excel Mac lama): seri 0 = 1 Januari 1904
    const QString mac = dir.filePath(QStringLiteral("mac.xlsx"));
    QByteArray workbook1904 = workbook;
    workbook1904.replace("<workbookPr/>", "<workbookPr date1904=\"1\"/>");
    const QByteArray sheetDate = R"(<worksheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main"><sheetData>
  <row r="1"><c r="A1" s="1"><v>0</v></c><c r="B1" s="1"><v>0.25</v></c></row></sheetData></worksheet>)";
    QVERIFY(writeZip(mac, {{QStringLiteral("xl/workbook.xml"), workbook1904},
                           {QStringLiteral("xl/_rels/workbook.xml.rels"), rels},
                           {QStringLiteral("xl/styles.xml"), styles},
                           {QStringLiteral("xl/worksheets/sheet1.xml"), sheetDate},
                           {QStringLiteral("xl/worksheets/sheet2.xml"), sheet2}}));
    QVERIFY(DocumentText::extract(mac).startsWith(QStringLiteral("[Sheet: Data]\n1904-01-01,06:00\n")));
}

void TestSwarm::documentTextRejectsUnsupportedFiles() {
    QTemporaryDir dir;
    auto problem = [](const QString &path) {
        QString error;
        const bool ok = DocumentText::check(path, &error);
        // extract() menolak dengan alasan yang sama
        QString extractError;
        const QString text = DocumentText::extract(path, &extractError);
        return ok || !text.isEmpty() || extractError != error ? QStringLiteral("<lolos>") : error;
    };

    const QString xls = dir.filePath(QStringLiteral("lama.xls"));
    QVERIFY(writeFile(xls, "x"));
    QCOMPARE(problem(xls), QStringLiteral("Format lama .xls tidak didukung; simpan ulang sebagai .xlsx"));
    const QString doc = dir.filePath(QStringLiteral("lama.doc"));
    QVERIFY(writeFile(doc, "x"));
    QCOMPARE(problem(doc), QStringLiteral("Format lama .doc tidak didukung; simpan ulang sebagai .docx"));

    const QString txt = dir.filePath(QStringLiteral("catatan.txt"));
    QVERIFY(writeFile(txt, "x"));
    QVERIFY(problem(txt).startsWith(QStringLiteral("Hanya file Excel")));
    QVERIFY(!DocumentText::isSupported(txt));
    QVERIFY(DocumentText::isSupported(QStringLiteral("Laporan.XLSX")));

    QCOMPARE(problem(dir.filePath(QStringLiteral("hilang.csv"))), QStringLiteral("File tidak ditemukan"));

    const QString fake = dir.filePath(QStringLiteral("palsu.xlsx"));
    QVERIFY(writeFile(fake, "bukan arsip zip"));
    QCOMPARE(problem(fake), QStringLiteral("Bukan file Excel yang valid"));

    // File Office berpassword disimpan sebagai compound file (OLE), bukan ZIP
    const QString locked = dir.filePath(QStringLiteral("rahasia.docx"));
    QVERIFY(writeFile(locked, QByteArray::fromHex("d0cf11e0a1b11ae1") + QByteArray(512, '\0')));
    QVERIFY(problem(locked).contains(QStringLiteral("berpassword")));

    const QString empty = dir.filePath(QStringLiteral("kosong.xlsx"));
    QVERIFY(writeZip(empty, {{QStringLiteral("[Content_Types].xml"), QByteArray("<Types/>")}}));
    QVERIFY(problem(empty).contains(QStringLiteral("xl/workbook.xml")));
}

void TestSwarm::attachmentNamesAreSafeAndUnique() {
    using TaskAttachments::uniqueName;
    QCOMPARE(uniqueName(QStringLiteral("data.csv"), {}), QStringLiteral("data.csv"));
    // Tanpa beda huruf besar/kecil, seperti sistem file Windows
    QCOMPARE(uniqueName(QStringLiteral("data.csv"), {QStringLiteral("DATA.csv")}), QStringLiteral("data (2).csv"));
    QCOMPARE(uniqueName(QStringLiteral("data.csv"), {QStringLiteral("data.csv"), QStringLiteral("data (2).csv")}),
             QStringLiteral("data (3).csv"));
    QCOMPARE(uniqueName(QStringLiteral("a:b?c*.png"), {}), QStringLiteral("a_b_c_.png"));
    QCOMPARE(uniqueName(QStringLiteral("../../rahasia.txt"), {}), QStringLiteral(".._.._rahasia.txt"));
    QCOMPARE(uniqueName(QStringLiteral("CON.txt"), {}), QStringLiteral("lampiranCON.txt"));
    // Nama berawalan titik (mis. folder .teks) tidak pernah dipakai
    QCOMPARE(uniqueName(QStringLiteral(".teks"), {}), QStringLiteral("lampiran.teks"));
    QCOMPARE(uniqueName(QStringLiteral("titik. "), {}), QStringLiteral("titik"));
    QCOMPARE(uniqueName(QString(200, QLatin1Char('x')) + QStringLiteral(".xlsx"), {}).size(), qsizetype(105));

    QVERIFY(TaskAttachments::isImage(QStringLiteral("Foto.JPG")));
    QVERIFY(!TaskAttachments::isImage(QStringLiteral("data.csv")));
    QVERIFY(TaskAttachments::isDocument(QStringLiteral("data.csv")));
    QVERIFY(TaskAttachments::documentFilter().contains(QStringLiteral("*.xlsx")));
    QVERIFY(TaskAttachments::imageFilter().contains(QStringLiteral("*.png")));
}

void TestSwarm::attachmentsSaveAndRemoveFiles() {
    QTemporaryDir root;
    const QString source = root.filePath(QStringLiteral("asal/Data Penjualan.csv"));
    QVERIFY(writeFile(source, "produk,harga\nkopi,15000\n"));
    const QString directory = root.filePath(QStringLiteral("attachments/t1"));

    TaskAttachments::Draft photo;
    photo.fileName = QStringLiteral("tempel.png");
    photo.imageData = pngBytes(QSize(8, 8), Qt::red);
    TaskAttachments::Draft document;
    document.fileName = QStringLiteral("Data Penjualan.csv");
    document.sourcePath = source;
    TaskAttachments::Draft missing;
    missing.fileName = QStringLiteral("hilang.csv");
    missing.sourcePath = root.filePath(QStringLiteral("tidak-ada.csv"));

    // Task baru: draft baru ditulis, yang gagal dilewati dengan alasan
    QStringList errors;
    QStringList saved = TaskAttachments::save(directory, {photo, document, missing}, {}, &errors);
    QCOMPARE(saved, QStringList({"tempel.png", "Data Penjualan.csv"}));
    QCOMPARE(errors, QStringList({"hilang.csv gagal disimpan"}));
    const QDir dir(directory);
    QCOMPARE(QFile(dir.filePath(QStringLiteral("tempel.png"))).size(), qint64(photo.imageData.size()));
    QFile copied(dir.filePath(QStringLiteral("Data Penjualan.csv")));
    QVERIFY(copied.open(QIODevice::ReadOnly));
    QCOMPARE(copied.readAll(), QByteArray("produk,harga\nkopi,15000\n"));
    copied.close();

    // Edit: foto lama tetap, dokumen dihapus dari form, dokumen baru bernama sama dengan foto lama
    QVERIFY(writeFile(dir.filePath(QStringLiteral(".teks/Data Penjualan.csv.txt")), "cache"));
    TaskAttachments::Draft keep;
    keep.fileName = QStringLiteral("tempel.png");
    keep.storedPath = dir.filePath(QStringLiteral("tempel.png"));
    TaskAttachments::Draft clash = document;
    clash.fileName = QStringLiteral("TEMPEL.png");
    errors.clear();
    saved = TaskAttachments::save(directory, {keep, clash}, saved, &errors);
    QVERIFY(errors.isEmpty());
    QCOMPARE(saved, QStringList({"tempel.png", "TEMPEL (2).png"}));
    QVERIFY(!QFile::exists(dir.filePath(QStringLiteral("Data Penjualan.csv"))));
    // Cache teks lengkap dibuang; dibuat ulang pada run berikutnya
    QVERIFY(!QFileInfo::exists(dir.filePath(QStringLiteral(".teks"))));

    // Semua lampiran dihapus: folder task ikut hilang
    saved = TaskAttachments::save(directory, {}, saved, &errors);
    QVERIFY(saved.isEmpty());
    QVERIFY(!QFileInfo::exists(directory));

    // Nama dari session.json yang menunjuk ke luar folder lampiran tidak pernah dihapus
    const QString outside = root.filePath(QStringLiteral("asal/penting.txt"));
    QVERIFY(writeFile(outside, "jangan dihapus"));
    TaskAttachments::save(directory, {}, {QStringLiteral("../../asal/penting.txt")}, &errors);
    QVERIFY(QFile::exists(outside));

    // Folder kosong = tidak diketahui: tidak menulis apa pun ke folder kerja proses
    errors.clear();
    QVERIFY(TaskAttachments::save(QString(), {photo}, {}, &errors).isEmpty());
    QCOMPARE(errors, QStringList({"folder lampiran tidak diketahui"}));
    QVERIFY(!QFile::exists(QStringLiteral("tempel.png")));
}

void TestSwarm::imagesAreNormalizedForClaude() {
    using TaskAttachments::kMaxImageBytes;
    using TaskAttachments::kMaxImageSide;

    // Tangkapan layar besar: diperkecil, tetap PNG
    QImage screenshot(3200, 1600, QImage::Format_ARGB32);
    screenshot.fill(QColor(20, 40, 200, 128));
    TaskAttachments::NormalizedImage result = TaskAttachments::normalizeImage(screenshot);
    QVERIFY(result.error.isEmpty());
    QCOMPARE(result.suffix, QStringLiteral("png"));
    QCOMPARE(result.size, QSize(kMaxImageSide, kMaxImageSide / 2));
    QVERIFY(QImage::fromData(result.data).hasAlphaChannel());

    // Derau acak tidak bisa dikompres PNG: jatuh ke JPEG di bawah batas ukuran
    QImage noise(1400, 1400, QImage::Format_RGB32);
    for (int y = 0; y < noise.height(); ++y) {
        auto *line = reinterpret_cast<quint32 *>(noise.scanLine(y));
        for (int x = 0; x < noise.width(); ++x) {
            line[x] = 0xff000000u | (QRandomGenerator::global()->generate() & 0xffffffu);
        }
    }
    result = TaskAttachments::normalizeImage(noise);
    QVERIFY(result.error.isEmpty());
    QCOMPARE(result.suffix, QStringLiteral("jpg"));
    QVERIFY(result.data.size() <= kMaxImageBytes);
    QVERIFY(!QImage::fromData(result.data).isNull());

    QVERIFY(!TaskAttachments::normalizeImage(QImage()).error.isEmpty());

    QTemporaryDir dir;
    // PNG kecil disimpan apa adanya, tanpa kompresi ulang
    const QString small = dir.filePath(QStringLiteral("kecil.png"));
    const QByteArray smallBytes = pngBytes(QSize(40, 20), Qt::green);
    QVERIFY(writeFile(small, smallBytes));
    result = TaskAttachments::normalizeImageFile(small);
    QCOMPARE(result.data, smallBytes);
    QCOMPARE(result.suffix, QStringLiteral("png"));

    // Foto kamera besar (JPEG): diperkecil, tetap JPEG
    QImage photo(2400, 1800, QImage::Format_RGB32);
    photo.fill(QColor(200, 150, 100));
    const QString camera = dir.filePath(QStringLiteral("kamera.jpeg"));
    QVERIFY(writeFile(camera, imageBytes(photo, "JPEG")));
    result = TaskAttachments::normalizeImageFile(camera);
    QVERIFY(result.error.isEmpty());
    QCOMPARE(result.suffix, QStringLiteral("jpg"));
    QCOMPARE(result.size, QSize(kMaxImageSide, 1176));

    const QString broken = dir.filePath(QStringLiteral("rusak.png"));
    QVERIFY(writeFile(broken, "bukan gambar"));
    result = TaskAttachments::normalizeImageFile(broken);
    QVERIFY(result.data.isEmpty());
    QVERIFY(result.error.startsWith(QStringLiteral("Bukan gambar")));
}

void TestSwarm::materialsReadAttachmentsAndReferences() {
    QTemporaryDir root;
    const QDir attachments(root.filePath(QStringLiteral("attachments/t1")));
    QVERIFY(writeFile(attachments.filePath(QStringLiteral("foto.png")), pngBytes(QSize(4, 4), Qt::red)));
    QVERIFY(writeFile(attachments.filePath(QStringLiteral("data.csv")), "produk,harga\nkopi,15000\n"));
    QByteArray big;
    for (int i = 0; i < 3000; ++i) {
        big += QByteArray("baris-") + QByteArray::number(i).rightJustified(4, '0') + QByteArray(",isi-data-panjang\n");
    }
    QVERIFY(writeFile(attachments.filePath(QStringLiteral("besar.csv")), big));
    QVERIFY(writeFile(attachments.filePath(QStringLiteral("rusak.xlsx")), "bukan zip"));
    const QString reference = root.filePath(QStringLiteral("referensi"));
    QVERIFY(QDir().mkpath(reference));

    TaskItem task = makeTask(QStringLiteral("t1"), QStringLiteral("CODER"));
    task.attachments = QStringList({"foto.png", "data.csv", "besar.csv", "rusak.xlsx", "hilang.docx"});
    const TaskMaterials materials = TaskAttachments::materials(
        task, attachments.path(), {reference, root.filePath(QStringLiteral("sudah-dihapus"))});

    QCOMPARE(materials.imagePaths, QStringList({QDir::toNativeSeparators(attachments.filePath(QStringLiteral("foto.png")))}));
    QCOMPARE(int(materials.documents.size()), 3);

    const TaskMaterials::Document &data = materials.documents.at(0);
    QCOMPARE(data.fileName, QStringLiteral("data.csv"));
    QCOMPARE(data.path, QDir::toNativeSeparators(attachments.filePath(QStringLiteral("data.csv"))));
    QCOMPARE(data.text, QStringLiteral("produk,harga\nkopi,15000\n"));
    QCOMPARE(data.omittedChars, qsizetype(0));
    QVERIFY(data.fullTextPath.isEmpty());

    // Dokumen panjang dipotong di akhir baris; teks lengkapnya disimpan untuk dibaca dengan Read
    const TaskMaterials::Document &large = materials.documents.at(1);
    QVERIFY(large.text.size() <= TaskAttachments::kDocumentPromptChars);
    QVERIFY(large.text.endsWith(QStringLiteral(",isi-data-panjang")));
    QCOMPARE(large.text.size() + large.omittedChars, qsizetype(big.size()));
    QFile full(large.fullTextPath);
    QVERIFY2(full.open(QIODevice::ReadOnly), qPrintable(large.fullTextPath));
    QCOMPARE(full.readAll(), big);

    const TaskMaterials::Document &broken = materials.documents.at(2);
    QCOMPARE(broken.error, QStringLiteral("Bukan file Excel yang valid"));
    QVERIFY(broken.text.isEmpty());

    QCOMPARE(materials.referenceDirectories, QStringList({QDir::toNativeSeparators(reference)}));
    QCOMPARE(materials.attachmentDirectory, QDir::toNativeSeparators(attachments.path()));
    QCOMPARE(materials.readableDirectories(),
             QStringList({QDir::toNativeSeparators(reference), QDir::toNativeSeparators(attachments.path())}));
    QCOMPARE(int(materials.warnings.size()), 3);
    QVERIFY(materials.warnings.at(0).startsWith(QStringLiteral("folder referensi tidak ditemukan")));
    QCOMPARE(materials.warnings.at(1), QStringLiteral("lampiran rusak.xlsx tidak bisa dibaca: Bukan file Excel yang valid"));
    QCOMPARE(materials.warnings.at(2), QStringLiteral("lampiran hilang.docx tidak ditemukan, dilewati"));

    // Task tanpa lampiran: folder lampirannya tidak ikut dibuka untuk agent
    const TaskMaterials plain = TaskAttachments::materials(makeTask(QStringLiteral("t2"), QStringLiteral("CODER")),
                                                           attachments.path(), {});
    QVERIFY(plain.attachmentDirectory.isEmpty());
    QVERIFY(plain.readableDirectories().isEmpty());
}

void TestSwarm::composerAddsInstructionsAttachmentsAndReferences() {
    TaskItem task = makeTask(QStringLiteral("1"), QStringLiteral("CODER"));
    task.subtext = QStringLiteral("  Buat halaman login seperti foto.\nWarna mengikuti data.csv.  ");
    StageRun spec = stageRun(QStringLiteral("SPECIFIER"), true, QStringLiteral("Spek"));
    spec.decision = ReviewDecision::Approved;
    task.runs = {spec};

    TaskMaterials materials;
    materials.imagePaths = QStringList({QStringLiteral("C:\\lampiran\\mockup.png"), QStringLiteral("C:\\lampiran\\warna.jpg")});
    TaskMaterials::Document csv;
    csv.fileName = QStringLiteral("data.csv");
    csv.path = QStringLiteral("C:\\lampiran\\data.csv");
    csv.text = QStringLiteral("a,b\n1,2");
    TaskMaterials::Document word;
    word.fileName = QStringLiteral("spec.docx");
    word.path = QStringLiteral("C:\\lampiran\\spec.docx");
    word.text = QStringLiteral("# Judul\n```kode```");
    word.omittedChars = 10;
    word.fullTextPath = QStringLiteral("C:\\lampiran\\.teks\\spec.docx.txt");
    TaskMaterials::Document broken;
    broken.fileName = QStringLiteral("rusak.xlsx");
    broken.path = QStringLiteral("C:\\lampiran\\rusak.xlsx");
    broken.error = QStringLiteral("Bukan file Excel yang valid");
    materials.documents = {csv, word, broken};
    materials.referenceDirectories = QStringList({QStringLiteral("E:\\shared\\design-system"),
                                                  QStringLiteral("E:\\arsip (lama)")});

    const QString prompt = TaskPromptComposer().compose(task, materials);
    // Prompt multi-baris jadi bagian sendiri, bukan "Catatan:" di bagian Task
    QVERIFY(prompt.startsWith(QStringLiteral("# Task\nJudul: Task 1\nKategori: utility\n\n# Instruksi\n\n"
                                             "Buat halaman login seperti foto.\nWarna mengikuti data.csv.\n\n# Lampiran\n")));
    QVERIFY(!prompt.contains(QStringLiteral("Catatan:")));
    QVERIFY(prompt.contains(QStringLiteral("# Lampiran\n"
                                           "Foto (terlampir sebagai gambar di pesan ini): mockup.png, warna.jpg\n"
                                           "Dokumen (isi teksnya di bawah):\n"
                                           "- data.csv: `C:\\lampiran\\data.csv`\n"
                                           "- spec.docx: `C:\\lampiran\\spec.docx`\n"
                                           "- rusak.xlsx: `C:\\lampiran\\rusak.xlsx`\n"
                                           "\n## data.csv\n```csv\na,b\n1,2\n```\n")));
    // Pagar blok lebih panjang dari backtick di dalam dokumen
    QVERIFY(prompt.contains(QStringLiteral("## spec.docx\n(Dipotong: 10 karakter terakhir tidak ikut di sini; teks "
                                           "lengkapnya bisa dibaca dengan Read: `C:\\lampiran\\.teks\\spec.docx.txt`)\n"
                                           "````markdown\n# Judul\n```kode```\n````\n")));
    QVERIFY(prompt.contains(QStringLiteral("## rusak.xlsx\n(Tidak bisa dibaca: Bukan file Excel yang valid)")));
    QVERIFY(prompt.contains(QStringLiteral("# Folder referensi (hanya dibaca)\n")));
    // Path dalam kode sebaris: "(lama)" di nama folder bukan keterangan
    QVERIFY(prompt.contains(QStringLiteral("Jangan mengubah isinya.\n- `E:\\shared\\design-system`\n- `E:\\arsip (lama)`\n\n")));

    // Urutan: task, instruksi, lampiran, referensi, lalu dokumen serah-terima antar stage
    const QStringList order = {
        QStringLiteral("# Task"), QStringLiteral("# Instruksi"), QStringLiteral("# Lampiran"),
        QStringLiteral("# Folder referensi"), QStringLiteral("# Spesifikasi yang disetujui (SPECIFIER)"),
    };
    for (qsizetype i = 1; i < order.size(); ++i) {
        QVERIFY2(prompt.indexOf(order.at(i - 1)) < prompt.indexOf(order.at(i)), qPrintable(order.at(i)));
    }

    // Log konsol meringkas bagian-bagian itu; judul "# Judul" di dalam blok kode bukan bagian prompt
    AgentLaunch launch;
    launch.prompt = prompt;
    launch.workingDirectory = QStringLiteral("E:/repo");
    const QString log = RunLogFormatter::startLines(task, launch).join(QLatin1Char('\n'));
    QVERIFY(log.contains(QStringLiteral("+ Instruksi (2 baris)")));
    QVERIFY(log.contains(QStringLiteral("+ Lampiran (")));
    QVERIFY(log.contains(QStringLiteral("+ Folder referensi (hanya dibaca) (3 baris)")));
    QVERIFY(!log.contains(QStringLiteral("+ Judul")));

    // Tanpa bahan: prompt sama persis dengan sebelumnya
    task.subtext = QStringLiteral("PIC: Budi");
    task.runs.clear();
    QCOMPARE(TaskPromptComposer().compose(task, TaskMaterials()), TaskPromptComposer().compose(task));
    QVERIFY(TaskPromptComposer().compose(task).endsWith(QStringLiteral("Catatan: PIC: Budi\n")));
}

void TestSwarm::cliGrantsReadOnlyFoldersAndStreamInput() {
    QCOMPARE(ClaudeCli::readRule(QStringLiteral("E:\\File Bondan\\lib")), QStringLiteral("Read(//e/File Bondan/lib/**)"));
    QCOMPARE(ClaudeCli::readRule(QStringLiteral("C:/Proyek (lama)/src/")), QStringLiteral("Read(//c/Proyek (lama)/src/**)"));
    QCOMPARE(ClaudeCli::readRule(QStringLiteral("D:\\")), QStringLiteral("Read(//d/**)"));

    const StageCatalog catalog = StageCatalog::standard();
    AgentLaunch launch;
    launch.agent = *catalog.profile(QStringLiteral("CODER"))->agent();
    // Tanpa folder tambahan dan foto: argumen sama dengan argumen agent
    QCOMPARE(ClaudeCli::arguments(launch), ClaudeCli::arguments(launch.agent));

    launch.readableDirectories = QStringList({QStringLiteral("E:\\referensi"), QStringLiteral("C:\\lampiran\\t1")});
    QStringList args = ClaudeCli::arguments(launch);
    QCOMPARE(argValue(args, QStringLiteral("--allowedTools")),
             QStringLiteral("Bash(git *),Read(//e/referensi/**),Read(//c/lampiran/t1/**)"));
    // Bukan --add-dir: di mode acceptEdits folder itu ikut bisa diedit
    QVERIFY(!args.contains(QStringLiteral("--add-dir")));
    QVERIFY(!args.contains(QStringLiteral("--input-format")));
    QCOMPARE(argValue(args, QStringLiteral("--permission-mode")), QStringLiteral("acceptEdits"));

    launch.imagePaths = QStringList({QStringLiteral("C:\\lampiran\\t1\\foto.png")});
    args = ClaudeCli::arguments(launch);
    QCOMPARE(argValue(args, QStringLiteral("--input-format")), QStringLiteral("stream-json"));
    QCOMPARE(argValue(args, QStringLiteral("--output-format")), QStringLiteral("stream-json"));

    // Stage tanpa allowedTools bawaan tetap mendapat izin bacanya
    launch.agent = *catalog.profile(QStringLiteral("SPECIFIER"))->agent();
    QCOMPARE(argValue(ClaudeCli::arguments(launch), QStringLiteral("--allowedTools")),
             QStringLiteral("Read(//e/referensi/**),Read(//c/lampiran/t1/**)"));
}

void TestSwarm::userMessageCarriesImages() {
    QTemporaryDir dir;
    const QString png = dir.filePath(QStringLiteral("mockup.png"));
    const QByteArray pngData = pngBytes(QSize(6, 6), Qt::blue);
    QVERIFY(writeFile(png, pngData));
    const QString bmp = dir.filePath(QStringLiteral("lama.bmp"));
    QVERIFY(writeFile(bmp, "BM"));

    const QByteArray line = ClaudeCli::userMessage(QStringLiteral("# Task\nJudul: \u2713 \"kutip\"\n"),
                                                   {png, bmp, dir.filePath(QStringLiteral("hilang.jpg"))});
    // Satu baris JSON utuh per pesan
    QVERIFY(line.endsWith('\n'));
    QCOMPARE(line.count('\n'), qsizetype(1));

    const QJsonObject root = QJsonDocument::fromJson(line).object();
    QCOMPARE(root.value(QStringLiteral("type")).toString(), QStringLiteral("user"));
    const QJsonObject message = root.value(QStringLiteral("message")).toObject();
    QCOMPARE(message.value(QStringLiteral("role")).toString(), QStringLiteral("user"));
    const QJsonArray content = message.value(QStringLiteral("content")).toArray();
    QCOMPARE(int(content.size()), 5);
    QCOMPARE(content.at(0).toObject().value(QStringLiteral("text")).toString(), QStringLiteral("Foto: mockup.png"));
    const QJsonObject image = content.at(1).toObject();
    QCOMPARE(image.value(QStringLiteral("type")).toString(), QStringLiteral("image"));
    const QJsonObject source = image.value(QStringLiteral("source")).toObject();
    QCOMPARE(source.value(QStringLiteral("type")).toString(), QStringLiteral("base64"));
    QCOMPARE(source.value(QStringLiteral("media_type")).toString(), QStringLiteral("image/png"));
    QCOMPARE(QByteArray::fromBase64(source.value(QStringLiteral("data")).toString().toLatin1()), pngData);
    // Format yang tidak diterima Claude dan file yang hilang diganti catatan, bukan membatalkan run
    QCOMPARE(content.at(2).toObject().value(QStringLiteral("text")).toString(), QStringLiteral("(Foto lama.bmp tidak bisa dibaca)"));
    QCOMPARE(content.at(3).toObject().value(QStringLiteral("text")).toString(), QStringLiteral("(Foto hilang.jpg tidak bisa dibaca)"));
    QCOMPARE(content.at(4).toObject().value(QStringLiteral("text")).toString(),
             QStringLiteral("# Task\nJudul: \u2713 \"kutip\"\n"));
}

void TestSwarm::claudeSessionSendsImagesAsStreamJson() {
    QTemporaryDir dir;
    const QString echoPath = dir.filePath(QStringLiteral("echo.json"));
    const FakeClaudeMode mode(QStringLiteral("echo:") + echoPath);
    ClaudeCodeRuntime runtime(QCoreApplication::applicationFilePath());
    const QString png = dir.filePath(QStringLiteral("foto.png"));
    const QByteArray pngData = pngBytes(QSize(6, 6), Qt::red);
    QVERIFY(writeFile(png, pngData));
    AgentLaunch launch = fakeLaunch(dir.path());
    launch.imagePaths = QStringList({png});
    launch.readableDirectories = QStringList({dir.path()});

    SessionRecorder recorder;
    std::unique_ptr<AgentSession> session(runtime.createSession(launch, nullptr));
    recorder.attach(session.get());
    session->start();
    QTRY_COMPARE_WITH_TIMEOUT(int(recorder.results.size()), 1, 15000);
    QVERIFY(recorder.results.first().success);

    QFile file(echoPath);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonObject echo = QJsonDocument::fromJson(file.readAll()).object();
    QStringList args;
    const QJsonArray argArray = echo.value(QStringLiteral("args")).toArray();
    for (const QJsonValue &value : argArray) {
        args.append(value.toString());
    }
    QCOMPARE(args, ClaudeCli::arguments(launch));
    QCOMPARE(argValue(args, QStringLiteral("--input-format")), QStringLiteral("stream-json"));
    QVERIFY(argValue(args, QStringLiteral("--allowedTools")).contains(ClaudeCli::readRule(dir.path())));

    // stdin: satu pesan stream-json berisi foto lalu prompt yang sama persis
    const QString stdinText = echo.value(QStringLiteral("stdin")).toString();
    QCOMPARE(stdinText.toUtf8(), ClaudeCli::userMessage(launch.prompt, launch.imagePaths));
    const QJsonArray content = QJsonDocument::fromJson(stdinText.toUtf8()).object()
                                   .value(QStringLiteral("message")).toObject()
                                   .value(QStringLiteral("content")).toArray();
    QCOMPARE(content.last().toObject().value(QStringLiteral("text")).toString(), launch.prompt);
}

void TestSwarm::swarmPassesMaterialsToLaunch() {
    SwarmFixture f;
    TaskMaterials materials;
    materials.attachmentDirectory = QStringLiteral("C:\\lampiran\\m1");
    materials.imagePaths = QStringList({QStringLiteral("C:\\lampiran\\m1\\foto.png")});
    materials.referenceDirectories = QStringList({QStringLiteral("E:\\referensi")});

    QString reason;
    QVERIFY2(f.swarm.run(makeTask(QStringLiteral("m1"), QStringLiteral("CODER")), f.dir.path(), materials, &reason),
             qPrintable(reason));
    FakeAgentSession *session = sessionFor(f.runtime, QStringLiteral("m1"));
    QVERIFY(session);
    QCOMPARE(session->launch().imagePaths, materials.imagePaths);
    QCOMPARE(session->launch().readableDirectories,
             QStringList({QStringLiteral("E:\\referensi"), QStringLiteral("C:\\lampiran\\m1")}));
    QVERIFY(session->launch().prompt.contains(QStringLiteral("Foto (terlampir sebagai gambar di pesan ini): foto.png")));
    QVERIFY(session->launch().prompt.contains(QStringLiteral("- `E:\\referensi`")));

    // Tanpa bahan: tidak ada foto dan izin folder tambahan
    QVERIFY(f.run(QStringLiteral("m2"), QStringLiteral("SPECIFIER")));
    QVERIFY(sessionFor(f.runtime, QStringLiteral("m2"))->launch().imagePaths.isEmpty());
    QVERIFY(sessionFor(f.runtime, QStringLiteral("m2"))->launch().readableDirectories.isEmpty());
}

void TestSwarm::realClaudeReadsAttachmentsAndReferences() {
    if (qEnvironmentVariableIsEmpty("LASSOMOIR_REAL_CLAUDE")) {
        QSKIP("Set LASSOMOIR_REAL_CLAUDE=1 untuk menjalankan claude sungguhan (memakai token)");
    }

    QTemporaryDir root;
    const QString work = root.filePath(QStringLiteral("kerja"));
    const QString reference = root.filePath(QStringLiteral("referensi (bersama)"));
    const QString attachments = root.filePath(QStringLiteral("lampiran/t1"));
    QVERIFY(writeFile(QDir(work).filePath(QStringLiteral("README.md")), "# Proyek uji\n"));
    QVERIFY(writeFile(QDir(reference).filePath(QStringLiteral("kode.txt")), "kode-referensi-4821\n"));
    QVERIFY(writeFile(QDir(attachments).filePath(QStringLiteral("warna.png")), pngBytes(QSize(64, 64), QColor(220, 20, 20))));
    QVERIFY(writeFile(QDir(attachments).filePath(QStringLiteral("harga.csv")), "produk,harga\nkopi,15000\nteh,8000\n"));

    // SPECIFIER hanya punya Read/Grep/Glob: folder referensi & lampiran terbaca, tidak bisa ditulis
    TaskItem task = makeTask(QStringLiteral("real-lampiran"), QStringLiteral("SPECIFIER"));
    task.title = QStringLiteral("Uji lampiran");
    task.subtext = QStringLiteral("Jangan menulis spesifikasi. Jawab tiga baris saja:\n"
                                  "1) warna dominan foto warna.png dalam bahasa Indonesia\n"
                                  "2) harga kopi dari harga.csv\n"
                                  "3) isi file kode.txt di folder referensi (baca dengan Read)");
    task.attachments = QStringList({"warna.png", "harga.csv"});
    task.tuning.insert(QStringLiteral("SPECIFIER"), {QStringLiteral("haiku"), QStringLiteral("low")});

    const StageCatalog catalog = StageCatalog::standard();
    ClaudeCodeRuntime runtime;
    QString reason;
    QVERIFY2(runtime.isAvailable(&reason), qPrintable(reason));
    TaskPromptComposer composer;
    SwarmCoordinator swarm(catalog, runtime, composer);
    QList<AgentResult> results;
    connect(&swarm, &SwarmCoordinator::runEvent, this, [&work](const TaskItem &item, const AgentEvent &event) {
        qInfo().noquote() << RunLogFormatter::eventLine(item, event, work);
    });
    connect(&swarm, &SwarmCoordinator::runFinished, this, [&results](const TaskItem &item, const AgentResult &result) {
        qInfo().noquote() << RunLogFormatter::finishLine(item, result);
        results.append(result);
    });

    const TaskMaterials materials = TaskAttachments::materials(task, attachments, {reference});
    QVERIFY(materials.warnings.isEmpty());
    QVERIFY2(swarm.run(task, work, materials, &reason), qPrintable(reason));
    QTRY_COMPARE_WITH_TIMEOUT(int(results.size()), 1, 300000);

    const AgentResult result = results.first();
    QVERIFY2(result.success, qPrintable(result.outcome + QStringLiteral(": ") + result.message));
    QVERIFY2(result.deniedTools.isEmpty(), qPrintable(result.deniedTools.join(QStringLiteral(", "))));
    const QString answer = result.message.toLower();
    QVERIFY2(answer.contains(QStringLiteral("merah")), qPrintable(result.message));
    QVERIFY2(answer.contains(QStringLiteral("15000")) || answer.contains(QStringLiteral("15.000")), qPrintable(result.message));
    QVERIFY2(answer.contains(QStringLiteral("kode-referensi-4821")), qPrintable(result.message));
}

int main(int argc, char *argv[]) {
    // Test FileManager menulis ke lokasi khusus test, bukan AppData asli
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("LassomoirTest"));

    const QString fakeMode = qEnvironmentVariable(kFakeClaudeEnv);
    if (!fakeMode.isEmpty()) {
        return runFakeClaude(fakeMode);
    }

    TestSwarm test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_swarm.moc"

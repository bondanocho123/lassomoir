#include "ClaudeCli.h"
#include "ClaudeCodeRuntime.h"
#include "CodeMetrics.h"
#include "EdgeMermaidRenderer.h"
#include "FakeAgentRuntime.h"
#include "FileManager.h"
#include "MermaidRenderer.h"
#include "PromptComposer.h"
#include "RunLogFormatter.h"
#include "StageCatalog.h"
#include "StreamJsonParser.h"
#include "SwarmCoordinator.h"
#include "TaskItem.h"
#include "TaskManager.h"
#include "WorkspaceDiff.h"
#include "WorkspaceGuard.h"

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

    // Claude Code sungguhan; hanya jalan bila LASSOMOIR_REAL_CLAUDE=1 (memakai token)
    void realClaudeRunsCoderTask();
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

    // Form edit menyimpan pilihan lewat TaskManager tanpa menyentuh stage atau status
    TaskManager manager(f.catalog);
    manager.addTask(makeTask(QStringLiteral("m1"), QStringLiteral("CODER")));
    QVERIFY(manager.updateDetails(QStringLiteral("m1"), QStringLiteral("Baru"), QStringLiteral("bug"),
                                  QStringLiteral("catatan"), heavy.tuning));
    const std::optional<TaskItem> updated = manager.task(QStringLiteral("m1"));
    QVERIFY(updated);
    QCOMPARE(updated->title, QStringLiteral("Baru"));
    QCOMPARE(updated->stage, QStringLiteral("CODER"));
    QCOMPARE(updated->tuning.value(QStringLiteral("CODER")).model, QStringLiteral("opus"));
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

    QString error;
    {
        FileManager writer;
        writer.setWorkingDirectory(QStringLiteral("RoundTrip"), QStringLiteral("C:/repo"));
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

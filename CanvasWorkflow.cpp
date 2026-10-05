#include "CanvasWorkflow.h"
#include "DocumentText.h"
#include "TaskAttachments.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

namespace {

const QString kDefaultModel = QStringLiteral("sonnet");
const QString kDefaultEffort = QStringLiteral("medium");
constexpr int kTimeoutMs = 15 * 60 * 1000;

// Bahan ringkasan task baru lebih hemat dari prompt langkah AI: isinya ikut ke setiap run task itu
constexpr qsizetype kBriefInputChars = 12000;
constexpr qsizetype kBriefInputsChars = 40000;

// Nama lampiran dari canvas.json dipakai sebagai nama file: jangan sampai menunjuk ke luar folder lampiran
bool isPlainName(const QString &name) {
    return !name.isEmpty() && name != QLatin1String(".") && name != QLatin1String("..")
           && !name.contains(QLatin1Char('/')) && !name.contains(QLatin1Char('\\'));
}

QString decisionLabel(const StageRun &run) {
    if (run.decision == ReviewDecision::Approved) {
        return QStringLiteral("disetujui");
    }
    if (run.decision == ReviewDecision::Revise) {
        return QStringLiteral("direvisi");
    }
    if (run.decision == ReviewDecision::SentBack) {
        return QStringLiteral("dikembalikan");
    }
    return run.result.success ? QString() : QStringLiteral("gagal");
}

QString stateLabel(TaskState state) {
    switch (state) {
    case TaskState::AwaitingReview: return QStringLiteral("menunggu review");
    case TaskState::Failed: return QStringLiteral("gagal");
    case TaskState::Idle: break;
    }
    return QString();
}

// Dipotong di akhir baris supaya tabel atau daftar tidak terbelah
QString truncated(const QString &text, qsizetype limit, qsizetype *omitted = nullptr) {
    if (text.size() <= limit) {
        if (omitted) {
            *omitted = 0;
        }
        return text;
    }
    const qsizetype lineEnd = text.lastIndexOf(QLatin1Char('\n'), limit);
    const QString kept = text.left(lineEnd > limit / 2 ? lineEnd : limit);
    if (omitted) {
        *omitted = text.size() - kept.size();
    }
    return kept;
}

// Pagar blok kode yang lebih panjang dari deretan backtick mana pun di dalam isinya
QString fenceFor(const QString &text) {
    int longest = 0;
    int run = 0;
    for (const QChar c : text) {
        run = c == QLatin1Char('`') ? run + 1 : 0;
        longest = qMax(longest, run);
    }
    return QString(qMax(3, longest + 1), QLatin1Char('`'));
}

QString elided(const QString &text, qsizetype max) {
    return text.size() > max ? text.left(max - 1).trimmed() + QChar(0x2026) : text;
}

// Satu kartu sebagai bahan prompt atau ringkasan task
struct Material {
    QString heading;
    QString note;          // keterangan sumber di bawah judul
    QString body;          // kosong = tanpa isi teks (mis. foto)
    bool fenced = false;   // dokumen agent: dipagari supaya judul di dalamnya tidak tercampur struktur prompt
};

Material materialOf(const CanvasNode &node) {
    Material material;
    switch (node.kind) {
    case CanvasNodeKind::Note:
        material.heading = QStringLiteral("Catatan");
        material.body = node.text.trimmed();
        if (material.body.isEmpty()) {
            material.note = QStringLiteral("(catatan kosong)");
        }
        break;
    case CanvasNodeKind::Step: {
        const QString name = CanvasWorkflow::firstLine(node.text);
        material.heading = name.isEmpty() ? QStringLiteral("Hasil langkah AI")
                                          : QStringLiteral("Hasil langkah AI \"%1\"").arg(elided(name, 80));
        if (node.result.success && !node.result.message.trimmed().isEmpty()) {
            material.body = node.result.message.trimmed();
            material.fenced = true;
        } else {
            material.note = node.hasResult() ? QStringLiteral("(run terakhirnya tidak berhasil; belum ada hasil yang bisa dipakai)")
                                             : QStringLiteral("(belum dijalankan; belum ada hasil)");
        }
        break;
    }
    case CanvasNodeKind::Artifact: {
        material.heading = node.title.isEmpty() ? QStringLiteral("Artefak") : node.title;
        QStringList facts;
        if (!node.detail.isEmpty()) {
            facts.append(node.detail);
        }
        facts.append(QStringLiteral("project %1").arg(node.source.projectId));
        if (CanvasWorkflow::isImageReference(node)) {
            facts.append(QStringLiteral("foto, terlampir sebagai gambar di pesan ini"));
        } else {
            material.body = node.text.trimmed();
            material.fenced = true;
            if (material.body.isEmpty()) {
                facts.append(QStringLiteral("tanpa isi teks"));
            }
        }
        if (!node.available) {
            facts.append(QStringLiteral("sumbernya sudah tidak ada; ini salinan terakhir"));
        }
        material.note = facts.join(QStringLiteral(" · "));
        break;
    }
    case CanvasNodeKind::Task: {
        material.heading = QStringLiteral("Task \"%1\"").arg(node.title);
        QStringList facts;
        if (!node.detail.isEmpty()) {
            facts.append(node.detail);
        }
        facts.append(QStringLiteral("project %1").arg(node.source.projectId));
        if (!node.available) {
            facts.append(QStringLiteral("task sudah tidak ada; ini salinan terakhir"));
        }
        material.note = facts.join(QStringLiteral(" · "));
        material.body = node.text.trimmed();
        break;
    }
    }
    return material;
}

void appendMaterials(QStringList &lines, const QList<Material> &materials, const QString &prefix, bool numbered,
                     qsizetype perItem, qsizetype total) {
    qsizetype budget = total;
    for (qsizetype i = 0; i < materials.size(); ++i) {
        const Material &material = materials.at(i);
        lines.append(QString());
        lines.append(numbered ? QStringLiteral("%1 %2. %3").arg(prefix).arg(i + 1).arg(material.heading)
                              : QStringLiteral("%1 %2").arg(prefix, material.heading));
        if (!material.note.isEmpty()) {
            lines.append(material.note);
        }
        if (material.body.isEmpty()) {
            continue;
        }
        if (budget <= 0) {
            lines.append(QStringLiteral("(Isinya tidak ikut: batas panjang bahan sudah habis.)"));
            continue;
        }
        qsizetype omitted = 0;
        const QString body = truncated(material.body, qMin(perItem, budget), &omitted);
        budget -= body.size();
        if (material.fenced) {
            const QString fence = fenceFor(body);
            lines.append(fence + QStringLiteral("markdown"));
            lines.append(body);
            lines.append(fence);
        } else {
            lines.append(body);
        }
        if (omitted > 0) {
            lines.append(QStringLiteral("(Dipotong: %1 karakter terakhir tidak ikut.)").arg(omitted));
        }
    }
}

QString stringField(const QJsonObject &object, const QStringList &keys) {
    for (const QString &key : keys) {
        const QJsonValue value = object.value(key);
        if (value.isString()) {
            return value.toString();
        }
        if (value.isArray()) {
            QStringList parts;
            const QJsonArray array = value.toArray();
            for (const QJsonValue &part : array) {
                if (part.isString()) {
                    parts.append(part.toString());
                }
            }
            return parts.join(QLatin1Char('\n'));
        }
    }
    return QString();
}

QList<CanvasWorkflow::TaskProposal> proposalsFrom(const QString &json) {
    QList<CanvasWorkflow::TaskProposal> proposals;
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(json.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        return proposals;
    }
    const QJsonArray items = document.isArray() ? document.array()
                                                : document.object().value(QStringLiteral("tasks")).toArray();
    QStringList titles;
    for (const QJsonValue &item : items) {
        const QJsonObject object = item.toObject();
        CanvasWorkflow::TaskProposal proposal;
        proposal.title = stringField(object, {QStringLiteral("title"), QStringLiteral("judul")}).simplified().left(120);
        if (proposal.title.isEmpty() || titles.contains(proposal.title, Qt::CaseInsensitive)) {
            continue;
        }
        proposal.category = stringField(object, {QStringLiteral("category"), QStringLiteral("kategori")}).trimmed().toLower();
        if (proposal.category.isEmpty()) {
            proposal.category = QStringLiteral("feature");
        }
        proposal.instructions = stringField(object, {QStringLiteral("instructions"), QStringLiteral("instruksi"),
                                                     QStringLiteral("description"), QStringLiteral("deskripsi")})
                                    .trimmed();
        titles.append(proposal.title);
        proposals.append(proposal);
        if (proposals.size() == CanvasWorkflow::kMaxProposals) {
            break;
        }
    }
    return proposals;
}

}

AgentDefinition CanvasWorkflow::brainstormAgent(const QString &model, const QString &effort) {
    AgentDefinition agent;
    QFile file(QStringLiteral(":/prompts/BRAINSTORM.md"));
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        agent.rolePrompt = QString::fromUtf8(file.readAll()).trimmed();
    }
    // Hanya membaca: tidak pernah ditahan WorkspaceGuard dan aman berjalan di samping agent pipeline
    agent.tools = {QStringLiteral("Read"), QStringLiteral("Grep"), QStringLiteral("Glob")};
    agent.model = kDefaultModel;
    agent.effort = kDefaultEffort;
    agent.timeoutMs = kTimeoutMs;
    agent.applyTuning({model, effort});
    return agent;
}

QString CanvasWorkflow::documentLabel(const QString &stage) {
    static const QHash<QString, QString> labels = {
        {QStringLiteral("SPECIFIER"), QStringLiteral("Spesifikasi")},
        {QStringLiteral("CODER"), QStringLiteral("Ringkasan kode")},
        {QStringLiteral("CLEANER"), QStringLiteral("Laporan perapian")},
        {QStringLiteral("ARCHITECT"), QStringLiteral("Catatan arsitektur")},
        {QStringLiteral("HARDENER"), QStringLiteral("Laporan pengerasan")},
        {QStringLiteral("QA"), QStringLiteral("Laporan QA")},
    };
    return labels.value(stage, QStringLiteral("Hasil %1").arg(stage));
}

const StageRun *CanvasWorkflow::documentRun(const TaskItem &task, const QString &stage) {
    const StageRun *latest = nullptr;
    for (auto it = task.runs.crbegin(); it != task.runs.crend(); ++it) {
        if (it->stage != stage) {
            continue;
        }
        if (it->result.success && !it->result.message.trimmed().isEmpty()) {
            return &*it;
        }
        if (!latest) {
            latest = &*it;
        }
    }
    return latest;
}

QList<CanvasWorkflow::Artifact> CanvasWorkflow::artifactsOf(const TaskItem &task, const QStringList &stageOrder) {
    QStringList stages;
    for (const QString &stage : stageOrder) {
        if (documentRun(task, stage)) {
            stages.append(stage);
        }
    }
    for (const StageRun &run : task.runs) {
        if (!stages.contains(run.stage)) {
            stages.append(run.stage);
        }
    }

    QList<Artifact> artifacts;
    for (const QString &stage : std::as_const(stages)) {
        const StageRun *run = documentRun(task, stage);
        const QString decision = run ? decisionLabel(*run) : QString();
        Artifact artifact;
        artifact.source = {task.projectId, task.id, stage, QString()};
        artifact.label = documentLabel(stage);
        artifact.detail = decision.isEmpty() ? stage : QStringLiteral("%1 · %2").arg(stage, decision);
        artifacts.append(artifact);
    }
    for (const QString &name : task.attachments) {
        Artifact artifact;
        artifact.source = {task.projectId, task.id, QString(), name};
        artifact.label = name;
        artifact.detail = TaskAttachments::isImage(name) ? QStringLiteral("foto") : QStringLiteral("dokumen");
        artifacts.append(artifact);
    }
    return artifacts;
}

CanvasWorkflow::Reference CanvasWorkflow::resolve(const CanvasSource &source, const std::optional<TaskItem> &task,
                                                  const QString &attachmentDirectory) {
    Reference reference;
    if (!task || task->id != source.taskId) {
        return reference;
    }
    reference.available = true;

    if (!source.attachment.isEmpty()) {
        const QString &name = source.attachment;
        reference.title = name;
        const bool listed = isPlainName(name) && task->attachments.contains(name);
        const QString path = listed && !attachmentDirectory.isEmpty() ? QDir(attachmentDirectory).filePath(name) : QString();
        if (path.isEmpty() || !QFileInfo(path).isFile()) {
            reference.available = false;
            return reference;
        }
        if (TaskAttachments::isImage(name)) {
            reference.detail = QStringLiteral("foto · %1").arg(task->title);
            return reference;
        }
        reference.detail = QStringLiteral("lampiran · %1").arg(task->title);
        QString error;
        const QString text = DocumentText::extract(path, &error);
        reference.text = error.isEmpty() ? truncated(text, kSnapshotChars)
                                         : QStringLiteral("(Tidak bisa dibaca: %1)").arg(error);
        return reference;
    }

    if (!source.stage.isEmpty()) {
        reference.title = QStringLiteral("%1 · %2").arg(documentLabel(source.stage), task->title);
        const StageRun *run = documentRun(*task, source.stage);
        if (!run) {
            reference.detail = source.stage;
            reference.text = QStringLiteral("(Belum ada hasil run di %1.)").arg(source.stage);
            return reference;
        }
        const QString decision = decisionLabel(*run);
        reference.detail = decision.isEmpty() ? source.stage : QStringLiteral("%1 · %2").arg(source.stage, decision);
        reference.text = truncated(run->result.message.trimmed(), kSnapshotChars);
        return reference;
    }

    reference.title = task->title;
    const QString state = stateLabel(task->state);
    reference.detail = state.isEmpty() ? task->stage : QStringLiteral("%1 · %2").arg(task->stage, state);
    QStringList facts;
    if (!task->category.isEmpty()) {
        facts.append(QStringLiteral("Kategori: %1").arg(task->category));
    }
    facts.append(QStringLiteral("Stage: %1").arg(task->stage));
    if (!state.isEmpty()) {
        facts.append(QStringLiteral("Status: %1").arg(state));
    }
    QStringList lines = {facts.join(QStringLiteral(" · "))};
    const QString instructions = task->subtext.trimmed();
    if (!instructions.isEmpty()) {
        lines.append(QString());
        lines.append(truncated(instructions, 4000));
    }
    if (!task->runs.isEmpty() && !task->runs.constLast().result.message.trimmed().isEmpty()) {
        const StageRun &last = task->runs.constLast();
        const QString decision = decisionLabel(last);
        lines.append(QString());
        lines.append(QStringLiteral("Hasil run terakhir (%1%2):")
                         .arg(last.stage, decision.isEmpty() ? QString() : QStringLiteral(" · ") + decision));
        lines.append(truncated(last.result.message.trimmed(), 8000));
    }
    reference.text = lines.join(QLatin1Char('\n'));
    return reference;
}

bool CanvasWorkflow::isImageReference(const CanvasNode &node) {
    return node.kind == CanvasNodeKind::Artifact && !node.source.attachment.isEmpty()
           && TaskAttachments::isImage(node.source.attachment);
}

QString CanvasWorkflow::firstLine(const QString &text) {
    static const QRegularExpression marker(QStringLiteral(R"(^\s*(?:(?:#{1,6}|>|[-*+]|\d+[.)])\s+)*)"));
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const QString &raw : lines) {
        QString line = raw.trimmed();
        line.remove(marker);
        line.remove(QStringLiteral("**"));
        line = line.trimmed();
        if (!line.isEmpty() && !line.startsWith(QLatin1String("```"))) {
            return line;
        }
    }
    return QString();
}

QString CanvasWorkflow::stepProblem(const CanvasBoard &board, const QString &stepId) {
    const CanvasNode *step = board.node(stepId);
    if (!step || step->kind != CanvasNodeKind::Step) {
        return QStringLiteral("langkah AI tidak ditemukan");
    }
    if (step->text.trimmed().isEmpty() && board.inputsOf(stepId).isEmpty()) {
        return QStringLiteral("tulis instruksi atau sambungkan kartu bahan ke langkah ini dulu");
    }
    return QString();
}

QString CanvasWorkflow::stepPrompt(const CanvasBoard &board, const QString &stepId) {
    const CanvasNode *step = board.node(stepId);
    if (!step) {
        return QString();
    }
    const QString instruction = step->text.trimmed();
    QStringList lines = {
        QStringLiteral("# Langkah brainstorming"),
        QString(),
        instruction.isEmpty() ? QStringLiteral("Kembangkan, kelompokkan, dan tajamkan ide dari bahan di bawah.") : instruction,
    };

    QList<Material> materials;
    const QStringList inputs = board.inputsOf(stepId);
    for (const QString &id : inputs) {
        if (const CanvasNode *input = board.node(id)) {
            materials.append(materialOf(*input));
        }
    }
    lines.append(QString());
    lines.append(QStringLiteral("# Bahan dari kanvas"));
    lines.append(materials.isEmpty()
                     ? QStringLiteral("Tidak ada kartu yang tersambung ke langkah ini; kerjakan dari instruksi saja.")
                     : QStringLiteral("%1 kartu tersambung ke langkah ini, urut sesuai garisnya dibuat.").arg(materials.size()));
    appendMaterials(lines, materials, QStringLiteral("##"), true, kInputChars, kInputsChars);

    lines.append(QString());
    lines.append(QStringLiteral("# Bentuk jawaban"));
    if (step->output == CanvasStepOutput::Tasks) {
        lines.append(QStringLiteral("Pecah hasilnya menjadi task yang bisa dikerjakan pipeline, paling banyak %1 task. "
                                    "Tiap task harus bisa dikerjakan tanpa membaca kanvas ini: tulis konteks penting "
                                    "dari bahan di atas di dalam instruksinya.")
                         .arg(kMaxProposals));
        lines.append(QStringLiteral("Tutup jawabanmu dengan tepat satu blok ```json berisi array objek dengan field:"));
        lines.append(QStringLiteral("- \"title\": judul task singkat, paling banyak 60 karakter;"));
        lines.append(QStringLiteral("- \"category\": salah satu dari component, utility, design, bug, feature;"));
        lines.append(QStringLiteral("- \"instructions\": instruksi untuk agent dalam Markdown: tujuan, batasan, dan "
                                    "kriteria selesai yang bisa diuji."));
    } else {
        lines.append(QStringLiteral("Jawab dalam Markdown berbahasa Indonesia yang ringkas dan terstruktur (judul, "
                                    "daftar, tabel bila perlu). Jawaban akhirmu langsung menjadi isi kartu hasil di kanvas."));
    }
    return lines.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

QString CanvasWorkflow::suggestedTitle(const CanvasNode &node) {
    QString title;
    switch (node.kind) {
    case CanvasNodeKind::Note:
        title = firstLine(node.text);
        break;
    case CanvasNodeKind::Step:
        title = node.result.success ? firstLine(node.result.message) : QString();
        if (title.isEmpty()) {
            title = firstLine(node.text);
        }
        break;
    case CanvasNodeKind::Artifact:
    case CanvasNodeKind::Task:
        title = node.title;
        break;
    }
    title = elided(title.simplified(), 80);
    return title.isEmpty() ? QStringLiteral("Task dari kanvas") : title;
}

QString CanvasWorkflow::taskBrief(const CanvasBoard &board, const QStringList &nodeIds) {
    QStringList lines;
    QStringList chosen;
    QList<Material> references;
    for (const QString &id : nodeIds) {
        const CanvasNode *node = board.node(id);
        if (!node || chosen.contains(id)) {
            continue;
        }
        chosen.append(id);
        QString body;
        if (node->kind == CanvasNodeKind::Note) {
            body = node->text.trimmed();
        } else if (node->kind == CanvasNodeKind::Step) {
            body = node->result.success && !node->result.message.trimmed().isEmpty() ? node->result.message.trimmed()
                                                                                       : node->text.trimmed();
        } else {
            references.append(materialOf(*node));
            continue;
        }
        if (!body.isEmpty()) {
            if (!lines.isEmpty()) {
                lines.append(QString());
            }
            lines.append(body);
        }
    }
    if (!references.isEmpty()) {
        appendMaterials(lines, references, QStringLiteral("##"), false, kBriefInputChars, kBriefInputsChars);
    }

    QList<Material> inputs;
    QStringList seen = chosen;
    for (const QString &id : std::as_const(chosen)) {
        const QStringList upstream = board.inputsOf(id);
        for (const QString &input : upstream) {
            const CanvasNode *node = board.node(input);
            if (!node || seen.contains(input)) {
                continue;
            }
            seen.append(input);
            inputs.append(materialOf(*node));
        }
    }
    if (!inputs.isEmpty()) {
        lines.append(QString());
        lines.append(QStringLiteral("## Bahan dari kanvas"));
        appendMaterials(lines, inputs, QStringLiteral("###"), false, kBriefInputChars, kBriefInputsChars);
    }
    return lines.join(QLatin1Char('\n')).trimmed();
}

QList<CanvasWorkflow::TaskProposal> CanvasWorkflow::parseTaskProposals(const QString &answer, QString *error) {
    // Isi blok ```json bisa memuat ``` di dalam string JSON-nya: tiap kemungkinan penutup dicoba sampai
    // isinya terbaca sebagai JSON. Blok terakhir didahulukan.
    static const QRegularExpression opening(QStringLiteral("```[ \\t]*json[ \\t]*\\r?\\n"),
                                            QRegularExpression::CaseInsensitiveOption);
    QList<qsizetype> starts;
    QRegularExpressionMatchIterator it = opening.globalMatch(answer);
    while (it.hasNext()) {
        starts.prepend(it.next().capturedEnd());
    }
    for (const qsizetype start : std::as_const(starts)) {
        for (qsizetype end = answer.indexOf(QStringLiteral("```"), start); end >= 0;
             end = answer.indexOf(QStringLiteral("```"), end + 3)) {
            const QList<TaskProposal> proposals = proposalsFrom(answer.mid(start, end - start));
            if (!proposals.isEmpty()) {
                return proposals;
            }
        }
    }
    // Tanpa blok kode: array JSON dari '[' pertama sampai ']' terakhir
    const qsizetype open = answer.indexOf(QLatin1Char('['));
    const qsizetype close = answer.lastIndexOf(QLatin1Char(']'));
    if (open >= 0 && close > open) {
        const QList<TaskProposal> proposals = proposalsFrom(answer.mid(open, close - open + 1));
        if (!proposals.isEmpty()) {
            return proposals;
        }
    }
    if (error) {
        *error = QStringLiteral("Jawaban tidak berisi blok ```json berisi daftar task (title, category, instructions).");
    }
    return {};
}

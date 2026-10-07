#include "CanvasInspector.h"
#include "CanvasItems.h"
#include "CanvasWorkflow.h"
#include "MarkdownView.h"
#include "RunLogFormatter.h"
#include "Theme.h"

#include <QComboBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

enum Page { EmptyPage, NotePage, ReferencePage, StepPage, ManyPage };

struct Choice {
    const char *value;
    const char *label;
};
const QList<Choice> kModels = {{"haiku", "Haiku"}, {"sonnet", "Sonnet"}, {"opus", "Opus"}};
const QList<Choice> kEfforts = {
    {"low", "Low"}, {"medium", "Medium"}, {"high", "High"}, {"xhigh", "XHigh"}, {"max", "Max"},
};

void fillChoices(QComboBox *combo, const QList<Choice> &choices, const QString &defaultValue) {
    QString defaultLabel = defaultValue;
    for (const Choice &choice : choices) {
        if (defaultValue == QLatin1String(choice.value)) {
            defaultLabel = QString::fromLatin1(choice.label);
        }
    }
    combo->addItem(QStringLiteral("Bawaan (%1)").arg(defaultLabel), QString());
    for (const Choice &choice : choices) {
        combo->addItem(QString::fromLatin1(choice.label), QString::fromLatin1(choice.value));
    }
}

void selectData(QComboBox *combo, const QString &value) {
    const int index = combo->findData(value);
    combo->setCurrentIndex(index >= 0 ? index : 0);
}

QPushButton *button(const QString &text, const char *name, QWidget *parent) {
    auto *result = new QPushButton(text, parent);
    result->setObjectName(QLatin1String(name));
    result->setCursor(Qt::PointingHandCursor);
    return result;
}

QLabel *sectionLabel(const QString &text, QWidget *parent) {
    auto *label = new QLabel(text, parent);
    label->setObjectName("canvasPanelTitle");
    return label;
}

QIcon swatchIcon(const QString &key) {
    QPixmap pixmap(QSize(36, 36));
    pixmap.setDevicePixelRatio(2.0);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(Theme::fill(0xd9cdbb), 1.0));
    painter.setBrush(CanvasPalette::noteFill(key));
    painter.drawRoundedRect(QRectF(1.5, 1.5, 15.0, 15.0), 4.0, 4.0);
    painter.end();
    return QIcon(pixmap);
}

QString kindLabel(const CanvasNode &node) {
    switch (node.kind) {
    case CanvasNodeKind::Note:
        return QStringLiteral("CATATAN");
    case CanvasNodeKind::Artifact:
        if (CanvasWorkflow::isImageReference(node)) {
            return QStringLiteral("FOTO LAMPIRAN");
        }
        return node.source.stage.isEmpty() ? QStringLiteral("LAMPIRAN")
                                           : QStringLiteral("ARTEFAK · %1").arg(node.source.stage);
    case CanvasNodeKind::Task:
        return QStringLiteral("TASK");
    case CanvasNodeKind::Step:
        return node.output == CanvasStepOutput::Tasks ? QStringLiteral("LANGKAH AI · USULAN TASK")
                                                       : QStringLiteral("LANGKAH AI");
    }
    return QString();
}

QString resultStatus(const CanvasNode &node, RunState state) {
    if (state == RunState::Running) {
        return QStringLiteral("Berjalan… keluaran agent tampil di bawah.");
    }
    if (state == RunState::Queued) {
        return QStringLiteral("Antre: menunggu giliran atau langkah hulunya selesai.");
    }
    if (!node.hasResult()) {
        return QStringLiteral("Belum dijalankan.");
    }
    QStringList parts = {node.result.success ? QStringLiteral("Selesai")
                                             : QStringLiteral("Gagal (%1)").arg(node.result.outcome)};
    if (node.finishedAt.isValid()) {
        parts.append(node.finishedAt.toLocalTime().toString(QStringLiteral("d MMM HH:mm")));
    }
    if (node.result.durationMs > 0) {
        parts.append(RunLogFormatter::formatDuration(node.result.durationMs));
    }
    if (node.result.totalTokens > 0) {
        parts.append(QStringLiteral("%1 tok").arg(RunLogFormatter::formatTokens(node.result.totalTokens)));
    }
    if (node.result.costUsd > 0) {
        parts.append(QStringLiteral("$%1").arg(node.result.costUsd, 0, 'f', 2));
    }
    return parts.join(QStringLiteral(" · "));
}

void repolish(QWidget *widget) {
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
}

}

CanvasInspector::CanvasInspector(MermaidRenderer *renderer, QWidget *parent) : QWidget(parent), m_renderer(renderer) {
    setObjectName("canvasInspector");
    setAttribute(Qt::WA_StyledBackground, true);

    m_kind = new QLabel(this);
    m_kind->setObjectName("canvasPanelTitle");
    m_preview = button(QStringLiteral("Pratinjau"), "btnCanvasPreview", this);
    m_preview->setIcon(Theme::icon(QStringLiteral(":/icons/preview.svg")));
    m_preview->setIconSize(QSize(14, 14));
    m_preview->setCheckable(true);
    // Tidak mengambil fokus: editor catatan tetap bisa diketik, pratinjaunya mengikuti
    m_preview->setFocusPolicy(Qt::NoFocus);
    m_preview->setToolTip(QStringLiteral("Pratinjau Markdown: isi kartu ini dalam bentuk jadi (judul, daftar, tabel, "
                                         "kode, diagram) di drawer di samping panel"));
    connect(m_preview, &QPushButton::clicked, this, &CanvasInspector::previewToggled);
    m_preview->hide();
    m_previewTimer.setSingleShot(true);
    m_previewTimer.setInterval(200);
    connect(&m_previewTimer, &QTimer::timeout, this, &CanvasInspector::previewChanged);
    m_title = new QLabel(this);
    m_title->setObjectName("canvasInspectorTitle");
    m_title->setWordWrap(true);
    m_meta = new QLabel(this);
    m_meta->setObjectName("canvasPanelHint");
    m_meta->setWordWrap(true);
    m_pages = new QStackedWidget(this);

    // Petunjuk saat belum ada yang dipilih
    auto *emptyPage = new QWidget(m_pages);
    m_tips = new QLabel(emptyPage);
    m_tips->setObjectName("canvasInspectorTips");
    m_tips->setWordWrap(true);
    m_tips->setTextFormat(Qt::RichText);
    m_stats = new QLabel(emptyPage);
    m_stats->setObjectName("canvasPanelHint");
    auto *emptyLayout = new QVBoxLayout(emptyPage);
    emptyLayout->setContentsMargins(0, 0, 0, 0);
    emptyLayout->setSpacing(10);
    emptyLayout->addWidget(m_tips);
    emptyLayout->addWidget(m_stats);
    emptyLayout->addStretch(1);
    m_pages->addWidget(emptyPage);

    // Catatan
    auto *notePage = new QWidget(m_pages);
    m_noteText = new QPlainTextEdit(notePage);
    m_noteText->setObjectName("canvasInspectorText");
    m_noteText->setPlaceholderText(QStringLiteral("Tulis ide, pertanyaan, atau keputusan…"));
    m_noteText->installEventFilter(this);
    connect(m_noteText, &QPlainTextEdit::textChanged, &m_previewTimer, qOverload<>(&QTimer::start));
    auto *swatchRow = new QHBoxLayout();
    swatchRow->setSpacing(4);
    const QStringList colors = CanvasPalette::noteColors();
    for (const QString &key : colors) {
        auto *swatch = new QToolButton(notePage);
        swatch->setObjectName("canvasSwatch");
        swatch->setCheckable(true);
        swatch->setIcon(swatchIcon(key));
        swatch->setIconSize(QSize(18, 18));
        swatch->setToolTip(CanvasPalette::noteColorLabel(key));
        swatch->setCursor(Qt::PointingHandCursor);
        swatch->setProperty("colorKey", key);
        connect(swatch, &QToolButton::clicked, this, [this, key]() {
            if (!m_node.id.isEmpty()) {
                emit colorChosen(m_node.id, key);
            }
        });
        m_swatches.append(swatch);
        swatchRow->addWidget(swatch);
    }
    swatchRow->addStretch(1);
    auto *noteToTask = button(QStringLiteral("Jadikan task…"), "btnCanvasSecondary", notePage);
    noteToTask->setToolTip(QStringLiteral("Buat task baru di pipeline dari catatan ini dan bahan yang tersambung ke sana"));
    connect(noteToTask, &QPushButton::clicked, this, [this]() { emit createTaskRequested({m_node.id}); });
    auto *noteLayout = new QVBoxLayout(notePage);
    noteLayout->setContentsMargins(0, 0, 0, 0);
    noteLayout->setSpacing(8);
    noteLayout->addWidget(m_noteText, 1);
    noteLayout->addWidget(sectionLabel(QStringLiteral("WARNA KERTAS"), notePage));
    noteLayout->addLayout(swatchRow);
    noteLayout->addWidget(noteToTask, 0, Qt::AlignLeft);
    m_pages->addWidget(notePage);

    // Artefak & task
    auto *referencePage = new QWidget(m_pages);
    m_referenceMissing = new QLabel(QStringLiteral("Sumbernya sudah tidak ada (task dihapus atau project ditutup). "
                                                   "Yang tampil adalah salinan terakhir, dan salinan inilah yang dibaca agent."),
                                    referencePage);
    m_referenceMissing->setObjectName("canvasInspectorWarning");
    m_referenceMissing->setWordWrap(true);
    m_referenceView = new MarkdownView(renderer, referencePage);
    m_openTask = button(QStringLiteral("Buka task di board"), "btnCanvasSecondary", referencePage);
    connect(m_openTask, &QPushButton::clicked, this, [this]() { emit openTaskRequested(m_node.source.taskId); });
    auto *referenceToTask = button(QStringLiteral("Jadikan task…"), "btnCanvasSecondary", referencePage);
    connect(referenceToTask, &QPushButton::clicked, this, [this]() { emit createTaskRequested({m_node.id}); });
    auto *referenceActions = new QHBoxLayout();
    referenceActions->addWidget(m_openTask);
    referenceActions->addWidget(referenceToTask);
    referenceActions->addStretch(1);
    auto *referenceLayout = new QVBoxLayout(referencePage);
    referenceLayout->setContentsMargins(0, 0, 0, 0);
    referenceLayout->setSpacing(8);
    referenceLayout->addWidget(m_referenceMissing);
    referenceLayout->addWidget(m_referenceView, 1);
    referenceLayout->addLayout(referenceActions);
    m_pages->addWidget(referencePage);

    // Langkah AI
    auto *stepPage = new QWidget(m_pages);
    m_stepText = new QPlainTextEdit(stepPage);
    m_stepText->setObjectName("canvasInspectorText");
    m_stepText->setPlaceholderText(QStringLiteral("Apa yang harus dikerjakan agent dengan bahan yang tersambung?\n"
                                                  "Mis. \"Bandingkan kedua spesifikasi ini, cari celah dan risikonya\""));
    m_stepText->setMaximumHeight(110);
    m_stepText->installEventFilter(this);
    m_stepModel = new QComboBox(stepPage);
    m_stepModel->setObjectName("canvasInspectorCombo");
    const AgentDefinition defaults = CanvasWorkflow::brainstormAgent();
    fillChoices(m_stepModel, kModels, defaults.model);
    m_stepEffort = new QComboBox(stepPage);
    m_stepEffort->setObjectName("canvasInspectorCombo");
    fillChoices(m_stepEffort, kEfforts, defaults.effort);
    m_stepOutput = new QComboBox(stepPage);
    m_stepOutput->setObjectName("canvasInspectorCombo");
    m_stepOutput->addItem(QStringLiteral("Keluaran: dokumen Markdown"));
    m_stepOutput->addItem(QStringLiteral("Keluaran: usulan task untuk pipeline"));
    for (QComboBox *combo : {m_stepModel, m_stepEffort, m_stepOutput}) {
        connect(combo, &QComboBox::currentIndexChanged, this, &CanvasInspector::emitStepOptions);
    }
    auto *tuningRow = new QHBoxLayout();
    tuningRow->setSpacing(6);
    tuningRow->addWidget(m_stepModel, 1);
    tuningRow->addWidget(m_stepEffort, 1);

    m_stepRun = button(QStringLiteral("▶ Jalankan"), "btnCanvasRunStep", stepPage);
    connect(m_stepRun, &QPushButton::clicked, this, [this]() {
        if (m_node.id.isEmpty()) {
            return;
        }
        commitText();
        if (m_state != RunState::Idle) {
            emit cancelRequested(m_node.id);
        } else {
            emit runRequested(m_node.id, false);
        }
    });
    m_stepRunUpstream = button(QStringLiteral("Jalankan + hulunya"), "btnCanvasSecondary", stepPage);
    m_stepRunUpstream->setToolTip(QStringLiteral("Jalankan dulu langkah AI yang hasilnya menjadi bahan langkah ini, berurutan"));
    connect(m_stepRunUpstream, &QPushButton::clicked, this, [this]() {
        commitText();
        emit runRequested(m_node.id, true);
    });
    auto *runRow = new QHBoxLayout();
    runRow->setSpacing(6);
    runRow->addWidget(m_stepRun);
    runRow->addWidget(m_stepRunUpstream);
    runRow->addStretch(1);
    m_stepStatus = new QLabel(stepPage);
    m_stepStatus->setObjectName("canvasStepStatus");
    m_stepStatus->setWordWrap(true);
    m_stepResult = new MarkdownView(renderer, stepPage);
    m_stepToNote = button(QStringLiteral("Jadikan catatan"), "btnCanvasSecondary", stepPage);
    m_stepToNote->setToolTip(QStringLiteral("Salin hasil ke catatan baru yang bisa diedit, tersambung dari langkah ini"));
    connect(m_stepToNote, &QPushButton::clicked, this, [this]() { emit noteFromResultRequested(m_node.id); });
    auto *stepToTask = button(QStringLiteral("Jadikan task…"), "btnCanvasSecondary", stepPage);
    connect(stepToTask, &QPushButton::clicked, this, [this]() { emit createTaskRequested({m_node.id}); });
    m_stepProposals = button(QStringLiteral("Buat task usulan"), "btnCanvasPrimary", stepPage);
    m_stepProposals->setToolTip(QStringLiteral("Buat semua task dari blok json hasil langkah ini di stage WAITING; "
                                               "kartunya ditaruh di kanvas dan tersambung dari langkah ini"));
    connect(m_stepProposals, &QPushButton::clicked, this, [this]() { emit proposalsRequested(m_node.id); });
    auto *resultActions = new QHBoxLayout();
    resultActions->setSpacing(6);
    resultActions->addWidget(m_stepToNote);
    resultActions->addWidget(stepToTask);
    resultActions->addStretch(1);
    auto *stepLayout = new QVBoxLayout(stepPage);
    stepLayout->setContentsMargins(0, 0, 0, 0);
    stepLayout->setSpacing(6);
    stepLayout->addWidget(sectionLabel(QStringLiteral("INSTRUKSI"), stepPage));
    stepLayout->addWidget(m_stepText);
    stepLayout->addLayout(tuningRow);
    stepLayout->addWidget(m_stepOutput);
    stepLayout->addLayout(runRow);
    stepLayout->addWidget(m_stepStatus);
    stepLayout->addWidget(sectionLabel(QStringLiteral("HASIL"), stepPage));
    stepLayout->addWidget(m_stepResult, 1);
    stepLayout->addLayout(resultActions);
    stepLayout->addWidget(m_stepProposals);
    m_pages->addWidget(stepPage);

    // Beberapa kartu sekaligus
    auto *manyPage = new QWidget(m_pages);
    m_manySummary = new QLabel(manyPage);
    m_manySummary->setObjectName("canvasInspectorTips");
    m_manySummary->setWordWrap(true);
    auto *manyToTask = button(QStringLiteral("Jadikan satu task…"), "btnCanvasSecondary", manyPage);
    manyToTask->setToolTip(QStringLiteral("Gabungkan isi kartu-kartu ini menjadi satu task baru di pipeline"));
    connect(manyToTask, &QPushButton::clicked, this, [this]() { emit createTaskRequested(m_ids); });
    auto *manyRemove = button(QStringLiteral("Hapus kartu"), "btnCanvasDanger", manyPage);
    connect(manyRemove, &QPushButton::clicked, this, [this]() { emit removeRequested(m_ids); });
    auto *manyLayout = new QVBoxLayout(manyPage);
    manyLayout->setContentsMargins(0, 0, 0, 0);
    manyLayout->setSpacing(8);
    manyLayout->addWidget(m_manySummary);
    manyLayout->addWidget(manyToTask, 0, Qt::AlignLeft);
    manyLayout->addWidget(manyRemove, 0, Qt::AlignLeft);
    manyLayout->addStretch(1);
    m_pages->addWidget(manyPage);

    auto *kindRow = new QHBoxLayout();
    kindRow->setSpacing(6);
    kindRow->addWidget(m_kind, 1);
    kindRow->addWidget(m_preview);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(4);
    layout->addLayout(kindRow);
    layout->addWidget(m_title);
    layout->addWidget(m_meta);
    layout->addSpacing(6);
    layout->addWidget(m_pages, 1);

    showEmpty(CanvasBoard());
}

CanvasInspector::~CanvasInspector() {
    // Editor yang masih memegang fokus mendapat FocusOut saat ikut dibongkar, sesudah anggota panel ini
    // hilang: event filter-nya dilepas lebih dulu
    m_noteText->removeEventFilter(this);
    m_stepText->removeEventFilter(this);
}

void CanvasInspector::setHeader(const QString &kind, const QString &title, const QString &meta) {
    m_kind->setText(kind);
    m_title->setText(title);
    m_meta->setText(meta);
    m_meta->setVisible(!meta.isEmpty());
}

void CanvasInspector::showSelection(const CanvasBoard &board, const QStringList &ids, RunState stepState,
                                    const QString &live) {
    const CanvasNode *single = ids.size() == 1 ? board.node(ids.first()) : nullptr;
    const bool sameNode = single && single->id == m_node.id;
    if (!sameNode) {
        commitText();
    }
    m_ids = ids;
    m_state = stepState;
    if (!single) {
        m_node = CanvasNode();
        if (ids.isEmpty()) {
            showEmpty(board);
        } else {
            showMany(board, ids);
        }
    } else {
        m_node = *single;
        switch (single->kind) {
        case CanvasNodeKind::Note:
            showNote(*single, sameNode);
            break;
        case CanvasNodeKind::Artifact:
        case CanvasNodeKind::Task:
            showReference(*single);
            break;
        case CanvasNodeKind::Step:
            showStep(*single, sameNode, stepState, live);
            break;
        }
    }
    // Tanpa satu kartu tidak ada yang dipratinjau; tombolnya bertahan selama drawernya terbuka,
    // supaya tetap bisa ditutup dari sini
    m_preview->setVisible(single || m_preview->isChecked());
    emit previewChanged();
}

void CanvasInspector::showEmpty(const CanvasBoard &board) {
    m_pages->setCurrentIndex(EmptyPage);
    setHeader(QStringLiteral("KANVAS BRAINSTORM"), QStringLiteral("Belum ada kartu yang dipilih"), QString());
    m_tips->setText(Theme::html(QStringLiteral(
        "<p style='color:#3d3730'>Kumpulkan ide dan artefak dari banyak task di satu ruang tanpa batas, lalu "
        "biarkan langkah AI mengolahnya.</p>"
        "<p style='color:#6f6557; line-height:140%'>"
        "<b>Klik dua kali</b> di ruang kosong: catatan baru<br>"
        "<b>N</b> catatan · <b>L</b> langkah AI · <b>C</b> tanya di chat · <b>Del</b> hapus<br>"
        "Seret dari <b>Pustaka</b> di kiri untuk merujuk dokumen hasil stage dan lampiran task lain<br>"
        "Tarik dari titik <b>●</b> di tepi kartu ke kartu lain untuk menyambungkan. Garis yang "
        "masuk ke langkah AI menjadi bahan agent<br>"
        "<b>Spasi + seret</b> atau tombol tengah: geser · <b>Ctrl + scroll</b>: zoom · <b>Ctrl + 0</b>: "
        "tampilkan semua<br>"
        "<b>Ctrl + Z</b> / <b>Ctrl + Y</b>: urungkan / ulangi</p>")));
    int steps = 0;
    int references = 0;
    for (const CanvasNode &node : board.nodes) {
        steps += node.kind == CanvasNodeKind::Step ? 1 : 0;
        references += node.isReference() ? 1 : 0;
    }
    m_stats->setText(QStringLiteral("%1 kartu · %2 referensi · %3 langkah AI · %4 garis")
                         .arg(board.nodes.size())
                         .arg(references)
                         .arg(steps)
                         .arg(board.edges.size()));
}

void CanvasInspector::showMany(const CanvasBoard &board, const QStringList &ids) {
    m_pages->setCurrentIndex(ManyPage);
    int notes = 0;
    int references = 0;
    int steps = 0;
    for (const QString &id : ids) {
        if (const CanvasNode *node = board.node(id)) {
            notes += node->kind == CanvasNodeKind::Note ? 1 : 0;
            references += node->isReference() ? 1 : 0;
            steps += node->kind == CanvasNodeKind::Step ? 1 : 0;
        }
    }
    setHeader(QStringLiteral("PILIHAN"), QStringLiteral("%1 kartu dipilih").arg(ids.size()), QString());
    QStringList parts;
    if (notes > 0) {
        parts.append(QStringLiteral("%1 catatan").arg(notes));
    }
    if (references > 0) {
        parts.append(QStringLiteral("%1 referensi").arg(references));
    }
    if (steps > 0) {
        parts.append(QStringLiteral("%1 langkah AI").arg(steps));
    }
    m_manySummary->setText(parts.join(QStringLiteral(" · ")));
}

void CanvasInspector::showNote(const CanvasNode &node, bool sameNode) {
    m_pages->setCurrentIndex(NotePage);
    const QString first = CanvasWorkflow::suggestedTitle(node);
    setHeader(QStringLiteral("CATATAN"), node.text.trimmed().isEmpty() ? QStringLiteral("Catatan kosong") : first, QString());
    if (!sameNode || !m_noteText->hasFocus()) {
        const QSignalBlocker blocker(m_noteText);
        if (m_noteText->toPlainText() != node.text) {
            m_noteText->setPlainText(node.text);
        }
    }
    const QString color = node.color.isEmpty() ? CanvasPalette::defaultNoteColor() : node.color;
    for (QToolButton *swatch : std::as_const(m_swatches)) {
        swatch->setChecked(swatch->property("colorKey").toString() == color);
    }
}

void CanvasInspector::showReference(const CanvasNode &node) {
    m_pages->setCurrentIndex(ReferencePage);
    QStringList meta;
    if (!node.detail.isEmpty()) {
        meta.append(node.detail);
    }
    meta.append(QStringLiteral("project %1").arg(node.source.projectId));
    setHeader(kindLabel(node), node.title, meta.join(QStringLiteral(" · ")));
    m_referenceMissing->setVisible(!node.available);
    m_openTask->setEnabled(node.available);
    QString markdown;
    if (CanvasWorkflow::isImageReference(node)) {
        markdown = QStringLiteral("_Foto lampiran: tampil di kartu kanvas, dan ikut ke agent sebagai gambar bila kartu ini "
                                  "disambungkan ke langkah AI._");
    } else {
        markdown = node.text.trimmed().isEmpty() ? QStringLiteral("_Tidak ada isi teks._") : node.text;
    }
    if (m_referenceView->markdown() != markdown) {
        m_referenceView->showMarkdown(markdown);
    }
}

void CanvasInspector::showStep(const CanvasNode &node, bool sameNode, RunState state, const QString &live) {
    m_pages->setCurrentIndex(StepPage);
    const AgentDefinition agent = CanvasWorkflow::brainstormAgent(node.model, node.effort);
    // Judulnya baris pertama instruksi, sama dengan judul kartunya; instruksi lengkap ada di editor di bawah
    QString title = CanvasWorkflow::firstLine(node.text).simplified();
    if (title.size() > 90) {
        title = title.left(89).trimmed() + QChar(0x2026);
    }
    setHeader(kindLabel(node), title.isEmpty() ? QStringLiteral("Langkah AI tanpa instruksi") : title,
              QStringLiteral("%1 · %2").arg(agent.model, agent.effort));
    if (!sameNode || !m_stepText->hasFocus()) {
        const QSignalBlocker blocker(m_stepText);
        if (m_stepText->toPlainText() != node.text) {
            m_stepText->setPlainText(node.text);
        }
    }
    {
        const QSignalBlocker modelBlocker(m_stepModel);
        const QSignalBlocker effortBlocker(m_stepEffort);
        const QSignalBlocker outputBlocker(m_stepOutput);
        selectData(m_stepModel, node.model);
        selectData(m_stepEffort, node.effort);
        m_stepOutput->setCurrentIndex(node.output == CanvasStepOutput::Tasks ? 1 : 0);
    }

    const bool busy = state != RunState::Idle;
    m_stepRun->setText(busy ? QStringLiteral("■ Hentikan") : QStringLiteral("▶ Jalankan"));
    m_stepRun->setToolTip(busy ? QStringLiteral("Hentikan agent langkah ini")
                               : QStringLiteral("Jalankan langkah ini dengan bahan yang tersambung (Ctrl+Enter di kanvas)"));
    if (m_stepRun->property("busy").toBool() != busy) {
        m_stepRun->setProperty("busy", busy);
        repolish(m_stepRun);
    }
    m_stepRunUpstream->setEnabled(!busy);
    m_stepStatus->setText(resultStatus(node, state));
    const bool failed = !busy && node.hasResult() && !node.result.success;
    if (m_stepStatus->property("error").toBool() != failed) {
        m_stepStatus->setProperty("error", failed);
        repolish(m_stepStatus);
    }

    QString markdown;
    if (busy) {
        markdown = live.isEmpty() ? QStringLiteral("_Menunggu keluaran agent…_") : live;
    } else if (node.result.success) {
        markdown = node.result.message;
    } else if (node.hasResult()) {
        markdown = QStringLiteral("**Gagal (%1)**\n\n%2").arg(node.result.outcome, node.result.message);
    } else {
        markdown = QStringLiteral("_Belum ada hasil. Sambungkan kartu bahan ke langkah ini, tulis "
                                  "instruksinya, lalu tekan ▶ Jalankan._");
    }
    if (m_stepResult->markdown() != markdown) {
        m_stepResult->showMarkdown(markdown);
        if (busy) {
            m_stepResult->verticalScrollBar()->setValue(m_stepResult->verticalScrollBar()->maximum());
        }
    }
    m_stepToNote->setEnabled(!busy && node.result.success);

    const bool wantsTasks = node.output == CanvasStepOutput::Tasks;
    m_stepProposals->setVisible(wantsTasks);
    if (wantsTasks) {
        const qsizetype count = node.result.success ? CanvasWorkflow::parseTaskProposals(node.result.message).size() : 0;
        m_stepProposals->setEnabled(!busy && count > 0);
        m_stepProposals->setText(count > 0 ? QStringLiteral("Buat %1 task di WAITING").arg(count)
                                           : QStringLiteral("Buat task usulan"));
    }
}

void CanvasInspector::setLiveOutput(const QString &markdown) {
    if (m_node.kind != CanvasNodeKind::Step || m_node.id.isEmpty() || m_state == RunState::Idle) {
        return;
    }
    m_stepResult->showMarkdown(markdown.isEmpty() ? QStringLiteral("_Menunggu keluaran agent…_") : markdown);
    m_stepResult->verticalScrollBar()->setValue(m_stepResult->verticalScrollBar()->maximum());
    emit previewChanged();
}

QString CanvasInspector::previewMarkdown() const {
    if (m_node.id.isEmpty()) {
        return QString();
    }
    switch (m_node.kind) {
    case CanvasNodeKind::Note: {
        const QString text = m_noteText->toPlainText();
        return text.trimmed().isEmpty() ? QStringLiteral("_Catatan ini masih kosong._") : text;
    }
    case CanvasNodeKind::Artifact:
    case CanvasNodeKind::Task:
        return m_referenceView->markdown();
    case CanvasNodeKind::Step:
        return m_stepResult->markdown();
    }
    return QString();
}

QString CanvasInspector::previewKind() const {
    return m_kind->text();
}

QString CanvasInspector::previewTitle() const {
    return m_title->text();
}

void CanvasInspector::setPreviewOpen(bool open) {
    m_preview->setChecked(open);
    m_preview->setVisible(open || !m_node.id.isEmpty());
}

bool CanvasInspector::eventFilter(QObject *watched, QEvent *event) {
    if ((watched == m_noteText || watched == m_stepText) && event->type() == QEvent::FocusOut) {
        commitText();
    }
    return QWidget::eventFilter(watched, event);
}

void CanvasInspector::commitText() {
    if (m_node.id.isEmpty()) {
        return;
    }
    QPlainTextEdit *editor = m_node.kind == CanvasNodeKind::Note   ? m_noteText
                             : m_node.kind == CanvasNodeKind::Step ? m_stepText
                                                                   : nullptr;
    if (!editor) {
        return;
    }
    const QString text = editor->toPlainText();
    if (text != m_node.text) {
        m_node.text = text;
        emit textCommitted(m_node.id, text);
    }
}

void CanvasInspector::emitStepOptions() {
    if (m_node.id.isEmpty() || m_node.kind != CanvasNodeKind::Step) {
        return;
    }
    emit stepOptionsChosen(m_node.id, m_stepModel->currentData().toString(), m_stepEffort->currentData().toString(),
                           m_stepOutput->currentIndex() == 1 ? CanvasStepOutput::Tasks : CanvasStepOutput::Document);
}

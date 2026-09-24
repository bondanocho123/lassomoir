#include "ResponseDrawer.h"
#include "MarkdownView.h"
#include "RunLogFormatter.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QHash>
#include <QIcon>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QShortcut>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace {

constexpr int kNone = -1;   // task belum punya run
constexpr int kLive = -2;   // output run yang sedang berjalan

QString decisionLabel(const QString &decision) {
    if (decision == ReviewDecision::Approved) return QStringLiteral("disetujui");
    if (decision == ReviewDecision::Revise) return QStringLiteral("diminta revisi");
    if (decision == ReviewDecision::SentBack) return QStringLiteral("dikembalikan");
    return QString();
}

QString statusLabel(TaskState state, RunState runState) {
    if (runState == RunState::Running) return QStringLiteral("Agent berjalan");
    if (runState == RunState::Queued) return QStringLiteral("Antre");
    switch (state) {
    case TaskState::AwaitingReview: return QStringLiteral("Menunggu review");
    case TaskState::Failed: return QStringLiteral("Gagal");
    case TaskState::Idle: break;
    }
    return QStringLiteral("Siap");
}

// Setiap baris diberi "> " supaya tampil sebagai kutipan Markdown
QString quoted(const QString &text) {
    QStringList lines = text.split(QLatin1Char('\n'));
    for (QString &line : lines) {
        line.prepend(QStringLiteral("> "));
    }
    return lines.join(QLatin1Char('\n'));
}

}

ResponseDrawer::ResponseDrawer(MermaidRenderer *renderer, QWidget *parent)
    : QWidget(parent) {
    setObjectName("responseDrawer");
    setAttribute(Qt::WA_StyledBackground, true);
    setMinimumWidth(360);

    m_title = new QLabel(this);
    m_title->setObjectName("drawerTitle");
    m_title->setWordWrap(true);
    m_status = new QLabel(this);
    m_status->setObjectName("drawerStatus");

    m_expand = new QPushButton(this);
    m_expand->setObjectName("btnDrawerExpand");
    m_expand->setCursor(Qt::PointingHandCursor);
    m_expand->setFixedSize(26, 26);
    m_expand->setIconSize(QSize(14, 14));
    connect(m_expand, &QPushButton::clicked, this, [this]() {
        setExpanded(!m_expanded);
        emit expandToggled(m_expanded);
    });
    setExpanded(false);

    auto *btnClose = new QPushButton(QStringLiteral("✕"), this);
    btnClose->setObjectName("btnDrawerClose");
    btnClose->setCursor(Qt::PointingHandCursor);
    btnClose->setToolTip("Tutup (Esc)");
    btnClose->setFixedSize(26, 26);
    connect(btnClose, &QPushButton::clicked, this, &ResponseDrawer::closeRequested);

    auto *titleBox = new QVBoxLayout();
    titleBox->setSpacing(2);
    titleBox->addWidget(m_title);
    titleBox->addWidget(m_status);
    auto *header = new QHBoxLayout();
    header->setSpacing(2);
    header->addLayout(titleBox, 1);
    header->addWidget(m_expand, 0, Qt::AlignTop);
    header->addWidget(btnClose, 0, Qt::AlignTop);

    m_runSelector = new QComboBox(this);
    m_runSelector->setObjectName("drawerRunSelector");
    connect(m_runSelector, &QComboBox::currentIndexChanged, this, [this]() {
        showSelected();
        updateReviewPanel();
    });

    m_metrics = new QLabel(this);
    m_metrics->setObjectName("drawerMetrics");
    m_metrics->setWordWrap(true);
    m_metrics->setTextInteractionFlags(Qt::TextSelectableByMouse);

    m_view = new MarkdownView(renderer, this);

    // Panel keputusan: hanya tampil saat task menunggu review dan run yang direview dipilih
    m_reviewPanel = new QWidget(this);
    m_reviewPanel->setObjectName("drawerReviewPanel");
    m_reviewPanel->setAttribute(Qt::WA_StyledBackground, true);

    auto *reviewLabel = new QLabel("KEPUTUSAN REVIEW", m_reviewPanel);
    reviewLabel->setObjectName("drawerSectionLabel");
    m_note = new QPlainTextEdit(m_reviewPanel);
    m_note->setObjectName("drawerNote");
    m_note->setPlaceholderText("Jawaban / catatan untuk agent (mis. jawaban pertanyaan terbuka)…");
    m_note->setFixedHeight(90);
    m_noteHint = new QLabel(m_reviewPanel);
    m_noteHint->setObjectName("drawerNoteHint");
    m_noteHint->hide();
    connect(m_note, &QPlainTextEdit::textChanged, m_noteHint, &QLabel::hide);

    m_approve = new QPushButton(m_reviewPanel);
    m_approve->setObjectName("btnDrawerApprove");
    m_approve->setCursor(Qt::PointingHandCursor);
    connect(m_approve, &QPushButton::clicked, this, [this]() {
        emit approveRequested(m_task.id, m_note->toPlainText().trimmed());
    });

    // "&&": satu "&" di teks tombol dibaca Qt sebagai penanda shortcut
    m_revise = new QPushButton("Revisi && jalankan lagi", m_reviewPanel);
    m_revise->setObjectName("btnDrawerRevise");
    m_revise->setCursor(Qt::PointingHandCursor);
    connect(m_revise, &QPushButton::clicked, this, [this]() {
        if (requireNote()) {
            emit revisionRequested(m_task.id, m_note->toPlainText().trimmed());
        }
    });

    auto *decisionRow = new QHBoxLayout();
    decisionRow->setSpacing(8);
    decisionRow->addWidget(m_approve);
    decisionRow->addWidget(m_revise);
    decisionRow->addStretch(1);

    m_sendBackRow = new QWidget(m_reviewPanel);
    m_sendBack = new QPushButton("Kembalikan ke", m_sendBackRow);
    m_sendBack->setObjectName("btnDrawerSendBack");
    m_sendBack->setCursor(Qt::PointingHandCursor);
    m_sendBackTarget = new QComboBox(m_sendBackRow);
    m_sendBackTarget->setObjectName("drawerSendBackTarget");
    connect(m_sendBack, &QPushButton::clicked, this, [this]() {
        if (requireNote()) {
            emit sendBackRequested(m_task.id, m_sendBackTarget->currentText(), m_note->toPlainText().trimmed());
        }
    });
    auto *sendBackLayout = new QHBoxLayout(m_sendBackRow);
    sendBackLayout->setContentsMargins(0, 0, 0, 0);
    sendBackLayout->setSpacing(8);
    sendBackLayout->addWidget(m_sendBack);
    sendBackLayout->addWidget(m_sendBackTarget);
    sendBackLayout->addStretch(1);

    auto *reviewLayout = new QVBoxLayout(m_reviewPanel);
    reviewLayout->setContentsMargins(10, 10, 10, 10);
    reviewLayout->setSpacing(8);
    reviewLayout->addWidget(reviewLabel);
    reviewLayout->addWidget(m_note);
    reviewLayout->addWidget(m_noteHint);
    reviewLayout->addLayout(decisionRow);
    reviewLayout->addWidget(m_sendBackRow);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(8);
    root->addLayout(header);
    root->addWidget(m_runSelector);
    root->addWidget(m_metrics);
    root->addWidget(m_view, 1);
    root->addWidget(m_reviewPanel);

    auto *escape = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    escape->setContext(Qt::WidgetWithChildrenShortcut);
    connect(escape, &QShortcut::activated, this, &ResponseDrawer::closeRequested);

    m_reviewPanel->hide();
}

void ResponseDrawer::setExpanded(bool expanded) {
    m_expanded = expanded;
    m_expand->setIcon(QIcon(expanded ? QStringLiteral(":/icons/collapse.svg") : QStringLiteral(":/icons/expand.svg")));
    m_expand->setToolTip(expanded ? QStringLiteral("Kembalikan ukuran") : QStringLiteral("Perluas"));
}

void ResponseDrawer::showTask(const TaskItem &task, RunState runState, const QString &nextStage,
                              const QStringList &sendBackStages) {
    const bool sameTask = !m_task.id.isEmpty() && task.id == m_task.id;
    const int previousSelection = selectedRun();
    const RunState previousRunState = m_runState;

    m_task = task;
    m_runState = runState;
    m_nextStage = nextStage;

    m_title->setText(QStringLiteral("%1 / %2").arg(task.projectId, task.title));
    m_status->setText(QStringLiteral("%1 · %2").arg(task.stage, statusLabel(task.state, runState)));

    const bool running = runState != RunState::Idle;
    const int newest = int(task.runs.size()) - 1;
    int selected = newest;
    if (!sameTask) {
        m_liveMarkdown.clear();
        selected = running ? kLive : newest;
    } else if (running) {
        selected = previousSelection;
    } else if (previousSelection == kLive || previousRunState != RunState::Idle) {
        // Run baru saja selesai: ganti dari Live ke hasil yang sudah tersimpan
        selected = newest;
    } else {
        selected = qMin(previousSelection, newest);
    }
    rebuildRunSelector(selected);

    m_approve->setText(nextStage.isEmpty() ? QStringLiteral("Setujui")
                                           : QStringLiteral("Setujui → %1").arg(nextStage));
    m_sendBackTarget->clear();
    m_sendBackTarget->addItems(sendBackStages);
    const qsizetype coder = sendBackStages.indexOf(QStringLiteral("CODER"));
    m_sendBackTarget->setCurrentIndex(int(coder >= 0 ? coder : sendBackStages.size() - 1));
    m_sendBackRow->setVisible(!sendBackStages.isEmpty());

    if (task.state != TaskState::AwaitingReview) {
        m_note->clear();
        m_noteHint->hide();
    }
    updateReviewPanel();
}

void ResponseDrawer::startLive() {
    m_liveMarkdown.clear();
    if (m_runState == RunState::Idle) {
        m_runState = RunState::Running;
    }
    rebuildRunSelector(kLive);
    updateReviewPanel();
}

void ResponseDrawer::appendLive(const AgentEvent &event, const QString &workingDirectory) {
    switch (event.kind) {
    case AgentEvent::Kind::Text:
        m_liveMarkdown += event.text + QStringLiteral("\n\n");
        break;
    case AgentEvent::Kind::ToolUse:
        m_liveMarkdown += QStringLiteral("`` %1 ``\n\n").arg(RunLogFormatter::toolLabel(event, workingDirectory));
        break;
    case AgentEvent::Kind::Stderr:
        m_liveMarkdown += quoted(QStringLiteral("stderr: ") + event.text) + QStringLiteral("\n\n");
        break;
    case AgentEvent::Kind::Result:
        return;
    }

    if (selectedRun() == kLive) {
        m_view->showMarkdown(m_liveMarkdown);
        m_view->verticalScrollBar()->setValue(m_view->verticalScrollBar()->maximum());
    }
}

void ResponseDrawer::rebuildRunSelector(int selected) {
    const QSignalBlocker blocker(m_runSelector);
    m_runSelector->clear();

    if (m_runState != RunState::Idle) {
        m_runSelector->addItem(QStringLiteral("● Live"), kLive);
    }

    // Nomor run dihitung per stage (SPECIFIER #1, SPECIFIER #2, …); terbaru di atas
    QHash<QString, int> perStage;
    QList<int> numbers;
    for (const StageRun &run : std::as_const(m_task.runs)) {
        numbers.append(++perStage[run.stage]);
    }
    for (int i = int(m_task.runs.size()) - 1; i >= 0; --i) {
        const StageRun &run = m_task.runs.at(i);
        QString text = QStringLiteral("%1 #%2 · %3 · %4")
                           .arg(run.stage)
                           .arg(numbers.at(i))
                           .arg(run.finishedAt.toLocalTime().toString(QStringLiteral("dd/MM HH:mm")),
                                run.result.success ? QStringLiteral("✓") : QStringLiteral("✗"));
        const QString decision = decisionLabel(run.decision);
        if (!decision.isEmpty()) {
            text += QStringLiteral(" · ") + decision;
        }
        m_runSelector->addItem(text, i);
    }

    if (m_runSelector->count() == 0) {
        m_runSelector->addItem(QStringLiteral("Belum ada run"), kNone);
    }
    const int index = m_runSelector->findData(selected);
    m_runSelector->setCurrentIndex(index >= 0 ? index : 0);
    showSelected();
}

void ResponseDrawer::showSelected() {
    const int index = selectedRun();
    if (index == kLive) {
        m_view->showMarkdown(m_liveMarkdown.isEmpty() ? QStringLiteral("_Menunggu output agent…_") : m_liveMarkdown);
        m_view->verticalScrollBar()->setValue(m_view->verticalScrollBar()->maximum());
        m_metrics->setText(QStringLiteral("Sedang berjalan — teks dan tool yang dipakai agent muncul di sini."));
        return;
    }
    if (index < 0 || index >= m_task.runs.size()) {
        m_view->showMarkdown(QStringLiteral("_Belum ada hasil untuk task ini. Tekan ▶ di kartu untuk menjalankan agent._"));
        m_metrics->clear();
        return;
    }

    const StageRun &run = m_task.runs.at(index);
    QString markdown = run.result.message.trimmed();
    if (!run.result.success) {
        QString failure = QStringLiteral("**Run gagal (%1)**")
                              .arg(run.result.outcome.isEmpty() ? QStringLiteral("gagal") : run.result.outcome);
        if (!markdown.isEmpty()) {
            failure += QStringLiteral("\n\n") + markdown;
        }
        markdown = quoted(failure);
    } else if (markdown.isEmpty()) {
        markdown = QStringLiteral("_Agent tidak menulis jawaban akhir._");
    }
    if (!run.decision.isEmpty()) {
        markdown += QStringLiteral("\n\n---\n\n**Keputusan: %1**").arg(decisionLabel(run.decision));
        if (!run.reviewNote.isEmpty()) {
            markdown += QStringLiteral("\n\n") + quoted(run.reviewNote);
        }
    }
    m_view->showMarkdown(markdown);

    QStringList parts = {
        RunLogFormatter::formatDuration(run.result.durationMs),
        QStringLiteral("%1 tok").arg(RunLogFormatter::formatTokens(run.result.totalTokens)),
        QStringLiteral("$%1").arg(run.result.costUsd, 0, 'f', 2),
    };
    if (!run.result.sessionId.isEmpty()) {
        parts.append(QStringLiteral("sesi %1").arg(run.result.sessionId));
    }
    QString metrics = parts.join(QStringLiteral(" · "));
    if (!run.result.deniedTools.isEmpty()) {
        QStringList tools = run.result.deniedTools;
        tools.removeDuplicates();
        metrics += QStringLiteral("\n%1 aksi ditolak karena butuh izin: %2")
                       .arg(run.result.deniedTools.size())
                       .arg(tools.join(QStringLiteral(", ")));
    }
    m_metrics->setText(metrics);
}

void ResponseDrawer::updateReviewPanel() {
    const bool reviewing = m_task.state == TaskState::AwaitingReview
                           && m_runState == RunState::Idle
                           && selectedRun() >= 0
                           && selectedRun() == reviewedRunIndex();
    m_reviewPanel->setVisible(reviewing);
}

int ResponseDrawer::selectedRun() const {
    const QVariant data = m_runSelector->currentData();
    return data.isValid() ? data.toInt() : kNone;
}

int ResponseDrawer::reviewedRunIndex() const {
    if (m_task.state != TaskState::AwaitingReview) {
        return -1;
    }
    for (int i = int(m_task.runs.size()) - 1; i >= 0; --i) {
        if (m_task.runs.at(i).stage == m_task.stage) {
            return i;
        }
    }
    return -1;
}

bool ResponseDrawer::requireNote() {
    if (!m_note->toPlainText().trimmed().isEmpty()) {
        return true;
    }
    m_noteHint->setText(QStringLiteral("Tulis catatan dulu: agent perlu tahu apa yang harus diubah."));
    m_noteHint->show();
    m_note->setFocus();
    return false;
}

#include "ConsoleTaskCard.h"
#include "RunPulse.h"
#include "Theme.h"

#include <QApplication>
#include <QEvent>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QSize>
#include <QStringList>
#include <QStyle>
#include <QTime>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

// Baris log per kartu; yang terlama dibuang lebih dulu supaya memori tidak terus membengkak
constexpr int kTaskLogLines = 2000;
constexpr int kSystemLogLines = 500;
constexpr int kLogHeight = 170;
constexpr int kTooltipChars = 300;

struct Summary {
    QString tag;
    QString text;
};

// "[GIT] TTT/Login push ..." -> tag "GIT", teks "push ..."; "[AGENT:CODER] → Read a.cpp" -> tag
// "CODER". Nama task dibuang karena sudah tampil di kepala kartu. Baris tanpa [TAG] tampil apa adanya.
Summary summarize(const QString &entry, const QString &taskLabel) {
    const QStringList lines = entry.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    Summary summary;
    QString text = lines.value(0).trimmed();
    if (text.startsWith(QLatin1Char('['))) {
        const qsizetype close = text.indexOf(QLatin1Char(']'));
        if (close > 1) {
            summary.tag = text.mid(1, close - 1);
            text = text.mid(close + 1).trimmed();
        }
    }
    if (summary.tag.startsWith(QLatin1String("AGENT:"))) {
        summary.tag = summary.tag.mid(6);
    }
    if (!taskLabel.isEmpty() && text.startsWith(taskLabel)) {
        text = text.mid(taskLabel.size()).trimmed();
        // "[RUN] TTT/Login · CODER · E:/repo" -> "CODER · E:/repo"
        while (text.startsWith(QChar(0x00B7)) || text.startsWith(QLatin1Char(':'))) {
            text = text.mid(1).trimmed();
        }
    }
    // Tag sendirian (teksnya mulai di baris berikutnya)
    if (text.isEmpty() && lines.size() > 1) {
        text = lines.at(1).trimmed();
    }
    summary.text = text;
    return summary;
}

// Property yang dibaca selector styles.qss berubah setelah stylesheet terpasang: aturannya baru
// dihitung ulang lewat repolish
void setStyleState(QWidget *widget, const QString &state) {
    if (widget->property("state").toString() == state) {
        return;
    }
    widget->setProperty("state", state);
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
}

}

ConsoleTaskCard::ConsoleTaskCard(const QString &taskId, QWidget *parent)
    : QWidget(parent),
      m_taskId(taskId),
      m_pulse(new RunPulse(this)) {
    // Tanpa WA_StyledBackground latar dan sudut membulat dari styles.qss tidak ikut dilukis
    setAttribute(Qt::WA_StyledBackground, true);
    setAttribute(Qt::WA_Hover, true);
    setCursor(Qt::PointingHandCursor);
    // Tetap selama umur kartu, jadi cukup dipasang sebelum stylesheet pertama kali diterapkan
    setProperty("kind", isSystem() ? QStringLiteral("system") : QStringLiteral("task"));

    m_project = new QLabel(isSystem() ? QStringLiteral("SISTEM") : QString(), this);
    m_project->setObjectName("consoleCardProject");
    m_stageLabel = new QLabel(this);
    m_stageLabel->setObjectName("consoleCardStage");
    m_status = new QLabel(this);
    m_status->setObjectName("consoleCardStatus");

    auto *header = new QHBoxLayout();
    header->setSpacing(6);
    header->addWidget(m_project);
    header->addWidget(m_stageLabel);
    header->addStretch(1);
    header->addWidget(m_status);

    m_title = new QLabel(this);
    m_title->setObjectName("consoleCardTitle");
    m_title->setWordWrap(true);

    m_tag = new QLabel(this);
    m_tag->setObjectName("consoleCardTag");
    m_summary = new QLabel(this);
    m_summary->setObjectName("consoleCardSummary");
    // Satu baris yang dipotong "…" (refreshSummary): tinggi kartu tetap selama output agent
    // mengalir, dan teks panjang tidak ikut melebarkan panel
    m_summary->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_summary->installEventFilter(this);

    auto *summaryRow = new QHBoxLayout();
    summaryRow->setSpacing(6);
    summaryRow->addWidget(m_tag);
    summaryRow->addWidget(m_summary, 1);

    m_meta = new QLabel(this);
    m_meta->setObjectName("consoleCardMeta");

    m_toggle = new QToolButton(this);
    m_toggle->setObjectName("btnConsoleCardLog");
    m_toggle->setText(QStringLiteral("Log"));
    m_toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_toggle->setIconSize(QSize(10, 10));
    m_toggle->setCursor(Qt::PointingHandCursor);
    connect(m_toggle, &QToolButton::clicked, this, [this]() { setLogVisible(!isLogVisible()); });

    auto *footer = new QHBoxLayout();
    footer->setSpacing(6);
    footer->addWidget(m_meta);
    footer->addStretch(1);
    footer->addWidget(m_toggle);

    m_log = new QPlainTextEdit(this);
    m_log->setObjectName("consoleCardLog");
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(isSystem() ? kSystemLogLines : kTaskLogLines);
    m_log->setFixedHeight(kLogHeight);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(10, 8, 10, 8);
    root->setSpacing(4);
    root->addLayout(header);
    root->addWidget(m_title);
    root->addLayout(summaryRow);
    root->addLayout(footer);
    root->addWidget(m_log);

    m_stageLabel->setVisible(!isSystem());
    m_title->setVisible(!isSystem());
    m_tag->hide();
    setLogVisible(false);
    refreshState();
}

void ConsoleTaskCard::setTask(const TaskItem &task) {
    m_taskLabel = QStringLiteral("%1/%2").arg(task.projectId, task.title);
    m_stage = task.stage;
    m_taskState = task.state;
    m_project->setText(task.projectId);
    m_stageLabel->setText(task.stage);
    m_title->setText(task.title);
    refreshState();
}

void ConsoleTaskCard::setRunState(RunState state) {
    if (m_runState == state) {
        return;
    }
    m_runState = state;
    m_pulse->setActive(state == RunState::Running);
    refreshState();
}

void ConsoleTaskCard::setRemoved() {
    m_removed = true;
    m_runState = RunState::Idle;
    m_pulse->setActive(false);
    refreshState();
}

void ConsoleTaskCard::appendEntry(const QString &text) {
    const QString time = QTime::currentTime().toString(QStringLiteral("hh:mm:ss"));
    m_log->appendPlainText(QStringLiteral("%1  %2").arg(time, text));
    ++m_entryCount;
    m_meta->setText(QStringLiteral("%1 · %2 notifikasi").arg(time).arg(m_entryCount));

    const Summary summary = summarize(text, m_taskLabel);
    m_tag->setText(summary.tag);
    m_tag->setVisible(!summary.tag.isEmpty());
    m_summaryText = summary.text;
    refreshSummary();
}

QString ConsoleTaskCard::logText() const {
    return m_log->toPlainText();
}

void ConsoleTaskCard::setLogVisible(bool visible) {
    m_log->setVisible(visible);
    m_toggle->setIcon(Theme::icon(visible ? QStringLiteral(":/icons/chevron-down.svg")
                                    : QStringLiteral(":/icons/chevron-right.svg")));
    m_toggle->setToolTip(visible ? QStringLiteral("Tutup log") : QStringLiteral("Lihat semua notifikasi"));
    if (visible) {
        // Baris terbaru di bawah; ukuran log baru benar setelah layout kartu dihitung ulang
        QTimer::singleShot(0, m_log, [log = m_log]() {
            log->verticalScrollBar()->setValue(log->verticalScrollBar()->maximum());
        });
    }
}

bool ConsoleTaskCard::isLogVisible() const {
    return !m_log->isHidden();
}

void ConsoleTaskCard::refreshState() {
    // Urutan sama dengan kartu kanban: run yang aktif lebih penting dari status task di stage-nya
    QString state = QStringLiteral("idle");
    QString label;
    if (m_removed) {
        state = QStringLiteral("removed");
        label = QStringLiteral("Dihapus");
    } else if (m_runState == RunState::Running) {
        state = QStringLiteral("running");
        label = QStringLiteral("Berjalan");
    } else if (m_runState == RunState::Queued) {
        state = QStringLiteral("queued");
        label = QStringLiteral("Antre");
    } else if (m_taskState == TaskState::AwaitingReview) {
        state = QStringLiteral("review");
        label = QStringLiteral("Menunggu review");
    } else if (m_taskState == TaskState::Failed) {
        state = QStringLiteral("failed");
        label = QStringLiteral("Gagal");
    } else if (m_stage == QLatin1String("DONE")) {
        state = QStringLiteral("done");
        label = QStringLiteral("Selesai");
    }
    m_status->setText(label);
    m_status->setVisible(!label.isEmpty());
    setStyleState(this, state);
    setStyleState(m_status, state);
}

void ConsoleTaskCard::refreshSummary() {
    const QString shown = m_summary->fontMetrics().elidedText(m_summaryText, Qt::ElideRight, m_summary->width());
    m_summary->setText(shown);
    m_summary->setToolTip(shown == m_summaryText ? QString() : m_summaryText.left(kTooltipChars));
}

void ConsoleTaskCard::mousePressEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    m_pressPosition = event->position().toPoint();
    m_pressed = true;
    event->accept();
}

void ConsoleTaskCard::mouseReleaseEvent(QMouseEvent *event) {
    const QPoint position = event->position().toPoint();
    const bool click = m_pressed && event->button() == Qt::LeftButton && rect().contains(position)
                       && (position - m_pressPosition).manhattanLength() < QApplication::startDragDistance();
    m_pressed = false;
    if (!click) {
        QWidget::mouseReleaseEvent(event);
        return;
    }

    // Task yang masih ada: buka hasil agent-nya, sama seperti klik kartu kanban. Kartu sistem dan
    // task yang sudah dihapus tidak punya tampilan lain selain log-nya.
    if (isSystem() || m_removed) {
        setLogVisible(!isLogVisible());
    } else {
        emit activated(m_taskId);
    }
}

void ConsoleTaskCard::paintEvent(QPaintEvent *event) {
    QWidget::paintEvent(event);
    if (m_pulse->isActive()) {
        QPainter painter(this);
        // Radius sama dengan border-radius kartu konsol di styles.qss
        m_pulse->paint(painter, rect(), 8.0);
    }
}

bool ConsoleTaskCard::eventFilter(QObject *watched, QEvent *event) {
    if (watched == m_summary && (event->type() == QEvent::Resize || event->type() == QEvent::FontChange)) {
        refreshSummary();
    }
    return QWidget::eventFilter(watched, event);
}

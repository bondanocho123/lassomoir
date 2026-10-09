#include "CanvasChatPanel.h"
#include "CanvasChat.h"
#include "CanvasWorkflow.h"
#include "MarkdownView.h"
#include "RunLogFormatter.h"
#include "Theme.h"

#include <QAbstractTextDocumentLayout>
#include <QClipboard>
#include <QComboBox>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QStyle>
#include <QTextDocument>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtMath>

#include <functional>

namespace {

struct ModelChoice {
    const char *value;
    const char *label;
};
const ModelChoice kModels[] = {{"haiku", "Haiku"}, {"sonnet", "Sonnet"}, {"opus", "Opus"}};

const char *const kSuggestions[] = {
    "Ringkas keputusan penting dari bahan ini",
    "Apa yang masih bertentangan atau belum jelas antar dokumen?",
    "Risiko apa yang paling perlu ditangani lebih dulu?",
    "Ide apa yang belum tergali dari bahan ini?",
};

QString timeOf(const QDateTime &at) {
    return at.isValid() ? at.toLocalTime().toString(QStringLiteral("HH:mm")) : QString();
}

QPushButton *smallButton(const QString &text, QWidget *parent) {
    auto *button = new QPushButton(text, parent);
    button->setObjectName("btnCanvasChatAction");
    button->setCursor(Qt::PointingHandCursor);
    button->setFocusPolicy(Qt::NoFocus);
    return button;
}

QLabel *label(const QString &text, const char *name, QWidget *parent) {
    auto *result = new QLabel(text, parent);
    result->setObjectName(QLatin1String(name));
    result->setWordWrap(true);
    return result;
}

void repolish(QWidget *widget) {
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
}

// Jawaban Markdown setinggi isinya: yang menggulir adalah daftar pesan, bukan tiap jawaban
class AnswerView final : public MarkdownView {
public:
    AnswerView(MermaidRenderer *renderer, QWidget *parent) : MarkdownView(renderer, parent) {
        setObjectName("canvasChatAnswer");
        setFrameShape(QFrame::NoFrame);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setFixedHeight(24);
        connect(document()->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged, this,
                [this]() { fitHeight(); });
    }

protected:
    void resizeEvent(QResizeEvent *event) override {
        MarkdownView::resizeEvent(event);
        fitHeight();
    }
    // Roda mouse diteruskan ke daftar pesan
    void wheelEvent(QWheelEvent *event) override { event->ignore(); }

private:
    void fitHeight() {
        const int chrome = qMax(0, height() - viewport()->height());
        const int wanted = qMax(20, qCeil(document()->size().height()) + chrome);
        if (wanted != height()) {
            setFixedHeight(wanted);
        }
    }
};

// Contoh pertanyaan yang bisa diklik; teksnya membungkus di panel yang sempit
class Suggestion final : public QLabel {
public:
    Suggestion(const QString &text, QWidget *parent) : QLabel(text, parent) {
        setObjectName("canvasChatSuggestion");
        setWordWrap(true);
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover, true);
    }
    std::function<void()> clicked;

protected:
    void mouseReleaseEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint()) && clicked) {
            clicked();
        }
        QLabel::mouseReleaseEvent(event);
    }
};

// Enter mengirim, Shift+Enter baris baru
class ChatInput final : public QPlainTextEdit {
public:
    using QPlainTextEdit::QPlainTextEdit;
    std::function<void()> submit;

protected:
    void keyPressEvent(QKeyEvent *event) override {
        const bool enter = event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter;
        if (enter && !(event->modifiers() & Qt::ShiftModifier)) {
            if (submit) {
                submit();
            }
            event->accept();
            return;
        }
        QPlainTextEdit::keyPressEvent(event);
    }
};

}

CanvasChatPanel::CanvasChatPanel(CanvasChat &chat, MermaidRenderer *renderer, QWidget *parent)
    : QWidget(parent), m_chat(chat), m_renderer(renderer) {
    setObjectName("canvasChatPanel");
    setAttribute(Qt::WA_StyledBackground, true);

    auto *title = label(QStringLiteral("CHAT BRAINSTORM"), "canvasPanelTitle", this);
    title->setWordWrap(false);
    m_clear = smallButton(QStringLiteral("Mulai ulang"), this);
    m_clear->setToolTip(QStringLiteral("Hapus seluruh percakapan chat kanvas ini"));
    auto *header = new QHBoxLayout();
    header->setContentsMargins(0, 0, 0, 0);
    header->addWidget(title);
    header->addStretch(1);
    header->addWidget(m_clear);
    auto *hint = label(QStringLiteral("Tanya jawab tentang artefak di kanvas. Agent membaca kartu bahan dan kode "
                                      "project tanpa mengubah apa pun."),
                       "canvasPanelHint", this);

    // Daftar pesan; pesan baru disisipkan sebelum stretch penutup
    m_scroll = new QScrollArea(this);
    m_scroll->setObjectName("canvasChatScroll");
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *listWidget = new QWidget(m_scroll);
    listWidget->setObjectName("canvasChatList");
    m_list = new QVBoxLayout(listWidget);
    m_list->setContentsMargins(0, 4, 6, 4);
    m_list->setSpacing(10);

    m_empty = new QWidget(listWidget);
    auto *emptyLayout = new QVBoxLayout(m_empty);
    emptyLayout->setContentsMargins(0, 0, 0, 0);
    emptyLayout->setSpacing(6);
    emptyLayout->addWidget(label(QStringLiteral("Pilih kartu di kanvas lalu tanyakan sesuatu tentangnya, atau bertanya "
                                                "tentang seluruh kanvas. Jawaban yang berguna bisa dijadikan catatan."),
                                 "canvasInspectorTips", m_empty));
    for (const char *suggestion : kSuggestions) {
        const QString text = QString::fromUtf8(suggestion);
        auto *item = new Suggestion(text, m_empty);
        item->clicked = [this, text]() {
            m_input->setPlainText(text);
            focusInput();
        };
        emptyLayout->addWidget(item);
    }
    m_list->addWidget(m_empty);
    m_list->addStretch(1);
    m_scroll->setWidget(listWidget);
    m_scroll->viewport()->setAutoFillBackground(false);
    listWidget->setAutoFillBackground(false);

    QScrollBar *bar = m_scroll->verticalScrollBar();
    connect(bar, &QScrollBar::rangeChanged, this, [this, bar](int, int maximum) {
        if (m_stickToEnd) {
            bar->setValue(maximum);
        }
    });
    connect(bar, &QScrollBar::valueChanged, this, [this, bar](int value) { m_stickToEnd = value >= bar->maximum() - 24; });

    m_error = label(QString(), "canvasStepStatus", this);
    m_error->setProperty("error", true);
    m_error->hide();
    m_context = label(QString(), "canvasChatContext", this);
    m_context->setTextFormat(Qt::RichText);
    m_contextHint = label(QString(), "canvasPanelHint", this);

    auto *input = new ChatInput(this);
    input->submit = [this]() { send(); };
    m_input = input;
    m_input->setObjectName("canvasChatInput");
    m_input->setPlaceholderText(QStringLiteral("Tanyakan sesuatu tentang bahan ini…  (Enter kirim, Shift+Enter baris baru)"));
    m_input->setTabChangesFocus(true);
    m_input->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    connect(m_input, &QPlainTextEdit::textChanged, this, &CanvasChatPanel::fitInput);

    m_model = new QComboBox(this);
    m_model->setObjectName("canvasInspectorCombo");
    m_model->setToolTip(QStringLiteral("Model agent untuk pertanyaan berikutnya"));
    const QString defaultModel = CanvasWorkflow::chatAgent().model;
    QString defaultLabel = defaultModel;
    for (const ModelChoice &choice : kModels) {
        if (defaultModel == QLatin1String(choice.value)) {
            defaultLabel = QString::fromLatin1(choice.label);
        }
    }
    m_model->addItem(QStringLiteral("Bawaan (%1)").arg(defaultLabel), QString());
    for (const ModelChoice &choice : kModels) {
        m_model->addItem(QString::fromLatin1(choice.label), QString::fromLatin1(choice.value));
    }
    m_send = new QPushButton(QStringLiteral("Kirim"), this);
    m_send->setObjectName("btnCanvasRunStep");
    m_send->setCursor(Qt::PointingHandCursor);
    m_send->setFocusPolicy(Qt::NoFocus);
    connect(m_send, &QPushButton::clicked, this, [this]() {
        if (m_chat.isBusy()) {
            m_chat.cancel();
        } else {
            send();
        }
    });
    auto *footer = new QHBoxLayout();
    footer->setContentsMargins(0, 0, 0, 0);
    footer->setSpacing(6);
    footer->addWidget(m_model, 1);
    footer->addWidget(m_send);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(6);
    layout->addLayout(header);
    layout->addWidget(hint);
    layout->addSpacing(2);
    layout->addWidget(m_scroll, 1);
    layout->addWidget(m_error);
    layout->addWidget(m_context);
    layout->addWidget(m_contextHint);
    layout->addWidget(m_input);
    layout->addLayout(footer);

    m_liveTimer.setSingleShot(true);
    m_liveTimer.setInterval(120);
    connect(&m_liveTimer, &QTimer::timeout, this, &CanvasChatPanel::showLive);

    connect(&m_chat, &CanvasChat::messageAdded, this, [this](const CanvasChatMessage &message) {
        if (message.isUser()) {
            // Input hanya dikosongkan bila pertanyaannya memang berasal dari sana (bukan Tanya lagi)
            if (!m_pendingQuestion.isEmpty() && message.text == m_pendingQuestion) {
                m_input->clear();
            }
            m_pendingQuestion.clear();
            m_error->hide();
            m_stickToEnd = true;
        }
        if (!message.isUser() && m_live) {
            delete m_live;   // jawaban akhirnya menggantikan keluaran live
            m_live = nullptr;
            m_liveView = nullptr;
            m_liveActivity = nullptr;
        }
        addMessage(message);
        updateEmptyState();
    });
    connect(&m_chat, &CanvasChat::messagesReset, this, &CanvasChatPanel::rebuild);
    connect(&m_chat, &CanvasChat::busyChanged, this, &CanvasChatPanel::setBusy);
    connect(&m_chat, &CanvasChat::liveChanged, this, [this]() {
        if (!m_liveTimer.isActive()) {
            m_liveTimer.start();
        }
    });
    connect(m_clear, &QPushButton::clicked, this, [this]() {
        const auto answer = QMessageBox::question(
            this, QStringLiteral("Mulai ulang chat"),
            QStringLiteral("Hapus seluruh percakapan chat di kanvas ini? Catatan yang sudah dibuat dari jawaban tetap ada."));
        if (answer == QMessageBox::Yes) {
            m_chat.clear();
        }
    });

    setContext(CanvasBoard(), {});
    rebuild();
    fitInput();
}

CanvasChatPanel::~CanvasChatPanel() {
    // Anak panel dibongkar sesudah anggota ini hilang: sinyal CanvasChat dan input tidak boleh sampai lagi
    disconnect(&m_chat, nullptr, this, nullptr);
    disconnect(m_input, nullptr, this, nullptr);
}

void CanvasChatPanel::setContext(const CanvasBoard &board, const QStringList &selectedIds) {
    m_contextIds = selectedIds;
    m_context->setText(QStringLiteral("<b>Bahan</b> · %1")
                           .arg(CanvasWorkflow::chatContextLabel(board, selectedIds).toHtmlEscaped()));
    m_contextHint->setText(selectedIds.isEmpty() ? QStringLiteral("Pilih kartu di kanvas untuk mempersempit bahan")
                                                 : QStringLiteral("Kosongkan pilihan (Esc di kanvas) untuk bertanya "
                                                                  "tentang seluruh kanvas"));
}

void CanvasChatPanel::focusInput() {
    m_input->setFocus(Qt::OtherFocusReason);
    m_input->moveCursor(QTextCursor::End);
}

void CanvasChatPanel::showError(const QString &text) {
    m_error->setText(text);
    m_error->setVisible(!text.isEmpty());
}

QString CanvasChatPanel::draft() const {
    return m_input->toPlainText();
}

void CanvasChatPanel::rebuild() {
    for (int i = m_list->count() - 1; i >= 0; --i) {
        QWidget *widget = m_list->itemAt(i)->widget();
        if (widget && widget != m_empty) {
            m_list->removeWidget(widget);
            delete widget;
        }
    }
    m_live = nullptr;
    m_liveView = nullptr;
    m_liveActivity = nullptr;
    const QList<CanvasChatMessage> messages = m_chat.messages();
    for (const CanvasChatMessage &message : messages) {
        addMessage(message);
    }
    m_stickToEnd = true;
    setBusy(m_chat.isBusy());
}

void CanvasChatPanel::addMessage(const CanvasChatMessage &message) {
    QWidget *bubble = message.isUser() ? userBubble(message) : agentBubble(message);
    // Sebelum keluaran live (bila ada) dan stretch penutup
    const int before = m_live ? m_list->indexOf(m_live) : m_list->count() - 1;
    m_list->insertWidget(before, bubble);
}

QWidget *CanvasChatPanel::userBubble(const CanvasChatMessage &message) {
    auto *row = new QWidget();
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(36, 0, 0, 0);
    auto *bubble = new QFrame(row);
    bubble->setObjectName("canvasChatUser");
    auto *layout = new QVBoxLayout(bubble);
    layout->setContentsMargins(10, 7, 10, 7);
    layout->setSpacing(3);
    auto *text = label(message.text, "canvasChatUserText", bubble);
    text->setTextFormat(Qt::PlainText);
    text->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(text);

    QString context = message.contextLabel.toHtmlEscaped();
    if (!message.contextIds.isEmpty()) {
        context = Theme::html(QStringLiteral("<a href=\"bahan\" style=\"color:#33517a; text-decoration:none\">%1</a>")
                                  .arg(context));
    }
    auto *meta = label(QStringLiteral("%1 · %2").arg(timeOf(message.at), context), "canvasChatMeta", bubble);
    meta->setTextFormat(Qt::RichText);
    meta->setToolTip(message.contextIds.isEmpty() ? QStringLiteral("Bahan pertanyaan ini")
                                                  : QStringLiteral("Klik untuk memilih kartu bahannya di kanvas"));
    const QStringList ids = message.contextIds;
    connect(meta, &QLabel::linkActivated, this, [this, ids]() { emit contextActivated(ids); });
    layout->addWidget(meta);
    // Rata kanan lewat stretch, bukan alignment: dengan alignment lebar gelembung tidak lagi sama
    // dengan lebar yang dipakai menghitung tinggi teks yang membungkus, dan barisnya terpotong
    rowLayout->addStretch(1);
    rowLayout->addWidget(bubble);
    return row;
}

QWidget *CanvasChatPanel::agentBubble(const CanvasChatMessage &message) {
    auto *bubble = new QFrame();
    bubble->setObjectName("canvasChatAgent");
    auto *layout = new QVBoxLayout(bubble);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(4);

    QStringList facts;
    if (!message.model.isEmpty()) {
        facts.append(message.model);
    }
    facts.append(timeOf(message.at));
    if (message.durationMs > 0) {
        facts.append(RunLogFormatter::formatDuration(message.durationMs));
    }
    if (message.totalTokens > 0) {
        facts.append(QStringLiteral("%1 tok").arg(RunLogFormatter::formatTokens(message.totalTokens)));
    }
    if (message.costUsd > 0) {
        facts.append(QStringLiteral("$%1").arg(message.costUsd, 0, 'f', 2));
    }
    auto *header = new QHBoxLayout();
    header->setSpacing(6);
    header->addWidget(label(QStringLiteral("AGENT"), "canvasChatAgentName", bubble));
    header->addWidget(label(facts.join(QStringLiteral(" · ")), "canvasChatMeta", bubble), 1);
    layout->addLayout(header);

    const bool cancelled = message.outcome == QLatin1String("cancelled");
    const QString answerId = message.id;
    auto *actions = new QHBoxLayout();
    actions->setSpacing(4);
    if (message.succeeded() || (cancelled && !message.text.trimmed().isEmpty())) {
        MarkdownView *view = answerView(bubble);
        view->showMarkdown(message.text);
        layout->addWidget(view);
        if (cancelled) {
            layout->addWidget(label(QStringLiteral("Dihentikan sebelum selesai."), "canvasChatMeta", bubble));
        }
        auto *toNote = smallButton(QStringLiteral("Jadikan catatan"), bubble);
        toNote->setToolTip(QStringLiteral("Taruh jawaban ini di kanvas sebagai catatan, tersambung dari kartu bahannya"));
        connect(toNote, &QPushButton::clicked, this, [this, answerId]() { emit noteRequested(answerId); });
        auto *copy = smallButton(QStringLiteral("Salin"), bubble);
        const QString text = message.text;
        connect(copy, &QPushButton::clicked, this, [text]() { QGuiApplication::clipboard()->setText(text); });
        // Panel chat sempit: jawaban panjang, tabel, dan diagram lebih enak dibaca di drawer pratinjau
        auto *expand = smallButton(QStringLiteral("Perluas"), bubble);
        expand->setIcon(Theme::icon(QStringLiteral(":/icons/expand.svg")));
        expand->setIconSize(QSize(11, 11));
        expand->setToolTip(QStringLiteral("Buka jawaban ini di drawer pratinjau yang lebih lebar"));
        connect(expand, &QPushButton::clicked, this, [this, answerId]() { emit expandRequested(answerId); });
        actions->addWidget(toNote);
        actions->addWidget(copy);
        actions->addWidget(expand);
    } else {
        const QString reason = cancelled ? QStringLiteral("Dihentikan sebelum ada jawaban.")
                                         : QStringLiteral("Gagal (%1): %2").arg(message.outcome, message.text);
        auto *failure = label(reason, "canvasChatFailure", bubble);
        failure->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(failure);
        auto *retry = smallButton(QStringLiteral("Tanya lagi"), bubble);
        retry->setToolTip(QStringLiteral("Kirim ulang pertanyaan ini dengan bahan yang sama"));
        const QString questionId = message.replyTo;
        connect(retry, &QPushButton::clicked, this, [this, questionId]() {
            const CanvasChatMessage *question = m_chat.message(questionId);
            if (question && !m_chat.isBusy()) {
                emit askRequested(question->text, question->contextIds, m_model->currentData().toString());
            }
        });
        retry->setEnabled(m_chat.message(questionId) != nullptr);
        actions->addWidget(retry);
    }
    actions->addStretch(1);
    layout->addLayout(actions);
    return bubble;
}

MarkdownView *CanvasChatPanel::answerView(QWidget *parent) {
    return new AnswerView(m_renderer, parent);
}

void CanvasChatPanel::setBusy(bool busy) {
    m_send->setText(busy ? QStringLiteral("■ Hentikan") : QStringLiteral("Kirim"));
    m_send->setToolTip(busy ? QStringLiteral("Hentikan agent; potongan jawabannya tetap disimpan")
                            : QStringLiteral("Kirim pertanyaan (Enter)"));
    if (m_send->property("busy").toBool() != busy) {
        m_send->setProperty("busy", busy);
        repolish(m_send);
    }
    m_model->setEnabled(!busy);
    m_clear->setEnabled(!busy && !m_chat.messages().isEmpty());

    if (busy && !m_live) {
        auto *bubble = new QFrame();
        bubble->setObjectName("canvasChatAgent");
        auto *layout = new QVBoxLayout(bubble);
        layout->setContentsMargins(10, 8, 10, 8);
        layout->setSpacing(4);
        auto *header = new QHBoxLayout();
        header->setSpacing(6);
        header->addWidget(label(QStringLiteral("AGENT"), "canvasChatAgentName", bubble));
        header->addWidget(label(QStringLiteral("sedang menjawab…"), "canvasChatMeta", bubble), 1);
        layout->addLayout(header);
        m_liveActivity = label(QStringLiteral("Membaca bahan…"), "canvasChatActivity", bubble);
        layout->addWidget(m_liveActivity);
        m_liveView = answerView(bubble);
        m_liveView->hide();
        layout->addWidget(m_liveView);
        m_live = bubble;
        m_list->insertWidget(m_list->count() - 1, bubble);
        showLive();
    } else if (!busy && m_live) {
        delete m_live;
        m_live = nullptr;
        m_liveView = nullptr;
        m_liveActivity = nullptr;
    }
    updateEmptyState();
}

void CanvasChatPanel::showLive() {
    if (!m_live) {
        return;
    }
    const QString activity = m_chat.activity();
    m_liveActivity->setText(activity.isEmpty() ? QStringLiteral("Membaca bahan…") : activity);
    const QString text = m_chat.liveText();
    if (!text.isEmpty()) {
        m_liveView->show();
        if (m_liveView->markdown() != text) {
            m_liveView->showMarkdown(text);
        }
    }
}

void CanvasChatPanel::send() {
    const QString question = m_input->toPlainText().trimmed();
    if (question.isEmpty() || m_chat.isBusy()) {
        return;
    }
    m_error->hide();
    m_pendingQuestion = question;
    emit askRequested(question, m_contextIds, m_model->currentData().toString());
}

void CanvasChatPanel::fitInput() {
    // Tumbuh mengikuti isinya, 2 sampai 6 baris
    const int lines = qBound(2, qCeil(m_input->document()->size().height()), 6);
    const int chrome = m_input->height() > m_input->viewport()->height() ? m_input->height() - m_input->viewport()->height()
                                                                         : 14;
    const int wanted = lines * m_input->fontMetrics().lineSpacing() + 2 * qCeil(m_input->document()->documentMargin())
                       + chrome;
    if (wanted != m_input->height()) {
        m_input->setFixedHeight(wanted);
    }
}

void CanvasChatPanel::updateEmptyState() {
    m_empty->setVisible(m_chat.messages().isEmpty() && !m_chat.isBusy());
    m_clear->setEnabled(!m_chat.isBusy() && !m_chat.messages().isEmpty());
}

#include "CanvasChatPanel.h"
#include "CanvasChat.h"
#include "CanvasWorkflow.h"
#include "ElidedLabel.h"
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
#include <QLayout>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
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

// Waktu pesan terakhir sebuah sesi di riwayat: tanggalnya ikut bila bukan hari ini
QString dayOf(const QDateTime &at) {
    if (!at.isValid()) {
        return QString();
    }
    const QDateTime local = at.toLocalTime();
    const QDate today = QDate::currentDate();
    if (local.date() == today) {
        return QStringLiteral("Hari ini %1").arg(local.toString(QStringLiteral("HH:mm")));
    }
    return local.toString(local.date().year() == today.year() ? QStringLiteral("d MMM HH:mm")
                                                               : QStringLiteral("d MMM yyyy"));
}

QPushButton *smallButton(const QString &text, QWidget *parent) {
    auto *button = new QPushButton(text, parent);
    button->setObjectName("btnCanvasChatAction");
    button->setCursor(Qt::PointingHandCursor);
    button->setFocusPolicy(Qt::NoFocus);
    return button;
}

// Tombol ikon di kepala panel, setinggi baris kepalanya
QPushButton *headerButton(const QString &icon, const char *name, const QString &tip, QWidget *parent) {
    auto *button = new QPushButton(parent);
    button->setObjectName(QLatin1String(name));
    button->setIcon(Theme::icon(icon));
    button->setIconSize(QSize(13, 13));
    button->setToolTip(tip);
    button->setCursor(Qt::PointingHandCursor);
    button->setFocusPolicy(Qt::NoFocus);
    button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Minimum);
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

// Daftar tegak yang bisa digulir; isinya disisipkan sebelum stretch penutup
QVBoxLayout *scrollList(QScrollArea *scroll, int spacing) {
    scroll->setObjectName("canvasChatScroll");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *content = new QWidget(scroll);
    content->setObjectName("canvasChatList");
    auto *list = new QVBoxLayout(content);
    list->setContentsMargins(0, 4, 6, 4);
    list->setSpacing(spacing);
    list->addStretch(1);
    scroll->setWidget(content);
    scroll->viewport()->setAutoFillBackground(false);
    content->setAutoFillBackground(false);
    return list;
}

// Semua isi daftar dibuang kecuali keep (penanda daftar kosong)
void clearList(QVBoxLayout *list, const QWidget *keep) {
    for (int i = list->count() - 1; i >= 0; --i) {
        QWidget *widget = list->itemAt(i)->widget();
        if (widget && widget != keep) {
            list->removeWidget(widget);
            delete widget;
        }
    }
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

// Baris tombol aksi sebuah jawaban: tombol yang tidak muat di panel yang sempit turun ke baris
// berikutnya, alih-alih memaksa gelembungnya lebih lebar dari panel
class ActionRow final : public QLayout {
public:
    explicit ActionRow(int spacing) {
        setContentsMargins(0, 0, 0, 0);
        setSpacing(spacing);
    }
    ~ActionRow() override { qDeleteAll(m_items); }

    void addItem(QLayoutItem *item) override { m_items.append(item); }
    int count() const override { return int(m_items.size()); }
    QLayoutItem *itemAt(int index) const override { return m_items.value(index); }
    QLayoutItem *takeAt(int index) override {
        return index >= 0 && index < m_items.size() ? m_items.takeAt(index) : nullptr;
    }
    Qt::Orientations expandingDirections() const override { return {}; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override { return arrange(QRect(0, 0, width, 0), false); }
    QSize sizeHint() const override { return minimumSize(); }
    // Selebar tombol terlebarnya: sisanya boleh turun baris
    QSize minimumSize() const override {
        QSize size;
        for (const QLayoutItem *item : m_items) {
            size = size.expandedTo(item->minimumSize());
        }
        return size;
    }
    void setGeometry(const QRect &rect) override {
        QLayout::setGeometry(rect);
        arrange(rect, true);
    }

private:
    // Tinggi yang terpakai untuk lebar rect; apply = tombolnya sekalian ditempatkan
    int arrange(const QRect &rect, bool apply) const {
        int x = rect.x();
        int y = rect.y();
        int rowHeight = 0;
        for (QLayoutItem *item : m_items) {
            const QSize size = item->sizeHint();
            if (x > rect.x() && x + size.width() > rect.x() + rect.width()) {
                x = rect.x();
                y += rowHeight + spacing();
                rowHeight = 0;
            }
            if (apply) {
                item->setGeometry(QRect(QPoint(x, y), size));
            }
            x += size.width() + spacing();
            rowHeight = qMax(rowHeight, size.height());
        }
        return y + rowHeight - rect.y();
    }

    QList<QLayoutItem *> m_items;
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

// Satu sesi di riwayat: judulnya, lalu waktu pesan terakhir dan jumlah pertanyaannya.
// Klik di mana pun pada barisnya membuka sesi itu; tombol pensil mengubah judulnya di tempat
// (Enter atau klik di luar menyimpan, Esc batal, judul kosong = kembali ke pertanyaan pertamanya).
class SessionRow final : public QFrame {
public:
    static constexpr int kMaxTitle = 120;

    SessionRow(const CanvasChatSession &session, bool current, QWidget *parent) : QFrame(parent) {
        setObjectName("canvasChatSession");
        setProperty("current", current);
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover, true);

        CanvasChatSession unnamed = session;
        unnamed.name.clear();
        m_question = unnamed.title();
        m_customName = session.name;

        m_name = new ElidedLabel(QString(), this);
        m_name->setObjectName("canvasChatSessionTitle");
        m_name->setTextFormat(Qt::PlainText);
        m_edit = new QLineEdit(this);
        m_edit->setObjectName("canvasChatSessionEdit");
        m_edit->setMaxLength(kMaxTitle);
        m_edit->setPlaceholderText(m_question);
        m_edit->setCursor(Qt::IBeamCursor);
        m_edit->installEventFilter(this);
        m_edit->hide();
        connect(m_edit, &QLineEdit::editingFinished, this, [this]() { finishEdit(true); });
        m_rename = headerButton(QStringLiteral(":/icons/edit.svg"), "btnCanvasChatRename",
                                QStringLiteral("Ubah judul sesi"), this);
        m_rename->setIconSize(QSize(12, 12));
        connect(m_rename, &QPushButton::clicked, this, [this]() { startEdit(); });
        showName();

        QStringList facts = {dayOf(session.lastAt()), QStringLiteral("%1 pertanyaan").arg(session.questionCount())};
        facts.removeAll(QString());
        if (current) {
            facts.append(QStringLiteral("sedang dibuka"));
        }
        auto *top = new QHBoxLayout();
        top->setContentsMargins(0, 0, 0, 0);
        top->setSpacing(4);
        top->addWidget(m_name, 1);
        top->addWidget(m_edit, 1);
        top->addWidget(m_rename);
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(10, 7, 6, 7);
        layout->setSpacing(2);
        layout->addLayout(top);
        layout->addWidget(label(facts.join(QStringLiteral(" · ")), "canvasChatMeta", this));
    }
    std::function<void()> clicked;
    // Judul baru dari pengguna (kosong = kembali ke pertanyaan pertamanya)
    std::function<void(const QString &)> renamed;

    bool isEditing() const { return m_edit->isVisible(); }

    void startEdit() {
        m_edit->setText(m_customName.isEmpty() ? m_question : m_customName);
        m_name->hide();
        m_rename->hide();
        m_edit->show();
        m_edit->setFocus(Qt::OtherFocusReason);
        m_edit->selectAll();
    }

protected:
    void mouseReleaseEvent(QMouseEvent *event) override {
        if (isEditing()) {
            finishEdit(true);   // klik di luar kotak judul menyimpan, tidak membuka sesi
        } else if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint()) && clicked) {
            clicked();
        }
        QFrame::mouseReleaseEvent(event);
    }

    bool eventFilter(QObject *watched, QEvent *event) override {
        if (watched == m_edit && event->type() == QEvent::KeyPress
            && static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) {
            finishEdit(false);
            return true;
        }
        return QFrame::eventFilter(watched, event);
    }

private:
    void showName() {
        const QString title = m_customName.isEmpty() ? m_question : m_customName;
        m_name->setFullText(title.isEmpty() ? QStringLiteral("(tanpa pertanyaan)") : title);
    }

    // editingFinished juga datang saat kotaknya disembunyikan: hanya yang pertama yang dipakai
    void finishEdit(bool keep) {
        if (!isEditing()) {
            return;
        }
        QString name = m_edit->text().simplified();
        if (name == m_question) {
            name.clear();   // sama dengan pertanyaan pertamanya: tetap mengikuti pertanyaan itu
        }
        m_edit->hide();
        m_name->show();
        m_rename->show();
        if (keep && name != m_customName) {
            m_customName = name;
            if (renamed) {
                renamed(name);
            }
        }
        showName();
    }

    QString m_question;     // pertanyaan pertamanya, judul bila pengguna belum memberi judul
    QString m_customName;
    ElidedLabel *m_name = nullptr;
    QLineEdit *m_edit = nullptr;
    QPushButton *m_rename = nullptr;
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

    // Kepala panel. Judul menyempit (terpotong "…") lebih dulu daripada mendorong tombol keluar dari
    // panel yang sempit; tombol kembali hanya tampil selama riwayat dibuka, tiga tombol kanan selama tidak.
    m_title = new ElidedLabel(QString(), this);
    m_title->setObjectName("canvasPanelTitle");
    m_title->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
    m_back = headerButton(QStringLiteral(":/icons/arrow-left.svg"), "btnCanvasChatBack",
                          QStringLiteral("Kembali ke percakapan"), this);
    m_clear = smallButton(QStringLiteral("Mulai ulang"), this);
    m_clear->setToolTip(QStringLiteral("Hapus seluruh percakapan sesi chat ini"));
    m_clear->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Minimum);
    m_new = headerButton(QStringLiteral(":/icons/plus-navy.svg"), "btnCanvasChatNew",
                         QStringLiteral("Sesi baru: percakapan ini tetap ada di riwayat"), this);
    m_history = headerButton(QStringLiteral(":/icons/history.svg"), "btnCanvasChatHistory",
                             QStringLiteral("Riwayat sesi chat kanvas ini"), this);
    auto *header = new QHBoxLayout();
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(4);
    header->addWidget(m_back);
    header->addWidget(m_title);
    header->addStretch(1);
    header->addWidget(m_clear);
    header->addWidget(m_new);
    header->addWidget(m_history);

    // Ruang chat: daftar pesan, bahan, dan input
    m_room = new QWidget(this);
    auto *hint = label(QStringLiteral("Tanya jawab tentang artefak di kanvas. Agent membaca kartu bahan dan kode "
                                      "project tanpa mengubah apa pun."),
                       "canvasPanelHint", m_room);
    m_scroll = new QScrollArea(m_room);
    m_list = scrollList(m_scroll, 10);

    m_empty = new QWidget(m_list->parentWidget());
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
    m_list->insertWidget(0, m_empty);

    QScrollBar *bar = m_scroll->verticalScrollBar();
    connect(bar, &QScrollBar::rangeChanged, this, [this, bar](int, int maximum) {
        if (m_stickToEnd) {
            bar->setValue(maximum);
        }
    });
    connect(bar, &QScrollBar::valueChanged, this, [this, bar](int value) { m_stickToEnd = value >= bar->maximum() - 24; });

    m_error = label(QString(), "canvasStepStatus", m_room);
    m_error->setProperty("error", true);
    m_error->hide();
    m_context = label(QString(), "canvasChatContext", m_room);
    m_context->setTextFormat(Qt::RichText);
    m_contextHint = label(QString(), "canvasPanelHint", m_room);

    auto *input = new ChatInput(m_room);
    input->submit = [this]() { send(); };
    m_input = input;
    m_input->setObjectName("canvasChatInput");
    m_input->setPlaceholderText(QStringLiteral("Tanyakan sesuatu tentang bahan ini…  (Enter kirim, Shift+Enter baris baru)"));
    m_input->setTabChangesFocus(true);
    m_input->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    connect(m_input, &QPlainTextEdit::textChanged, this, &CanvasChatPanel::fitInput);

    m_model = new QComboBox(m_room);
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
    m_send = new QPushButton(QStringLiteral("Kirim"), m_room);
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

    auto *room = new QVBoxLayout(m_room);
    room->setContentsMargins(0, 0, 0, 0);
    room->setSpacing(6);
    room->addWidget(hint);
    room->addSpacing(2);
    room->addWidget(m_scroll, 1);
    room->addWidget(m_error);
    room->addWidget(m_context);
    room->addWidget(m_contextHint);
    room->addWidget(m_input);
    room->addLayout(footer);

    // Riwayat sesi: menggantikan ruang chat selama dibuka
    m_historyPage = new QWidget(this);
    auto *sessionScroll = new QScrollArea(m_historyPage);
    m_sessions = scrollList(sessionScroll, 6);
    m_noSessions = label(QStringLiteral("Belum ada sesi chat. Sesi masuk ke sini setelah pertanyaan pertamanya dikirim."),
                         "canvasInspectorTips", m_sessions->parentWidget());
    m_sessions->insertWidget(0, m_noSessions);
    auto *history = new QVBoxLayout(m_historyPage);
    history->setContentsMargins(0, 0, 0, 0);
    history->setSpacing(6);
    history->addWidget(label(QStringLiteral("Pilih sesi untuk membuka lagi percakapannya."), "canvasPanelHint",
                             m_historyPage));
    history->addSpacing(2);
    history->addWidget(sessionScroll, 1);

    m_pages = new QStackedWidget(this);
    m_pages->addWidget(m_room);
    m_pages->addWidget(m_historyPage);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(6);
    layout->addLayout(header);
    layout->addWidget(m_pages, 1);

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
            QStringLiteral("Hapus seluruh percakapan sesi chat ini? Sesi lain di riwayat dan catatan yang sudah "
                           "dibuat dari jawaban tetap ada."));
        if (answer == QMessageBox::Yes) {
            m_chat.clear();
        }
    });
    connect(m_new, &QPushButton::clicked, this, [this]() {
        if (m_chat.newSession()) {
            m_error->hide();
            focusInput();
        }
    });
    connect(m_history, &QPushButton::clicked, this, [this]() { showHistory(true); });
    connect(m_back, &QPushButton::clicked, this, [this]() { showHistory(false); });

    setContext(CanvasBoard(), {});
    showHistory(false);
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
    showHistory(false);
    m_input->setFocus(Qt::OtherFocusReason);
    m_input->moveCursor(QTextCursor::End);
}

void CanvasChatPanel::showError(const QString &text) {
    m_error->setText(text);
    m_error->setVisible(!text.isEmpty());
    if (!text.isEmpty()) {
        showHistory(false);   // alasannya tampil di ruang chat
    }
}

QString CanvasChatPanel::draft() const {
    return m_input->toPlainText();
}

bool CanvasChatPanel::isHistoryOpen() const {
    return m_pages->currentWidget() == m_historyPage;
}

void CanvasChatPanel::showHistory(bool open) {
    if (open) {
        rebuildHistory();
    }
    m_pages->setCurrentWidget(open ? m_historyPage : m_room);
    m_title->setFullText(open ? QStringLiteral("RIWAYAT CHAT") : QStringLiteral("CHAT BRAINSTORM"));
    m_back->setVisible(open);
    m_clear->setVisible(!open);
    m_new->setVisible(!open);
    m_history->setVisible(!open);
}

void CanvasChatPanel::rebuildHistory() {
    clearList(m_sessions, m_noSessions);
    const QList<CanvasChatSession> sessions = m_chat.sessions();
    for (const CanvasChatSession &session : sessions) {
        auto *row = new SessionRow(session, session.id == m_chat.currentSessionId(), m_sessions->parentWidget());
        const QString id = session.id;
        row->clicked = [this, id]() {
            if (m_chat.openSession(id)) {
                m_error->hide();
                focusInput();
            }
        };
        row->renamed = [this, id](const QString &name) { m_chat.renameSession(id, name); };
        m_sessions->insertWidget(m_sessions->count() - 1, row);
    }
    m_noSessions->setVisible(sessions.isEmpty());
}

void CanvasChatPanel::rebuild() {
    clearList(m_list, m_empty);
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
    auto *actions = new ActionRow(4);
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
        auto *toNotion = smallButton(QStringLiteral("Ke Notion"), bubble);
        toNotion->setToolTip(QStringLiteral("Kirim pertanyaan dan jawaban ini ke Notion sebagai halaman baru"));
        connect(toNotion, &QPushButton::clicked, this, [this, answerId]() { emit exportRequested(answerId); });
        actions->addWidget(toNote);
        actions->addWidget(copy);
        actions->addWidget(expand);
        actions->addWidget(toNotion);
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

    if (busy && !m_live) {
        showHistory(false);   // jawabannya mengalir di ruang chat
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
    const bool idle = !m_chat.isBusy();
    const bool empty = m_chat.messages().isEmpty();
    m_empty->setVisible(idle && empty);
    // Sesi tidak berganti selagi agent menjawab; sesi yang masih kosong sudah sesi baru
    m_clear->setEnabled(idle && !empty);
    m_new->setEnabled(idle && !empty);
    m_history->setEnabled(idle);
}

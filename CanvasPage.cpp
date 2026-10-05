#include "CanvasPage.h"
#include "CanvasAutomation.h"
#include "CanvasChat.h"
#include "CanvasChatPanel.h"
#include "CanvasInspector.h"
#include "CanvasItems.h"
#include "CanvasLibrary.h"
#include "CanvasModel.h"
#include "CanvasView.h"
#include "Theme.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

QPushButton *toolbarButton(const QString &text, const char *name, const char *variant, QWidget *parent,
                           const QString &icon = QString()) {
    auto *button = new QPushButton(text, parent);
    button->setObjectName(QLatin1String(name));
    button->setProperty("variant", QLatin1String(variant));
    button->setCursor(Qt::PointingHandCursor);
    // Tombol tidak mengambil fokus: pintasan keyboard tetap diterima kanvas
    button->setFocusPolicy(Qt::NoFocus);
    if (!icon.isEmpty()) {
        button->setIcon(Theme::icon(icon));
        button->setIconSize(QSize(14, 14));
    }
    return button;
}

QToolButton *iconButton(const QString &icon, const char *name, const QString &tip, QWidget *parent) {
    auto *button = new QToolButton(parent);
    button->setObjectName(QLatin1String(name));
    button->setIcon(Theme::icon(icon));
    button->setIconSize(QSize(15, 15));
    button->setToolTip(tip);
    button->setCursor(Qt::PointingHandCursor);
    button->setFocusPolicy(Qt::NoFocus);
    return button;
}

QFrame *separator(QWidget *parent) {
    auto *line = new QFrame(parent);
    line->setObjectName("canvasToolbarSeparator");
    line->setFrameShape(QFrame::VLine);
    line->setFixedWidth(1);
    return line;
}

}

CanvasPage::CanvasPage(CanvasModel &model, CanvasAutomation &automation, CanvasChat &chat, MermaidRenderer *renderer,
                       QWidget *parent)
    : QWidget(parent), m_model(model), m_automation(automation), m_chat(chat) {
    setObjectName("canvasPage");

    m_view = new CanvasView(model, this);
    m_library = new CanvasLibrary(this);
    m_library->setMinimumWidth(180);
    // Panel kanan: Detail kartu terpilih atau Chat, bergantian
    m_inspector = new CanvasInspector(renderer, this);
    m_chatPanel = new CanvasChatPanel(chat, renderer, this);
    m_side = new QStackedWidget(this);
    m_side->setObjectName("canvasSidePanel");
    m_side->addWidget(m_inspector);
    m_side->addWidget(m_chatPanel);
    m_side->setMinimumWidth(260);

    auto *toolbar = new QWidget(this);
    toolbar->setObjectName("canvasToolbar");
    toolbar->setAttribute(Qt::WA_StyledBackground, true);
    auto *board = toolbarButton(QStringLiteral("Board"), "btnCanvasBoard", "secondary", toolbar, QStringLiteral(":/icons/board.svg"));
    board->setToolTip(QStringLiteral("Kembali ke board kanban project ini"));
    auto *title = new QLabel(QStringLiteral("Kanvas · %1").arg(model.projectId()), toolbar);
    title->setObjectName("canvasTitle");
    auto *addNote = toolbarButton(QStringLiteral("Catatan"), "btnCanvasAddNote", "secondary", toolbar,
                                  QStringLiteral(":/icons/note.svg"));
    addNote->setToolTip(QStringLiteral("Catatan baru di tengah tampilan (N, atau klik dua kali di ruang kosong)"));
    auto *addStep = new QToolButton(toolbar);
    addStep->setObjectName("btnCanvasAddStep");
    addStep->setText(QStringLiteral("Langkah AI"));
    addStep->setIcon(Theme::icon(QStringLiteral(":/icons/spark.svg")));
    addStep->setIconSize(QSize(14, 14));
    addStep->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    addStep->setPopupMode(QToolButton::MenuButtonPopup);
    addStep->setCursor(Qt::PointingHandCursor);
    addStep->setFocusPolicy(Qt::NoFocus);
    addStep->setToolTip(QStringLiteral("Langkah AI baru (L): agent mengolah kartu yang tersambung ke langkah ini"));
    auto *stepMenu = new QMenu(addStep);
    stepMenu->setObjectName("canvasContextMenu");
    QAction *documentStep = stepMenu->addAction(QStringLiteral("Langkah AI: dokumen Markdown"));
    QAction *tasksStep = stepMenu->addAction(QStringLiteral("Langkah AI: pecah jadi task untuk pipeline"));
    addStep->setMenu(stepMenu);

    m_runAll = toolbarButton(QStringLiteral("▶ Jalankan alur"), "btnCanvasRunAll", "primary", toolbar);
    m_runAll->setToolTip(QStringLiteral("Jalankan semua langkah AI berurutan: langkah hulu dulu, hasilnya jadi bahan langkah berikutnya"));
    m_stopAll = toolbarButton(QStringLiteral("■ Hentikan"), "btnCanvasStopAll", "secondary", toolbar);
    m_stopAll->setToolTip(QStringLiteral("Hentikan semua langkah AI yang antre atau berjalan"));
    m_undo = iconButton(QStringLiteral(":/icons/undo.svg"), "btnCanvasUndo", QStringLiteral("Urungkan (Ctrl+Z)"), toolbar);
    m_redo = iconButton(QStringLiteral(":/icons/redo.svg"), "btnCanvasRedo", QStringLiteral("Ulangi (Ctrl+Y)"), toolbar);

    m_status = new QLabel(toolbar);
    m_status->setObjectName("canvasStatus");
    m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    auto *zoomOut = toolbarButton(QStringLiteral("−"), "btnCanvasZoomOut", "secondary", toolbar);
    zoomOut->setToolTip(QStringLiteral("Perkecil (−)"));
    zoomOut->setFixedSize(28, 26);
    m_zoomLabel = new QLabel(QStringLiteral("100%"), toolbar);
    m_zoomLabel->setObjectName("canvasZoomLevel");
    m_zoomLabel->setAlignment(Qt::AlignCenter);
    auto *zoomIn = toolbarButton(QStringLiteral("+"), "btnCanvasZoomIn", "secondary", toolbar);
    zoomIn->setToolTip(QStringLiteral("Perbesar (+)"));
    zoomIn->setFixedSize(28, 26);
    auto *fit = toolbarButton(QStringLiteral("Paskan"), "btnCanvasFit", "secondary", toolbar);
    fit->setToolTip(QStringLiteral("Tampilkan semua kartu (Ctrl+0)"));
    m_toggleLibrary = toolbarButton(QStringLiteral("Pustaka"), "btnCanvasToggleLibrary", "toggle", toolbar);
    m_toggleLibrary->setCheckable(true);
    m_toggleLibrary->setChecked(true);
    m_toggleLibrary->setToolTip(QStringLiteral("Tampilkan/sembunyikan pustaka artefak"));
    m_toggleInspector = toolbarButton(QStringLiteral("Detail"), "btnCanvasToggleInspector", "toggle", toolbar);
    m_toggleInspector->setCheckable(true);
    m_toggleInspector->setChecked(true);
    m_toggleInspector->setToolTip(QStringLiteral("Tampilkan/sembunyikan detail kartu terpilih"));
    m_toggleChat = toolbarButton(QStringLiteral("Chat"), "btnCanvasToggleChat", "toggle", toolbar);
    m_toggleChat->setCheckable(true);
    m_toggleChat->setToolTip(QStringLiteral("Tanya jawab dengan agent tentang kartu terpilih atau seluruh kanvas (C)"));

    auto *tools = new QHBoxLayout(toolbar);
    tools->setContentsMargins(10, 6, 10, 6);
    tools->setSpacing(6);
    tools->addWidget(board);
    tools->addWidget(title);
    tools->addSpacing(4);
    tools->addWidget(separator(toolbar));
    tools->addWidget(addNote);
    tools->addWidget(addStep);
    tools->addWidget(separator(toolbar));
    tools->addWidget(m_runAll);
    tools->addWidget(m_stopAll);
    tools->addWidget(separator(toolbar));
    tools->addWidget(m_undo);
    tools->addWidget(m_redo);
    tools->addSpacing(6);
    tools->addWidget(m_status, 1);
    tools->addWidget(zoomOut);
    tools->addWidget(m_zoomLabel);
    tools->addWidget(zoomIn);
    tools->addWidget(fit);
    tools->addWidget(separator(toolbar));
    tools->addWidget(m_toggleLibrary);
    tools->addWidget(m_toggleInspector);
    tools->addWidget(m_toggleChat);

    m_splitter = new QSplitter(Qt::Horizontal, this);
    m_splitter->setObjectName("canvasSplitter");
    m_splitter->setHandleWidth(6);
    m_splitter->setChildrenCollapsible(false);
    m_splitter->addWidget(m_library);
    m_splitter->addWidget(m_view);
    m_splitter->addWidget(m_side);
    m_splitter->setStretchFactor(1, 1);
    m_splitter->setSizes({220, 780, 320});

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(8);
    root->addWidget(toolbar);
    root->addWidget(m_splitter, 1);

    m_inspectorTimer.setSingleShot(true);
    m_inspectorTimer.setInterval(0);
    connect(&m_inspectorTimer, &QTimer::timeout, this, &CanvasPage::refreshInspector);
    m_statusTimer.setSingleShot(true);
    m_statusTimer.setInterval(7000);
    connect(&m_statusTimer, &QTimer::timeout, m_status, &QLabel::clear);

    // Toolbar
    connect(board, &QPushButton::clicked, this, &CanvasPage::boardRequested);
    connect(addNote, &QPushButton::clicked, this, [this]() {
        const QSizeF size = CanvasNode::defaultSize(CanvasNodeKind::Note);
        m_view->addNoteAt(m_view->centerScenePos() - QPointF(size.width() / 2, size.height() / 2), true);
    });
    auto addStepAtCenter = [this](CanvasStepOutput output) {
        const QSizeF size = CanvasNode::defaultSize(CanvasNodeKind::Step);
        m_view->addStepAt(m_view->centerScenePos() - QPointF(size.width() / 2, size.height() / 2), output);
        showPanel(SidePanel::Detail);
    };
    connect(addStep, &QToolButton::clicked, this, [addStepAtCenter]() { addStepAtCenter(CanvasStepOutput::Document); });
    connect(documentStep, &QAction::triggered, this, [addStepAtCenter]() { addStepAtCenter(CanvasStepOutput::Document); });
    connect(tasksStep, &QAction::triggered, this, [addStepAtCenter]() { addStepAtCenter(CanvasStepOutput::Tasks); });
    connect(m_runAll, &QPushButton::clicked, this, &CanvasPage::runAllRequested);
    connect(m_stopAll, &QPushButton::clicked, this, [this]() { m_automation.cancelAll(); });
    connect(m_undo, &QToolButton::clicked, this, [this]() { m_model.undo(); });
    connect(m_redo, &QToolButton::clicked, this, [this]() { m_model.redo(); });
    connect(zoomOut, &QPushButton::clicked, m_view, &CanvasView::zoomOut);
    connect(zoomIn, &QPushButton::clicked, m_view, &CanvasView::zoomIn);
    connect(fit, &QPushButton::clicked, m_view, &CanvasView::fitAll);
    connect(m_toggleLibrary, &QPushButton::toggled, m_library, &QWidget::setVisible);
    // Tombol panel yang aktif menutup panel kanan; tombol lainnya berpindah panel
    connect(m_toggleInspector, &QPushButton::clicked, this, [this](bool checked) {
        checked ? showPanel(SidePanel::Detail) : hidePanel();
    });
    connect(m_toggleChat, &QPushButton::clicked, this, [this](bool checked) {
        if (checked) {
            openChat();
        } else {
            hidePanel();
        }
    });

    // Kanvas
    connect(m_view, &CanvasView::zoomChanged, this, [this](qreal zoom) {
        m_zoomLabel->setText(QStringLiteral("%1%").arg(qRound(zoom * 100)));
    });
    connect(m_view, &CanvasView::selectionChanged, this, &CanvasPage::scheduleInspectorRefresh);
    connect(m_view, &CanvasView::nodeActivated, this, [this](const QString &id) {
        showPanel(SidePanel::Detail);
        if (m_view->selectedNodeIds() != QStringList{id}) {
            m_view->selectNodes({id});
        }
        scheduleInspectorRefresh();
    });
    connect(m_view, &CanvasView::message, this, &CanvasPage::showMessage);
    connect(m_view, &CanvasView::sourcesDropped, this, [this](const QList<CanvasSource> &sources, const QPointF &scenePos) {
        emit sourcesDropped(sources, scenePos, true);
    });
    connect(m_view, &CanvasView::runRequested, this, &CanvasPage::runRequested);
    connect(m_view, &CanvasView::createTaskRequested, this, &CanvasPage::createTaskRequested);
    connect(m_view, &CanvasView::openTaskRequested, this, &CanvasPage::openTaskRequested);
    connect(m_view, &CanvasView::chatRequested, this, [this](const QStringList &ids) {
        if (m_view->selectedNodeIds() != ids) {
            m_view->selectNodes(ids);
        }
        openChat();
    });
    connect(m_library, &CanvasLibrary::sourceActivated, this, [this](const CanvasSource &source) {
        emit sourcesDropped({source}, m_view->centerScenePos(), false);
    });

    // Model & run langkah AI
    connect(&m_model, &CanvasModel::nodeChanged, this, &CanvasPage::scheduleInspectorRefresh);
    connect(&m_model, &CanvasModel::nodeRemoved, this, &CanvasPage::scheduleInspectorRefresh);
    connect(&m_model, &CanvasModel::edgeAdded, this, &CanvasPage::scheduleInspectorRefresh);
    connect(&m_model, &CanvasModel::edgeRemoved, this, &CanvasPage::scheduleInspectorRefresh);
    connect(&m_model, &CanvasModel::boardReset, this, &CanvasPage::scheduleInspectorRefresh);
    connect(&m_model, &CanvasModel::undoStateChanged, this, &CanvasPage::refreshToolbar);
    connect(&m_automation, &CanvasAutomation::stateChanged, this, [this](const QString &stepId, RunState state) {
        m_view->setRunState(stepId, state);
        refreshToolbar();
        scheduleInspectorRefresh();
    });

    // Detail kartu
    connect(m_inspector, &CanvasInspector::textCommitted, this, [this](const QString &id, const QString &text) {
        m_model.setText(id, text);
    });
    connect(m_inspector, &CanvasInspector::colorChosen, this, [this](const QString &id, const QString &color) {
        m_model.setColor(id, color);
    });
    connect(m_inspector, &CanvasInspector::stepOptionsChosen, this,
            [this](const QString &id, const QString &model, const QString &effort, CanvasStepOutput output) {
        m_model.setStepOptions(id, model, effort, output);
    });
    connect(m_inspector, &CanvasInspector::runRequested, this, [this](const QString &id, bool withUpstream) {
        emit runRequested({id}, withUpstream);
    });
    connect(m_inspector, &CanvasInspector::cancelRequested, this, [this](const QString &id) { m_automation.cancel(id); });
    connect(m_inspector, &CanvasInspector::noteFromResultRequested, this, &CanvasPage::noteFromResult);
    connect(m_inspector, &CanvasInspector::createTaskRequested, this, &CanvasPage::createTaskRequested);
    connect(m_inspector, &CanvasInspector::proposalsRequested, this, &CanvasPage::proposalsRequested);
    connect(m_inspector, &CanvasInspector::openTaskRequested, this, &CanvasPage::openTaskRequested);
    connect(m_inspector, &CanvasInspector::removeRequested, this, [this](const QStringList &ids) {
        m_model.removeNodes(ids);
    });

    // Chat
    connect(m_chatPanel, &CanvasChatPanel::askRequested, this, &CanvasPage::chatAskRequested);
    connect(m_chatPanel, &CanvasChatPanel::noteRequested, this, &CanvasPage::noteFromChat);
    connect(m_chatPanel, &CanvasChatPanel::contextActivated, this, [this](const QStringList &ids) {
        QStringList existing;
        for (const QString &id : ids) {
            if (m_model.node(id)) {
                existing.append(id);
            }
        }
        if (existing.isEmpty()) {
            showMessage(QStringLiteral("Kartu bahan pertanyaan itu sudah tidak ada di kanvas"), true);
            return;
        }
        m_view->selectNodes(existing);
        m_view->centerOnNode(existing.first());
    });
    // Jawaban yang datang selagi panel chat tertutup ditandai di tombolnya
    connect(&m_chat, &CanvasChat::finished, this, [this]() {
        if (!isChatOpen()) {
            m_toggleChat->setText(QStringLiteral("Chat •"));
        }
    });

    refreshToolbar();
    refreshInspector();
}

CanvasPage::~CanvasPage() {
    // Anak halaman dibongkar QWidget sesudah anggota halaman ini hilang: sinyal mereka, juga sinyal
    // model dan automation yang hidup lebih lama, tidak boleh lagi sampai ke halaman yang setengah hancur
    m_view->blockSignals(true);
    m_inspector->blockSignals(true);
    m_chatPanel->blockSignals(true);
    m_library->blockSignals(true);
    disconnect(&m_model, nullptr, this, nullptr);
    disconnect(&m_automation, nullptr, this, nullptr);
    disconnect(&m_chat, nullptr, this, nullptr);
}

QString CanvasPage::projectId() const {
    return m_model.projectId();
}

void CanvasPage::setLibraryTasks(const QList<TaskItem> &tasks, const QStringList &stageOrder) {
    m_library->setTasks(tasks, m_model.projectId(), stageOrder);
}

void CanvasPage::setLiveOutput(const QString &stepId, const QString &markdown) {
    if (markdown.isEmpty()) {
        m_live.remove(stepId);
        return;
    }
    m_live.insert(stepId, markdown);
    if (m_inspector->nodeId() == stepId) {
        m_inspector->setLiveOutput(markdown);
    }
}

void CanvasPage::showMessage(const QString &text, bool error) {
    m_status->setText(text);
    m_status->setToolTip(text);
    if (m_status->property("error").toBool() != error) {
        m_status->setProperty("error", error);
        m_status->style()->unpolish(m_status);
        m_status->style()->polish(m_status);
    }
    m_statusTimer.start();
}

void CanvasPage::storeView() {
    // Dipanggil saat halaman disembunyikan, project ditutup, dan aplikasi keluar (sebelum simpan
    // terakhir): ketikan yang belum tersimpan ikut, jangan menunggu fokus pindah
    m_view->finishEditing();
    m_inspector->commitText();
    m_model.setView(m_view->centerScenePos(), m_view->zoom());
    emit viewStored();
}

void CanvasPage::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);
    if (m_viewRestored) {
        return;
    }
    m_viewRestored = true;
    // Ukuran viewport baru final setelah tata letak pertama
    QTimer::singleShot(0, this, [this]() {
        const CanvasBoard &board = m_model.board();
        m_view->restoreView(board.viewCenter, board.zoom);
        m_view->setFocus(Qt::OtherFocusReason);
    });
}

void CanvasPage::hideEvent(QHideEvent *event) {
    if (m_viewRestored) {
        storeView();
    }
    QWidget::hideEvent(event);
}

void CanvasPage::scheduleInspectorRefresh() {
    // Banyak sinyal model bisa datang beruntun (macro, undo): detail cukup disegarkan sekali
    m_inspectorTimer.start();
}

void CanvasPage::refreshInspector() {
    const QStringList ids = m_view->selectedNodeIds();
    const QString single = ids.size() == 1 ? ids.first() : QString();
    m_inspector->showSelection(m_model.board(), ids, single.isEmpty() ? RunState::Idle : m_automation.state(single),
                               m_live.value(single));
    m_chatPanel->setContext(m_model.board(), ids);
}

void CanvasPage::refreshToolbar() {
    m_undo->setEnabled(m_model.canUndo());
    m_redo->setEnabled(m_model.canRedo());
    m_stopAll->setEnabled(m_automation.isBusy());
}

void CanvasPage::noteFromResult(const QString &stepId) {
    const CanvasNode *step = m_model.node(stepId);
    if (!step || !step->result.success) {
        return;
    }
    const QSizeF size(320, 260);
    const QPointF pos = m_model.board().openSpot(QRectF(step->pos + QPointF(step->size.width() + 100, 0), size));
    const QString text = step->result.message.trimmed();
    m_model.beginMacro();
    const QString note = m_model.addNote(pos, text, QStringLiteral("sky"));
    m_model.resizeNode(note, size);
    m_model.connectNodes(stepId, note);
    m_model.endMacro();
    m_view->selectNodes({note});
    showMessage(QStringLiteral("Hasil disalin ke catatan baru yang bisa diedit"));
}

void CanvasPage::showPanel(SidePanel panel) {
    m_side->setCurrentWidget(panel == SidePanel::Chat ? static_cast<QWidget *>(m_chatPanel) : m_inspector);
    m_side->show();
    m_toggleInspector->setChecked(panel == SidePanel::Detail);
    m_toggleChat->setChecked(panel == SidePanel::Chat);
    if (panel == SidePanel::Chat) {
        m_toggleChat->setText(QStringLiteral("Chat"));
    }
}

void CanvasPage::hidePanel() {
    m_side->hide();
    m_toggleInspector->setChecked(false);
    m_toggleChat->setChecked(false);
}

bool CanvasPage::isChatOpen() const {
    return m_side->isVisible() && m_side->currentWidget() == m_chatPanel;
}

void CanvasPage::openChat() {
    showPanel(SidePanel::Chat);
    m_chatPanel->setContext(m_model.board(), m_view->selectedNodeIds());
    m_chatPanel->focusInput();
}

void CanvasPage::showChatError(const QString &reason) {
    if (!isChatOpen()) {
        showPanel(SidePanel::Chat);
    }
    m_chatPanel->showError(reason);
}

void CanvasPage::noteFromChat(const QString &answerId) {
    const CanvasChatMessage *answer = m_chat.message(answerId);
    if (!answer || answer->text.trimmed().isEmpty()) {
        return;
    }
    const CanvasChatMessage *question = m_chat.message(answer->replyTo);
    QString text = answer->text.trimmed();
    QStringList sources;
    if (question) {
        // Pertanyaannya jadi baris pertama: judul catatan di kanvas
        QString asked = question->text.simplified();
        if (asked.size() > 200) {
            asked = asked.left(199).trimmed() + QChar(0x2026);
        }
        text = QStringLiteral("> %1\n\n%2").arg(asked, text);
        for (const QString &id : question->contextIds) {
            if (m_model.node(id)) {
                sources.append(id);
            }
        }
    }

    // Di kanan kartu bahannya, atau di tengah tampilan bila bahannya seluruh kanvas
    const QSizeF size(320, 260);
    QPointF anchor = m_view->centerScenePos() - QPointF(size.width() / 2, size.height() / 2);
    if (!sources.isEmpty()) {
        QRectF bounds;
        for (const QString &id : std::as_const(sources)) {
            const QRectF rect = m_model.node(id)->rect();
            bounds = bounds.isNull() ? rect : bounds.united(rect);
        }
        anchor = QPointF(bounds.right() + 100, bounds.top());
    }
    const QPointF pos = m_model.board().openSpot(QRectF(anchor, size));
    m_model.beginMacro();
    const QString note = m_model.addNote(pos, text, QStringLiteral("sky"));
    m_model.resizeNode(note, size);
    // Garis dari bahannya, selama masih terbaca (bukan puluhan garis)
    if (sources.size() <= 6) {
        for (const QString &id : std::as_const(sources)) {
            m_model.connectNodes(id, note);
        }
    }
    m_model.endMacro();
    m_view->selectNodes({note});
    m_view->centerOnNode(note);
    showMessage(QStringLiteral("Jawaban chat disalin ke catatan baru di kanvas"));
}

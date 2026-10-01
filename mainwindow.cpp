#include "MainWindow.h"
#include "ui_MainWindow.h"
#include "AgentAccess.h"
#include "BranchViewer.h"
#include "GitHistory.h"
#include "SwimlaneWidget.h"
#include "KanbanColumnWidget.h"
#include "KanbanCardWidget.h"
#include "TaskItem.h"
#include "ConsolePanelWidget.h"
#include "AppFonts.h"
#include "ElidedLabel.h"
#include "FileManager.h"
#include "FolderLauncher.h"
#include "FontPickerDialog.h"
#include "IntegrationsDialog.h"
#include "NewTaskDialog.h"
#include "ResponseDrawer.h"
#include "RunLogFormatter.h"
#include "RuntimeNoticeDialog.h"
#include "SplitterPaneAnimator.h"
#include "StageCatalog.h"
#include "SwarmCoordinator.h"
#include "TaskAttachments.h"
#include "TaskGit.h"
#include "TaskManager.h"
#include "WorkspaceDiff.h"
#include "Theme.h"

#include <QFileDialog>
#include <QFileInfo>
#include <QThreadPool>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>
#include <QInputDialog>
#include <QAbstractAnimation>
#include <QAction>
#include <QCursor>
#include <QEasingCurve>
#include <QEvent>
#include <QFont>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QApplication>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSize>
#include <QScreen>
#include <QSplitter>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QTimer>
#include <QVariantAnimation>
#include <QDir>

#include <algorithm>

namespace {

QString badgeText(const TaskItem &task) {
    const int sentBack = task.sentBackCount();
    return sentBack > 0 ? QString("✓ %1 · ↺ %2").arg(task.approvedGates()).arg(sentBack)
                        : QString("✓ %1").arg(task.approvedGates());
}

// Path yang sama atau berada di dalam root (tanpa beda huruf besar/kecil di Windows)
bool isSameOrInside(const QString &path, const QString &root) {
    const QString a = QDir::cleanPath(QDir(path).absolutePath());
    const QString b = QDir::cleanPath(QDir(root).absolutePath());
    const Qt::CaseSensitivity cs = QDir::separator() == QLatin1Char('\\') ? Qt::CaseInsensitive : Qt::CaseSensitive;
    return a.compare(b, cs) == 0 || a.startsWith(b.endsWith(QLatin1Char('/')) ? b : b + QLatin1Char('/'), cs);
}

// Alasan run terakhir gagal, untuk tooltip "Coba lagi" di kartu
QString failureDetail(const TaskItem &task) {
    const StageRun *run = task.latestRun(task.stage);
    if (task.state != TaskState::Failed || !run) {
        return QString();
    }
    QString detail = run->result.outcome;
    if (!run->result.message.isEmpty()) {
        detail += QStringLiteral(": ") + run->result.message.left(160);
    }
    return detail;
}

// Lebar rel yang tersisa saat sidebar disembunyikan: tombol sidebar (22px) + 8px kiri-kanan
constexpr int kSidebarRailWidth = 38;
// Selang pemeriksaan kursor selama sidebar tampil sementara
constexpr int kSidebarPeekPollMs = 120;

}

MainWindow::MainWindow(const StageCatalog &catalog, TaskManager &tasks, SwarmCoordinator &swarm,
                       MermaidRenderer &mermaid, QWidget *parent)
    : QMainWindow(parent),
    ui(new Ui::MainWindow),
    m_catalog(catalog),
    m_tasks(tasks),
    m_swarm(swarm) {
    m_fileManager = new FileManager(this);
    ui->setupUi(this);

    // Semua kejadian run agent (dari gerombolan stage mana pun) lewat coordinator
    connect(&m_swarm, &SwarmCoordinator::runQueued, this, &MainWindow::handleRunQueued);
    connect(&m_swarm, &SwarmCoordinator::runStarted, this, &MainWindow::handleRunStarted);
    connect(&m_swarm, &SwarmCoordinator::runEvent, this, &MainWindow::handleRunEvent);
    connect(&m_swarm, &SwarmCoordinator::runFinished, this, &MainWindow::handleRunFinished);

    // Kartu dan file sesi mengikuti TaskManager, satu-satunya pemilik data task
    connect(&m_tasks, &TaskManager::taskAdded, this, &MainWindow::handleTaskAdded);
    connect(&m_tasks, &TaskManager::taskChanged, this, &MainWindow::handleTaskChanged);
    connect(&m_tasks, &TaskManager::taskMoved, this, &MainWindow::handleTaskMoved);
    connect(&m_tasks, &TaskManager::taskMoveRejected, this, &MainWindow::handleTaskMoveRejected);
    connect(&m_tasks, &TaskManager::taskRemoved, this, &MainWindow::handleTaskRemoved);

    // Drawer hasil agent: splitter bersarang [board | drawer] menggantikan boardStack di
    // mainSplitter, sehingga mainSplitter tetap tiga pane dan animasi sidebar (indeks 0/1/2) aman
    m_boardSplitter = new QSplitter(Qt::Horizontal);
    m_boardSplitter->setObjectName("boardSplitter");
    m_boardSplitter->setHandleWidth(6);
    m_boardSplitter->setChildrenCollapsible(false);
    ui->mainSplitter->replaceWidget(1, m_boardSplitter);
    m_boardSplitter->addWidget(ui->boardStack);
    m_drawer = new ResponseDrawer(&mermaid, m_boardSplitter);
    m_boardSplitter->addWidget(m_drawer);
    m_boardSplitter->setStretchFactor(0, 1);
    m_drawer->hide();
    m_drawerAnimator = new SplitterPaneAnimator(m_boardSplitter, 1, this);

    connect(m_drawer, &ResponseDrawer::closeRequested, m_drawerAnimator, &SplitterPaneAnimator::close);
    // Dua arah: tombol expand menggerakkan animator, dan animator (mis. saat drawer ditutup
    // dari luar) mengembalikan ikon tombol
    connect(m_drawer, &ResponseDrawer::expandToggled, m_drawerAnimator, &SplitterPaneAnimator::setExpanded);
    connect(m_drawerAnimator, &SplitterPaneAnimator::expandedChanged, m_drawer, &ResponseDrawer::setExpanded);
    connect(m_drawer, &ResponseDrawer::approveRequested, this, &MainWindow::handleApproveRequested);
    connect(m_drawer, &ResponseDrawer::revisionRequested, this, &MainWindow::handleRevisionRequested);
    connect(m_drawer, &ResponseDrawer::sendBackRequested, this, &MainWindow::handleSendBackRequested);
    connect(m_drawer, &ResponseDrawer::diffRequested, this, &MainWindow::handleDiffRequested);
    connect(m_drawer, &ResponseDrawer::branchHistoryRequested, this, [this](const QString &taskId) {
        const std::optional<TaskItem> task = m_tasks.task(taskId);
        if (task && !task->branch.isEmpty()) {
            showBranchViewer(task->projectId, task->branch.name, task->branch.base);
        }
    });

    // Simpan lebar minimum asli sidebar (dari .ui) sebelum animasi bisa mengubahnya
    m_sidebarMinWidth = ui->sidebarPanel->minimumWidth();
    m_sidebarMaxWidth = ui->sidebarPanel->maximumWidth();
    setupSidebarRail();

    setupMenuBar();
    QTimer::singleShot(0, this, [this]() {
        // sidebar : board : console, boleh disesuaikan
        ui->mainSplitter->setSizes({180, 820, 350});
    });

    // Kartu task di konsol diklik: tampilkan project-nya dan buka hasil agent task itu,
    // sama seperti klik kartunya di board
    connect(ui->consolePanel, &ConsolePanelWidget::taskActivated, this, [this](const QString &taskId) {
        const std::optional<TaskItem> task = m_tasks.task(taskId);
        if (!task) return;
        setActiveProject(task->projectId);
        openDrawer(taskId);
    });

    // Jarak antar baris project di sidebar (spacing murni, bukan margin item,
    // supaya kotak hover/selected tetap pas dengan tinggi widget-nya)
    ui->projectList->setSpacing(4);
    // Baris mengikuti lebar daftar. Saat sidebar menyempit setelah tampil pertama (ukuran splitter
    // awal), QListView menghitung ulang kotak item tapi widget baris tetap selebar semula, sehingga
    // tombol hapus dan "+" baru terlihat setelah daftar digeser (lihat eventFilter).
    ui->projectList->viewport()->installEventFilter(this);

    // Pemilihan project di sidebar menentukan apa yang tampil di board
    connect(ui->projectList, &QListWidget::currentItemChanged,
            this, &MainWindow::handleProjectSelected);

    // Tombol sidebar di pojok kanan atas panel, sejajar judul PROJECTS; ikon dari .ui diganti versi
    // yang ikut tema
    ui->btnToggleSidebar->setIcon(Theme::icon(":/icons/sidebar.svg"));
    connect(ui->btnToggleSidebar, &QPushButton::clicked,
            this, &MainWindow::toggleSidebar);

    // Prioritaskan data tersimpan; mock hanya dipakai bila belum ada project di disk
    if (loadProjectsFromDisk() == 0) {
        loadInitialMockData();
    }

    // Fokus ke project pertama; bila belum ada project, tampilkan empty state
    if (ui->projectList->count() > 0) {
        ui->projectList->setCurrentRow(0);
    } else {
        ui->boardStack->setCurrentIndex(0);
    }
}

void MainWindow::setupMenuBar() {
    // Menu utama di baris paling atas jendela, mepet pojok kiri atas.
    // Yang tersambung: File > New / Close / Remove Project / Integrations / Preferences dan semua
    // item View; item lainnya belum ada aksinya.
    auto *menuBar = new QMenuBar(this);
    menuBar->setObjectName("topMenuBar");
    setMenuBar(menuBar);

    auto addMenu = [menuBar](const QString &title) {
        QMenu *menu = menuBar->addMenu(title);
        menu->setObjectName("topMenu");
        return menu;
    };

    QMenu *file = addMenu("&File");
    QAction *newProject = file->addAction("New Project", this, &MainWindow::handleNewProjectRequested);
    newProject->setObjectName("actionNewProject");
    // Id dioper sebagai salinan: slot-slot ini menerima referensi, dan membuang project
    // mengosongkan m_activeProjectId di tengah jalan (id kosong = log salah, dan deleteProject("")
    // menghapus folder projects/ seluruhnya)
    m_actionCloseProject = file->addAction("Close Project", this, [this]() {
        handleCloseProjectRequested(QString(m_activeProjectId));
    });
    m_actionCloseProject->setObjectName("actionCloseProject");
    m_actionRemoveProject = file->addAction("Remove Project", this, [this]() {
        confirmRemoveProject(QString(m_activeProjectId));
    });
    m_actionRemoveProject->setObjectName("actionRemoveProject");
    // Keduanya bekerja pada project yang sedang tampil: mati selama board kosong. Dihitung tiap
    // menu dibuka, jadi tidak ada tempat lain yang perlu ingat memperbaruinya.
    connect(file, &QMenu::aboutToShow, this, [this]() {
        const bool hasProject = !m_activeProjectId.isEmpty();
        m_actionCloseProject->setEnabled(hasProject);
        m_actionRemoveProject->setEnabled(hasProject);
    });
    file->addSeparator();
    // Cara run agent masuk ke Claude: login Claude Code (OAuth) atau API key Anthropic
    QAction *integrations = file->addAction("Integrations…", this, &MainWindow::showIntegrations);
    integrations->setObjectName("actionIntegrations");
    // Pengaturan aplikasi (sementara: ganti font antarmuka)
    QAction *preferences = file->addAction("Preferences…");
    preferences->setObjectName("actionPreferences");
    connect(preferences, &QAction::triggered, this, &MainWindow::showFontPicker);
    file->addSeparator();
    file->addAction("Exit");

    // Jendela branch & commit project yang tampil, sama dengan tombol branch di header swimlane.
    // Seperti tombol itu, hanya berlaku bila folder kerjanya repository git.
    QMenu *view = addMenu("&View");
    m_actionSourceControl = view->addAction("Source Control", this, [this]() {
        showBranchViewer(m_activeProjectId);
    });
    m_actionSourceControl->setObjectName("actionSourceControl");
    // Folder kerja project yang tampil: buka di File Explorer atau di terminal, dan ganti foldernya
    // (sama dengan tombol folder di header swimlane). Id dioper sebagai salinan, lihat File di atas.
    m_actionShowInExplorer = view->addAction("Show in Explorer", this, [this]() {
        revealWorkingDirectory(QString(m_activeProjectId), FolderTarget::Explorer);
    });
    m_actionShowInExplorer->setObjectName("actionShowInExplorer");
    m_actionShowInTerminal = view->addAction("Show in Terminal", this, [this]() {
        revealWorkingDirectory(QString(m_activeProjectId), FolderTarget::Terminal);
    });
    m_actionShowInTerminal->setObjectName("actionShowInTerminal");
    view->addSeparator();
    m_actionChangeFolder = view->addAction("Change Folder…", this, [this]() {
        chooseWorkingDirectory(QString(m_activeProjectId));
    });
    m_actionChangeFolder->setObjectName("actionChangeFolder");
    connect(view, &QMenu::aboutToShow, this, [this]() {
        const bool hasProject = !m_activeProjectId.isEmpty();
        const QString dir = hasProject ? m_fileManager->workingDirectory(m_activeProjectId) : QString();
        const bool hasFolder = !dir.isEmpty() && QDir(dir).exists();
        m_actionSourceControl->setEnabled(hasProject && TaskGit::looksLikeRepository(dir));
        m_actionShowInExplorer->setEnabled(hasFolder);
        m_actionShowInTerminal->setEnabled(hasFolder);
        m_actionChangeFolder->setEnabled(hasProject);
    });

    QMenu *help = addMenu("&Help");
    help->addAction("Report Issue");
    help->addAction("Tutorial (Tips && Tricks)");
    help->addSeparator();
    help->addAction("About");
}

int MainWindow::loadProjectsFromDisk() {
    const QStringList projectIds = m_fileManager->listProjectIds();

    for (const QString &projectId : projectIds) {
        QString error;
        const QList<TaskItem> tasks = m_fileManager->loadTasks(projectId, &error);
        if (!error.isEmpty()) {
            ui->consolePanel->appendLog(QString("[LOAD WARN] %1: %2").arg(projectId, error));
        }

        addSwimlane(projectId);
        auto *swimlane = m_swimlanes.value(projectId, nullptr);
        if (!swimlane) continue;

        // Kartu dibuat oleh handleTaskAdded(); file yang baru dibaca tidak perlu ditulis ulang
        m_loading = true;
        for (const TaskItem &task : tasks) {
            if (!swimlane->column(task.stage)) {
                ui->consolePanel->appendLog(QString("[LOAD WARN] %1: stage '%2' tidak dikenal, task '%3' dilewati")
                                                .arg(projectId, task.stage, task.title));
                continue;
            }
            TaskItem item = task;
            item.projectId = projectId;   // folder project yang menentukan, bukan isi file
            m_tasks.addTask(item);
        }
        m_loading = false;
    }

    if (!projectIds.isEmpty()) {
        ui->consolePanel->appendLog("--- SYSTEM INITIALIZED ---");
        ui->consolePanel->appendLog("Projects loaded: " + projectIds.join(", "));
    }
    return projectIds.size();
}

MainWindow::~MainWindow() {
    // Perubahan yang masih menunggu debounce (mis. hasil run barusan) jangan sampai hilang
    m_fileManager->flushPendingSaves();
    delete ui;
}

void MainWindow::saveProject(const QString &projectId) {
    m_fileManager->scheduleSave(projectId, m_tasks.tasksForProject(projectId));
}

void MainWindow::addSwimlane(const QString &projectId) {
    if (m_swimlanes.contains(projectId)) return;

    auto *swimlane = new SwimlaneWidget(projectId, m_catalog, this);
    swimlane->setWorkingDirectory(m_fileManager->workingDirectory(projectId));
    swimlane->setReferenceDirectories(m_fileManager->referenceDirectories(projectId));

    // Tangkap interaksi dari swimlane
    connect(swimlane, &SwimlaneWidget::cardMoved, this, &MainWindow::handleCardMoved);
    connect(swimlane, &SwimlaneWidget::closeProjectRequested, this, &MainWindow::handleCloseProjectRequested);
    connect(swimlane, &SwimlaneWidget::workingDirectoryChangeRequested, this, [this](const QString &id) {
        chooseWorkingDirectory(id);
    });
    connect(swimlane, &SwimlaneWidget::referenceDirectoryAddRequested, this, [this](const QString &id) {
        chooseReferenceDirectory(id);
    });
    connect(swimlane, &SwimlaneWidget::referenceDirectoryRemoveRequested, this, &MainWindow::removeReferenceDirectory);
    connect(swimlane, &SwimlaneWidget::branchViewRequested, this, [this](const QString &id) {
        showBranchViewer(id);
    });

    // Board hanya menampilkan satu project; sisanya menganggur di dalam stack
    ui->boardStack->addWidget(swimlane);

    // Daftarkan project ke sidebar: nama project (kiri) sejajar dengan tombol "+" (kanan)
    auto *item = new QListWidgetItem(ui->projectList);
    item->setData(Qt::UserRole, projectId);
    QWidget *rowWidget = createProjectRowWidget(projectId);
    // Hanya tingginya yang dipatok: lebar baris mengikuti lebar daftar, bukan panjang nama project
    item->setSizeHint(QSize(0, rowWidget->sizeHint().height()));
    ui->projectList->setItemWidget(item, rowWidget);

    m_swimlanes.insert(projectId, swimlane);
    refreshGitHead(projectId);
}

QWidget *MainWindow::createProjectRowWidget(const QString &projectId) {
    auto *row = new QWidget();
    row->setObjectName("projectRow");

    // Nama panjang dipotong "…" (nama lengkap di tooltip) supaya tombol di kanan tidak terdorong keluar
    auto *label = new ElidedLabel(projectId, row);
    label->setObjectName("projectRowLabel");

    auto *btnDelete = new QPushButton(row);
    btnDelete->setObjectName("btnProjectDelete");
    btnDelete->setIcon(Theme::icon(":/icons/trash.svg"));
    btnDelete->setIconSize(QSize(12, 12));
    btnDelete->setFixedSize(18, 18);
    btnDelete->setCursor(Qt::PointingHandCursor);
    btnDelete->setToolTip("Hapus project " + projectId);
    // Event filter dipakai untuk memutihkan icon selama hover (lihat eventFilter())
    btnDelete->installEventFilter(this);
    connect(btnDelete, &QPushButton::clicked, this, [this, projectId, btnDelete]() {
        showDeleteConfirmPopup(btnDelete, QString("Hapus project \"%1\"?").arg(projectId), [this, projectId]() {
            handleDeleteProjectRequested(projectId);
        });
    });

    auto *btnNewTask = new QPushButton(row);
    btnNewTask->setObjectName("btnProjectNewTask");
    btnNewTask->setIcon(Theme::icon(":/icons/plus.svg"));
    btnNewTask->setIconSize(QSize(12, 12));
    btnNewTask->setFixedSize(18, 18);
    btnNewTask->setCursor(Qt::PointingHandCursor);
    btnNewTask->setToolTip("New Task untuk " + projectId);
    btnNewTask->installEventFilter(this);
    connect(btnNewTask, &QPushButton::clicked, this, [this, projectId]() {
        handleNewTaskRequested(projectId);
    });

    // justify-between: label menempel kiri, tombol hapus + "+" menempel kanan
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(6, 3, 4, 3);
    rowLayout->setSpacing(2);
    rowLayout->addWidget(label, 1);
    rowLayout->addWidget(btnDelete);
    rowLayout->addWidget(btnNewTask);

    return row;
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event) {
    if (watched == ui->projectList->viewport() && event->type() == QEvent::Resize) {
        // Dijadwalkan sesudah QListView selesai menata ulang kotak item untuk lebar yang baru
        QTimer::singleShot(0, this, [this]() { fitProjectRowsToList(); });
    }

    if ((watched == m_sidebarRail || watched == ui->mainSplitter)
        && (event->type() == QEvent::Resize || event->type() == QEvent::Move)) {
        syncSidebarGeometry();
    }
    if (watched == m_btnShowSidebar && event->type() == QEvent::Enter) {
        showSidebarPeek();
    }

    auto *btn = qobject_cast<QPushButton*>(watched);
    if (btn) {
        const QString name = btn->objectName();
        const bool isRowButton = (name == "btnProjectDelete" || name == "btnProjectNewTask");

        if (isRowButton && event->type() == QEvent::Enter) {
            // Background hover-nya gelap, jadi icon versi putih yang dipakai
            btn->setIcon(Theme::icon(name == "btnProjectDelete" ? ":/icons/trash-white.svg"
                                                          : ":/icons/plus-white.svg"));
        } else if (isRowButton && event->type() == QEvent::Leave) {
            // Kembalikan ke warna sesuai status seleksi barisnya
            refreshProjectRowStyles();
        }
    }

    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::fitProjectRowsToList() {
    for (int row = 0; row < ui->projectList->count(); ++row) {
        QListWidgetItem *item = ui->projectList->item(row);
        QWidget *rowWidget = ui->projectList->itemWidget(item);
        const QRect rect = ui->projectList->visualItemRect(item);
        if (rowWidget && rect.isValid() && rowWidget->geometry() != rect) {
            rowWidget->setGeometry(rect);
        }
    }
}

void MainWindow::changeEvent(QEvent *event) {
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::ActivationChange && isActiveWindow() && !m_activeProjectId.isEmpty()) {
        refreshGitHead(m_activeProjectId);
    }
}

void MainWindow::showDeleteConfirmPopup(QWidget *anchor, const QString &question, std::function<void()> onConfirm) {
    // Qt::Popup: otomatis tertutup saat klik di luar area-nya, persis tooltip
    auto *popup = new QFrame(anchor, Qt::Popup);
    popup->setObjectName("deleteConfirmPopup");
    popup->setAttribute(Qt::WA_DeleteOnClose);

    auto *label = new QLabel(question, popup);
    label->setObjectName("deleteConfirmLabel");

    auto *btnCancel = new QPushButton("Batal", popup);
    btnCancel->setObjectName("btnDeleteConfirmCancel");
    btnCancel->setCursor(Qt::PointingHandCursor);

    auto *btnYes = new QPushButton("Ya", popup);
    btnYes->setObjectName("btnDeleteConfirmYes");
    btnYes->setCursor(Qt::PointingHandCursor);

    auto *actions = new QHBoxLayout();
    actions->setContentsMargins(0, 0, 0, 0);
    actions->setSpacing(6);
    actions->addStretch(1);
    actions->addWidget(btnCancel);
    actions->addWidget(btnYes);

    auto *popupLayout = new QVBoxLayout(popup);
    popupLayout->setContentsMargins(10, 8, 10, 8);
    popupLayout->setSpacing(8);
    popupLayout->addWidget(label);
    popupLayout->addLayout(actions);

    connect(btnCancel, &QPushButton::clicked, popup, &QWidget::close);
    connect(btnYes, &QPushButton::clicked, this, [this, popup, onConfirm]() {
        popup->close();
        // Dijadwalkan ke siklus event berikutnya: anchor yang dibuang (baris sidebar / kartu)
        // adalah induk popup ini, jadi menghapusnya selagi sinyal klik masih berjalan
        // berarti membongkar widget yang sedang mengirim sinyal tersebut.
        QTimer::singleShot(0, this, onConfirm);
    });

    popup->adjustSize();

    // Tepi kanan popup disejajarkan dengan tepi kanan tombol, menggantung di bawahnya
    const QPoint anchorBottomRight = anchor->mapToGlobal(QPoint(anchor->width(), anchor->height()));
    QPoint pos(anchorBottomRight.x() - popup->width(), anchorBottomRight.y() + 6);

    // Jaga popup tetap di dalam layar bila baris berada di pinggir/bawah
    if (QScreen *screen = QGuiApplication::screenAt(anchorBottomRight)) {
        const QRect available = screen->availableGeometry();
        pos.setX(qBound(available.left() + 4, pos.x(), available.right() - popup->width() - 4));
        if (pos.y() + popup->height() > available.bottom()) {
            pos.setY(anchor->mapToGlobal(QPoint(0, 0)).y() - popup->height() - 6);
        }
    }

    popup->move(pos);
    popup->show();
}

void MainWindow::refreshProjectRowStyles() {
    QListWidgetItem *current = ui->projectList->currentItem();

    for (int row = 0; row < ui->projectList->count(); ++row) {
        QListWidgetItem *item = ui->projectList->item(row);
        QWidget *rowWidget = ui->projectList->itemWidget(item);
        if (!rowWidget) {
            continue;
        }

        const bool selected = (item == current);
        if (auto *label = rowWidget->findChild<QLabel*>("projectRowLabel")) {
            label->setStyleSheet(selected ? "color: #ffffff;" : "");
        }
        if (auto *btn = rowWidget->findChild<QPushButton*>("btnProjectNewTask")) {
            // Tombol yang sedang di-hover tetap putih: background hover-nya gelap
            const bool white = selected || btn->underMouse();
            btn->setIcon(Theme::icon(white ? ":/icons/plus-white.svg" : ":/icons/plus.svg"));
        }
        if (auto *btn = rowWidget->findChild<QPushButton*>("btnProjectDelete")) {
            const bool white = selected || btn->underMouse();
            btn->setIcon(Theme::icon(white ? ":/icons/trash-white.svg" : ":/icons/trash.svg"));
        }
    }
}

int MainWindow::findProjectRow(const QString &projectId) const {
    for (int row = 0; row < ui->projectList->count(); ++row) {
        if (ui->projectList->item(row)->data(Qt::UserRole).toString() == projectId) {
            return row;
        }
    }
    return -1;
}

void MainWindow::setActiveProject(const QString &projectId) {
    auto *swimlane = m_swimlanes.value(projectId, nullptr);
    if (!swimlane) {
        m_activeProjectId.clear();
        ui->boardStack->setCurrentIndex(0);
        refreshProjectRowStyles();
        return;
    }

    m_activeProjectId = projectId;
    ui->boardStack->setCurrentWidget(swimlane);
    refreshGitHead(projectId);

    // Jaga sidebar tetap sinkron bila pemanggilan datang dari luar daftar
    const int row = findProjectRow(projectId);
    if (row >= 0 && ui->projectList->currentRow() != row) {
        QSignalBlocker blocker(ui->projectList);
        ui->projectList->setCurrentRow(row);
    }

    refreshProjectRowStyles();
}

void MainWindow::handleProjectSelected(QListWidgetItem *current, QListWidgetItem *previous) {
    Q_UNUSED(previous)

    if (!current) {
        m_activeProjectId.clear();
        ui->boardStack->setCurrentIndex(0);
        refreshProjectRowStyles();
        return;
    }

    setActiveProject(current->data(Qt::UserRole).toString());
}

void MainWindow::setupSidebarRail() {
    m_sidebarRail = new QWidget;
    m_sidebarRail->setObjectName("sidebarRail");
    m_sidebarRail->setMinimumWidth(m_sidebarMinWidth);
    m_sidebarRail->setMaximumWidth(m_sidebarMaxWidth);

    // Kembaran btnToggleSidebar: tetap terlihat selama sidebar disembunyikan, di ketinggian yang sama
    m_btnShowSidebar = new QPushButton(m_sidebarRail);
    m_btnShowSidebar->setObjectName("btnShowSidebar");
    m_btnShowSidebar->setIcon(Theme::icon(":/icons/sidebar.svg"));
    m_btnShowSidebar->setIconSize(ui->btnToggleSidebar->iconSize());
    m_btnShowSidebar->setFixedSize(ui->btnToggleSidebar->maximumSize());
    m_btnShowSidebar->setCursor(Qt::PointingHandCursor);
    m_btnShowSidebar->setToolTip("Pasang kembali daftar project");
    m_btnShowSidebar->hide();
    m_btnShowSidebar->installEventFilter(this);
    connect(m_btnShowSidebar, &QPushButton::clicked, this, &MainWindow::toggleSidebar);

    auto *railLayout = new QVBoxLayout(m_sidebarRail);
    railLayout->setContentsMargins(0, ui->sidebarLayout->contentsMargins().top(), 0, 0);
    railLayout->addWidget(m_btnShowSidebar, 0, Qt::AlignHCenter);
    railLayout->addStretch(1);

    // Rel menggantikan sidebarPanel di splitter; panelnya pindah ke atas contentWidget dan
    // mengikuti geometri rel lewat eventFilter(), jadi batas lebarnya kini milik rel
    ui->mainSplitter->replaceWidget(0, m_sidebarRail);
    ui->sidebarPanel->setParent(ui->contentWidget);
    ui->sidebarPanel->setMinimumWidth(0);
    ui->sidebarPanel->setMaximumWidth(QWIDGETSIZE_MAX);
    ui->sidebarPanel->show();
    m_sidebarRail->installEventFilter(this);
    ui->mainSplitter->installEventFilter(this);

    m_sidebarPeekTimer = new QTimer(this);
    m_sidebarPeekTimer->setInterval(kSidebarPeekPollMs);
    connect(m_sidebarPeekTimer, &QTimer::timeout, this, &MainWindow::hideSidebarPeekIfLeft);
}

int MainWindow::dockedSidebarWidth() const {
    return qBound(m_sidebarMinWidth, m_savedSidebarWidth, m_sidebarMaxWidth);
}

void MainWindow::syncSidebarGeometry() {
    if (!m_sidebarRail) return;

    QRect rect(m_sidebarRail->mapTo(ui->contentWidget, QPoint(0, 0)), m_sidebarRail->size());
    if (m_sidebarPeeking) {
        // Tepat di atas tepi kiri board: di kanan rel, melewati handle splitter
        rect.moveLeft(rect.right() + 1 + ui->mainSplitter->handleWidth());
        rect.setWidth(dockedSidebarWidth());
    } else if (m_sidebarHeldWidth > rect.width()) {
        rect.setWidth(m_sidebarHeldWidth);
    }
    ui->sidebarPanel->setGeometry(rect);
}

void MainWindow::toggleSidebar() {
    animateSidebar(!m_sidebarDocked);
}

void MainWindow::showSidebarPeek() {
    const bool animating = m_sidebarAnimation && m_sidebarAnimation->state() == QAbstractAnimation::Running;
    if (m_sidebarDocked || m_sidebarPeeking || animating) return;

    m_sidebarPeeking = true;
    m_sidebarPeekMisses = 0;
    // Selama tampil sementara, tombol di pojok sidebar memasangnya kembali (sama dengan tombol rel)
    ui->btnToggleSidebar->setToolTip(m_btnShowSidebar->toolTip());
    syncSidebarGeometry();
    ui->sidebarPanel->show();
    ui->sidebarPanel->raise();
    m_sidebarPeekTimer->start();
}

void MainWindow::hideSidebarPeekIfLeft() {
    if (!m_sidebarPeeking) {
        m_sidebarPeekTimer->stop();
        return;
    }

    // Rel + sidebar dihitung satu area, supaya celah di antara keduanya tidak menutup sidebar.
    // Popup konfirmasi hapus menempel di tombol baris project: selama popup itu terbuka, sidebar
    // (anchor-nya) ikut bertahan.
    const QRect area = ui->sidebarPanel->geometry().united(
        QRect(m_sidebarRail->mapTo(ui->contentWidget, QPoint(0, 0)), m_sidebarRail->size()));
    if (area.contains(ui->contentWidget->mapFromGlobal(QCursor::pos())) || QApplication::activePopupWidget()) {
        m_sidebarPeekMisses = 0;
        return;
    }
    // Baru ditutup setelah dua pemeriksaan berturut-turut di luar: kursor yang meleset sebentar
    // tidak langsung menghilangkan sidebar
    if (++m_sidebarPeekMisses < 2) return;

    m_sidebarPeekTimer->stop();
    m_sidebarPeeking = false;
    ui->sidebarPanel->hide();
}

void MainWindow::animateSidebar(bool opening) {
    // Interupsi animasi sebelumnya (mis. tombol diklik cepat berulang) agar
    // sidebar melanjutkan geseran dari posisi saat ini, bukan melompat.
    const bool interrupted = m_sidebarAnimation && m_sidebarAnimation->state() == QAbstractAnimation::Running;
    if (m_sidebarAnimation) {
        m_sidebarAnimation->stop();
    }

    QList<int> sizes = ui->mainSplitter->sizes();
    if (sizes.size() != ui->mainSplitter->count() || sizes.size() < 3) return;

    // sidebar + board berbagi lebar ini; console tetap sebesar sebelumnya
    const int sidebarBoardWidth = sizes.at(0) + sizes.at(1);
    const int consoleWidth = sizes.at(2);

    // Selalu mulai dari lebar rel saat ini agar animasi yang diinterupsi
    // melanjutkan geseran, bukan melompat balik
    const int startWidth = sizes.at(0);
    int endWidth;

    // Lebar digiring lewat maximumWidth rel; sidebarPanel mengikutinya (lihat eventFilter)
    m_sidebarRail->setMinimumWidth(kSidebarRailWidth);
    QSplitterHandle *handle = ui->mainSplitter->handle(1);

    if (opening) {
        endWidth = dockedSidebarWidth();

        // Sidebar yang sedang tampil sementara langsung jadi sidebar terpasang: lebarnya ditahan
        // selama rel melebar di bawahnya, jadi isinya tidak menciut dulu ke lebar rel
        m_sidebarHeldWidth = m_sidebarPeeking ? endWidth : 0;
        m_sidebarPeeking = false;
        m_sidebarPeekTimer->stop();

        m_btnShowSidebar->hide();
        ui->btnToggleSidebar->setToolTip("Sembunyikan daftar project");
        handle->setEnabled(true);
        handle->setCursor(Qt::SplitHCursor);
        syncSidebarGeometry();
        ui->sidebarPanel->show();
        ui->sidebarPanel->raise();
    } else {
        // Animasi buka yang dipotong belum sampai lebar penuh: lebar tersimpan yang lama tetap berlaku
        if (!interrupted) {
            m_savedSidebarWidth = startWidth;
        }
        endWidth = kSidebarRailWidth;
    }
    m_sidebarDocked = opening;

    auto *animation = new QVariantAnimation(this);
    animation->setStartValue(startWidth);
    animation->setEndValue(endWidth);
    animation->setDuration(220);
    animation->setEasingCurve(QEasingCurve::OutCubic);

    connect(animation, &QVariantAnimation::valueChanged, this,
            [this, sidebarBoardWidth, consoleWidth](const QVariant &value) {
        const int sidebarWidth = value.toInt();
        const int boardWidth = qMax(0, sidebarBoardWidth - sidebarWidth);
        m_sidebarRail->setMaximumWidth(sidebarWidth);
        ui->mainSplitter->setSizes({sidebarWidth, boardWidth, consoleWidth});
    });

    connect(animation, &QVariantAnimation::finished, this, [this, opening, handle]() {
        m_sidebarHeldWidth = 0;
        if (opening) {
            m_sidebarRail->setMinimumWidth(m_sidebarMinWidth);
            m_sidebarRail->setMaximumWidth(m_sidebarMaxWidth);
        } else {
            // Tinggal rel berisi tombol sidebar. Lebarnya tetap, jadi handle di kanannya tidak
            // lagi menawarkan geser.
            ui->sidebarPanel->hide();
            m_btnShowSidebar->show();
            m_sidebarRail->setFixedWidth(kSidebarRailWidth);
            handle->setEnabled(false);
            handle->unsetCursor();
        }
        syncSidebarGeometry();
    });

    m_sidebarAnimation = animation;
    animation->start(QAbstractAnimation::DeleteWhenStopped);
}

void MainWindow::handleCardMoved(const QString &projectId, KanbanCardWidget *card, const QString &targetStage, int targetIndex) {
    Q_UNUSED(projectId)
    Q_UNUSED(targetIndex)
    // Kolom sudah memindahkan widget-nya; TaskManager yang memutuskan boleh atau tidak.
    // Bila ditolak (mis. maju melewati gate yang belum disetujui), handleTaskMoveRejected()
    // mengembalikan kartu ke kolom asalnya; bila boleh, handleTaskMoved() mencatatnya di
    // kartu konsol task itu (jamnya sudah tercatat di sana)
    m_tasks.moveTask(card->id(), targetStage);
}

void MainWindow::handleNewTaskRequested(const QString &projectId) {
    NewTaskDialog dialog(projectId, m_catalog, this);
    dialog.loadBranches(m_fileManager->workingDirectory(projectId));
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    auto *swimlane = m_swimlanes.value(projectId, nullptr);
    if (!swimlane) return;

    TaskItem item = dialog.resultTask();
    if (!swimlane->column(item.stage)) {
        ui->consolePanel->appendLog(QString("[TASK CREATE WARN] %1: stage '%2' tidak dikenal").arg(projectId, item.stage));
        return;
    }
    item.attachments = saveAttachments(item, dialog.attachments());

    // Kartu dibuat dan task dipersistenkan oleh handleTaskAdded()
    m_tasks.addTask(item);

    logTask(item, QString("[TASK CREATED] %1 -> %2: '%3'").arg(projectId, item.stage, item.title));
    pullBaseLater(item);
}

void MainWindow::handleEditTaskRequested(const QString &projectId, const QString &taskId) {
    const std::optional<TaskItem> task = m_tasks.task(taskId);
    if (!task) return;

    NewTaskDialog dialog(*task, m_fileManager->attachmentDirectory(projectId, taskId), m_catalog, this);
    dialog.loadBranches(m_fileManager->workingDirectory(projectId));
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    // Kartu dan file sesi ikut diperbarui lewat sinyal taskChanged
    TaskItem item = dialog.resultTask();
    item.attachments = saveAttachments(item, dialog.attachments());
    if (!m_tasks.updateDetails(item)) {
        return;
    }

    logTask(item, QString("[TASK EDITED] %1 -> %2: '%3'").arg(projectId, item.stage, item.title));
    const std::optional<TaskItem> edited = m_tasks.task(taskId);
    if (edited && edited->branch.base != task->branch.base) {
        pullBaseLater(*edited);
    }
}

void MainWindow::pullBaseLater(const TaskItem &task) {
    const QString projectDir = m_fileManager->workingDirectory(task.projectId);
    if (task.branch.base.isEmpty() || !task.branch.isEmpty() || !TaskGit::looksLikeRepository(projectDir)) {
        return;
    }

    // Run task ini ditolak selama pull berjalan: run-nya sendiri pull ke ref yang sama
    m_gitBusy.insert(task.id);
    logTask(task, RunLogFormatter::gitLine(task, QString("pull %1 dari origin…").arg(task.branch.base)));
    auto *watcher = new QFutureWatcher<TaskGit::Result>(this);
    connect(watcher, &QFutureWatcher<TaskGit::Result>::finished, this, [this, watcher, task]() {
        watcher->deleteLater();
        m_gitBusy.remove(task.id);
        const TaskGit::Result result = watcher->result();
        logGitResult(task, result);
        if (!result.error.isEmpty()) {
            logTask(task, RunLogFormatter::gitLine(task, QStringLiteral("gagal: ") + result.error));
        }
        // Branch dasar yang sedang aktif di folder kerja ikut maju: commit di tombol header berubah
        refreshGitHead(task.projectId);
    });
    watcher->setFuture(QtConcurrent::run(&TaskGit::pull, projectDir, task.branch.base));
}

QStringList MainWindow::saveAttachments(const TaskItem &task, const QList<TaskAttachments::Draft> &drafts) {
    // task.attachments masih berisi lampiran sebelum form dibuka; yang tidak ada lagi di form dihapus
    QStringList errors;
    const QStringList saved = TaskAttachments::save(m_fileManager->attachmentDirectory(task.projectId, task.id),
                                                    drafts, task.attachments, &errors);
    for (const QString &error : std::as_const(errors)) {
        logTask(task, QString("[TASK WARN] %1/%2: lampiran %3").arg(task.projectId, task.title, error));
    }
    return saved;
}

void MainWindow::removeProjectFromUi(const QString &projectId) {
    auto *widget = m_swimlanes.value(projectId, nullptr);
    if (!widget) return;

    // Agent yang masih antre/berjalan untuk project ini ikut dihentikan
    m_swarm.cancelProject(projectId);
    if (BranchViewer *viewer = BranchViewer::find(projectId, this)) {
        viewer->close();
    }

    // Drawer yang menampilkan task project ini ditutup; data task dibuang dari memori
    // (untuk "Close", session.json di disk tetap ada)
    const std::optional<TaskItem> shown = m_tasks.task(m_drawer->taskId());
    if (shown && shown->projectId == projectId) {
        m_drawerAnimator->close();
    }
    m_tasks.removeProject(projectId);

    // Hapus baris sidebar lebih dulu: QListWidget otomatis memindahkan seleksi
    // ke baris tetangga, dan handleProjectSelected() yang menukar halaman board.
    const int row = findProjectRow(projectId);
    if (row >= 0) {
        delete ui->projectList->takeItem(row);
    }

    m_swimlanes.remove(projectId);
    ui->boardStack->removeWidget(widget);
    widget->deleteLater();

    // Pengaman bila seleksi tidak ikut berpindah (mis. yang ditutup bukan project aktif)
    if (m_activeProjectId == projectId) {
        if (ui->projectList->count() > 0) {
            ui->projectList->setCurrentRow(0);
        } else {
            m_activeProjectId.clear();
            ui->boardStack->setCurrentIndex(0);
        }
    }
}

void MainWindow::handleNewProjectRequested() {
    bool ok;
    QString projectName = QInputDialog::getText(this, "New Project",
                                                "Project Name / Target:",
                                                QLineEdit::Normal, "", &ok);
    if (ok && !projectName.trimmed().isEmpty()) {
        QString projectId = projectName.trimmed();

        if (m_swimlanes.contains(projectId)){
            ui->consolePanel->appendLog("[SYSTEM] Project '" + projectId + "' sudah ada.");
            setActiveProject(projectId);
            return;
        }

        addSwimlane(projectId);
        saveProject(projectId);

        // Project baru langsung ditampilkan di board
        setActiveProject(projectId);
        ui->consolePanel->appendLog("[SYSTEM] Project swimlane baru dibuat: " + projectId);
    }
}

void MainWindow::confirmRemoveProject(const QString &projectId) {
    if (!m_swimlanes.contains(projectId)) return;

    // Popup konfirmasi sidebar menempel di tombol hapus baris project, yang bisa sedang
    // tersembunyi (sidebar diciutkan); dari menu, dialog biasa yang tidak butuh anchor
    QMessageBox box(QMessageBox::Warning, "Remove Project",
                    QString("Hapus project \"%1\"?").arg(projectId), QMessageBox::NoButton, this);
    box.setInformativeText("Task, lampiran, dan worktree-nya dihapus permanen dari disk. "
                           "Folder kerja project tidak disentuh.");
    QPushButton *yes = box.addButton("Ya", QMessageBox::DestructiveRole);
    QPushButton *cancel = box.addButton("Batal", QMessageBox::RejectRole);
    // Hapus permanen: Enter/Esc jatuh ke "Batal"
    box.setDefaultButton(cancel);
    box.setEscapeButton(cancel);
    box.exec();

    if (box.clickedButton() == yes) {
        handleDeleteProjectRequested(projectId);
    }
}

void MainWindow::handleCloseProjectRequested(const QString &projectId) {
    if (!m_swimlanes.contains(projectId)) return;

    removeProjectFromUi(projectId);

    ui->consolePanel->appendLog(QString("[SYSTEM] Swimlane '%1' ditutup.").arg(projectId));
}

void MainWindow::handleDeleteProjectRequested(const QString &projectId) {
    if (!m_swimlanes.contains(projectId)) return;

    // Dibaca sebelum project dibuang: folder worktree task-nya ikut terhapus bersama folder project
    const QString projectDir = m_fileManager->workingDirectory(projectId);
    removeProjectFromUi(projectId);

    // Beda dengan "Close": data di disk ikut dibuang supaya project tidak
    // muncul lagi saat aplikasi dibuka berikutnya.
    QString error;
    const bool deleted = m_fileManager->deleteProject(projectId, &error);
    // Catatan worktree yang foldernya baru saja hilang dibersihkan dari repository
    if (TaskGit::looksLikeRepository(projectDir)) {
        QThreadPool::globalInstance()->start([projectDir]() { TaskGit::prune(projectDir); });
    }
    if (!deleted) {
        ui->consolePanel->appendLog(QString("[DELETE WARN] %1: %2").arg(projectId, error));
        return;
    }

    ui->consolePanel->appendLog(QString("[SYSTEM] Project '%1' dihapus permanen.").arg(projectId));
}

void MainWindow::handleDeleteTaskRequested(const QString &taskId) {
    if (!m_tasks.task(taskId)) return;

    // Run yang masih antre/berjalan dihentikan dulu; hasilnya (cancelled) tidak lagi berarti apa-apa
    m_swarm.cancel(taskId);

    // Kartu, file sesi, dan folder lampiran dibuang oleh handleTaskRemoved()
    m_tasks.removeTask(taskId);
}

void MainWindow::loadInitialMockData() {
    // Log awal konsol
    ui->consolePanel->appendLog("--- SYSTEM INITIALIZED ---");
    ui->consolePanel->appendLog("Belum ada project. Pilih File > New Project untuk memulai.");
    ui->consolePanel->appendLog("Ready for instructions.");
}

KanbanCardWidget *MainWindow::createCard(SwimlaneWidget *swimlane, const TaskItem &task) {
    auto *card = new KanbanCardWidget(swimlane);
    applyTaskToCard(card, task);

    const QString projectId = swimlane->projectId();
    connect(card, &KanbanCardWidget::runRequested, this, [this, projectId](const QString &taskId) {
        handleRunRequested(projectId, taskId);
    });
    connect(card, &KanbanCardWidget::cancelRequested, this, [this](const QString &taskId) {
        // Git masih menyiapkan run: agent-nya tidak dijalankan setelah git selesai
        if (m_gitBusy.contains(taskId)) {
            m_gitCancelled.insert(taskId);
            return;
        }
        m_swarm.cancel(taskId);
    });
    connect(card, &KanbanCardWidget::editRequested, this, [this, projectId](const QString &taskId) {
        handleEditTaskRequested(projectId, taskId);
    });
    connect(card, &KanbanCardWidget::deleteRequested, this, [this, card](const QString &taskId) {
        const std::optional<TaskItem> task = m_tasks.task(taskId);
        if (!task) return;
        QString question = QString("Hapus task \"%1\"?").arg(task->title);
        if (m_swarm.state(taskId) != RunState::Idle) {
            question += QStringLiteral("\nAgent-nya akan dihentikan.");
        }
        if (task->branch.hasWorktree()) {
            question += QString("\nWorktree-nya ikut dihapus; branch %1 tetap ada.").arg(task->branch.name);
        }
        showDeleteConfirmPopup(card, question, [this, taskId]() { handleDeleteTaskRequested(taskId); });
    });
    // Submenu "Pindah ke stage": aturannya sama dengan drag. handleTaskMoved() memindahkan
    // widget-nya; bila ditolak (gate), handleTaskMoveRejected() mencatat alasannya di konsol.
    card->setMoveTargets(m_catalog.keys());
    connect(card, &KanbanCardWidget::moveRequested, this, [this](const QString &taskId, const QString &stage) {
        m_tasks.moveTask(taskId, stage);
    });
    // 📋 (menunggu review) dan klik biasa pada kartu sama-sama membuka drawer hasil agent
    connect(card, &KanbanCardWidget::reviewRequested, this, &MainWindow::openDrawer);
    connect(card, &KanbanCardWidget::detailsRequested, this, &MainWindow::openDrawer);

    swimlane->addCardToStage(task.stage, card);
    return card;
}

void MainWindow::applyTaskToCard(KanbanCardWidget *card, const TaskItem &task) {
    card->setCardData(task.id, task.category, task.title, task.subtext, badgeText(task));
    card->setTaskState(task.state, failureDetail(task));
    card->setCompletedStage(task.completedStage());
    const int images = int(std::count_if(task.attachments.cbegin(), task.attachments.cend(), TaskAttachments::isImage));
    card->setAttachments(images, int(task.attachments.size()) - images, task.attachments);
}

KanbanCardWidget *MainWindow::cardFor(const TaskItem &task) const {
    auto *swimlane = m_swimlanes.value(task.projectId, nullptr);
    return swimlane ? swimlane->cardById(task.id) : nullptr;
}

void MainWindow::openDrawer(const QString &taskId) {
    const std::optional<TaskItem> task = m_tasks.task(taskId);
    if (!task) return;

    // Klik pada kartu yang belum pernah dijalankan tidak membuka apa-apa, kecuali di stage yang
    // menampilkan perubahan kode: perubahan kode sudah bisa dilihat sebelum agent-nya dijalankan
    const RunState runState = m_swarm.state(taskId);
    if (task->runs.isEmpty() && runState == RunState::Idle && !ResponseDrawer::showsCodeChanges(task->stage)) {
        return;
    }

    m_drawer->showTask(*task, runState, m_tasks.nextStage(task->stage), m_tasks.sendBackTargets(task->stage));
    const int boardWidth = m_boardSplitter->width();
    m_drawerAnimator->open(qBound(420, boardWidth * 45 / 100, 720));
}

void MainWindow::refreshDrawer(const TaskItem &task) {
    if (!m_drawerAnimator->isOpen() || m_drawer->taskId() != task.id) return;
    m_drawer->showTask(task, m_swarm.state(task.id), m_tasks.nextStage(task.stage),
                       m_tasks.sendBackTargets(task.stage));
}

void MainWindow::handleTaskAdded(const TaskItem &task) {
    auto *swimlane = m_swimlanes.value(task.projectId, nullptr);
    if (!swimlane) return;

    createCard(swimlane, task);
    if (!m_loading) {
        // Task baru ikut dipersistenkan; tanpa ini kartu hilang saat aplikasi ditutup
        saveProject(task.projectId);
    }
}

void MainWindow::handleTaskChanged(const TaskItem &task) {
    if (KanbanCardWidget *card = cardFor(task)) {
        if (card->taskState() != TaskState::AwaitingReview && task.state == TaskState::AwaitingReview) {
            logTask(task, RunLogFormatter::gateLine(
                task, QString("menunggu review di %1 — klik 📋 di kartu").arg(task.stage)));
        }
        applyTaskToCard(card, task);
    }
    ui->consolePanel->updateTask(task);
    refreshDrawer(task);
    saveProject(task.projectId);
}

void MainWindow::handleTaskMoved(const TaskItem &task, const QString &fromStage) {
    // Task selesai (approve QA, atau drag manual): worktree-nya tidak diperlukan lagi.
    // Branch dibiarkan tetap ada (lokal & remote) supaya bisa di-PR manual.
    if (task.stage == QLatin1String("DONE") && task.branch.hasWorktree()) {
        removeWorktreeLater(task);
    }

    auto *swimlane = m_swimlanes.value(task.projectId, nullptr);
    KanbanCardWidget *card = swimlane ? swimlane->cardById(task.id) : nullptr;
    if (!card) return;

    // Drag manual sudah memindahkan widget-nya; perpindahan lain (maju otomatis, keputusan review) belum
    if (swimlane->stageOf(card) != task.stage) {
        swimlane->addCardToStage(task.stage, card);
    }
    if (!m_userMoveInProgress) {
        logTask(task, QString("[TASK] %1/%2 %3 → %4").arg(task.projectId, task.title, fromStage, task.stage));
    }
}

void MainWindow::handleTaskMoveRejected(const QString &taskId, const QString &fromStage, const QString &reason) {
    const std::optional<TaskItem> task = m_tasks.task(taskId);
    if (!task) return;

    // Kolom tujuan sudah menerima widget saat drop; kembalikan ke kolom asal
    auto *swimlane = m_swimlanes.value(task->projectId, nullptr);
    if (KanbanCardWidget *card = swimlane ? swimlane->cardById(taskId) : nullptr) {
        if (swimlane->stageOf(card) != fromStage) {
            swimlane->addCardToStage(fromStage, card);
        }
    }
    logTask(*task, RunLogFormatter::gateLine(*task, QString("tidak bisa dipindah: %1").arg(reason)));
}

void MainWindow::handleTaskRemoved(const TaskItem &task) {
    if (m_drawer->taskId() == task.id) {
        m_drawerAnimator->close();
    }

    auto *swimlane = m_swimlanes.value(task.projectId, nullptr);
    if (KanbanCardWidget *card = swimlane ? swimlane->cardById(task.id) : nullptr) {
        if (KanbanColumnWidget *column = swimlane->column(swimlane->stageOf(card))) {
            column->removeCard(card);
        }
        card->hide();
        card->deleteLater();
    }
    saveProject(task.projectId);

    QString error;
    if (!m_fileManager->deleteAttachments(task.projectId, task.id, &error)) {
        logTask(task, QString("[DELETE WARN] %1/%2: %3").arg(task.projectId, task.title, error));
    }
    logTask(task, QString("[TASK DELETED] %1/%2 (%3)").arg(task.projectId, task.title, task.stage));
    ui->consolePanel->markTaskRemoved(task);
    removeWorktreeLater(task);
}

void MainWindow::handleApproveRequested(const QString &taskId, const QString &note) {
    const std::optional<TaskItem> before = m_tasks.task(taskId);
    if (!before) return;

    QString reason;
    m_userMoveInProgress = true;
    const bool approved = m_tasks.approve(taskId, note, &reason);
    m_userMoveInProgress = false;
    if (!approved) {
        logTask(*before, RunLogFormatter::gateLine(*before, QString("gagal disetujui: %1").arg(reason)));
        return;
    }
    const std::optional<TaskItem> after = m_tasks.task(taskId);
    logTask(*before, RunLogFormatter::gateLine(
        *before, QString("%1 disetujui → %2%3")
                     .arg(before->stage, after ? after->stage : QString(),
                          note.isEmpty() ? QString() : QStringLiteral(" (dengan catatan)"))));
}

void MainWindow::handleRevisionRequested(const QString &taskId, const QString &note) {
    const std::optional<TaskItem> task = m_tasks.task(taskId);
    if (!task) return;

    QString reason;
    if (!m_tasks.requestRevision(taskId, note, &reason)) {
        logTask(*task, RunLogFormatter::gateLine(*task, QString("revisi gagal: %1").arg(reason)));
        return;
    }
    logTask(*task, RunLogFormatter::gateLine(
        *task, QString("revisi diminta; %1 dijalankan lagi dengan catatan").arg(task->stage)));
    // Revisi adalah permintaan eksplisit pengguna, jadi langsung dijalankan
    handleRunRequested(task->projectId, taskId);
}

void MainWindow::handleSendBackRequested(const QString &taskId, const QString &stage, const QString &note) {
    const std::optional<TaskItem> task = m_tasks.task(taskId);
    if (!task) return;

    QString reason;
    m_userMoveInProgress = true;
    const bool sent = m_tasks.sendBack(taskId, stage, note, &reason);
    m_userMoveInProgress = false;
    logTask(*task, RunLogFormatter::gateLine(
        *task, sent ? QString("dikembalikan dari %1 ke %2").arg(task->stage, stage)
                    : QString("gagal dikembalikan: %1").arg(reason)));
}

void MainWindow::handleDiffRequested(const QString &taskId) {
    const std::optional<TaskItem> task = m_tasks.task(taskId);
    if (!task) return;

    const QString workingDirectory = taskDirectory(*task);
    if (workingDirectory.isEmpty() || !QDir(workingDirectory).exists()) {
        const QString why = task->branch.hasWorktree()
            ? QString("Worktree task ini sudah tidak ada (%1). Jalankan task lagi untuk memasangnya kembali.")
                  .arg(QDir::toNativeSeparators(task->branch.worktree))
            : QString("Folder kerja project %1 belum dipilih atau sudah tidak ada.").arg(task->projectId);
        m_drawer->showDiff(taskId, WorkspaceDiff::failure(why));
        return;
    }

    // git bisa lambat di repository besar, jadi dibaca di thread pool. Hasil permintaan lama
    // yang tersusul (mis. tombol muat ulang ditekan dua kali) dibuang.
    const int request = ++m_diffRequest;
    auto *watcher = new QFutureWatcher<WorkspaceDiff>(this);
    connect(watcher, &QFutureWatcher<WorkspaceDiff>::finished, this, [this, watcher, taskId, request]() {
        watcher->deleteLater();
        if (request == m_diffRequest) {
            m_drawer->showDiff(taskId, watcher->result());
        }
    });
    // Maintainability dan UML hanya tampil di stage peninjauan; stage lain cukup diff-nya
    const bool withMetrics = ResponseDrawer::showsCodeAnalysis(task->stage);
    // Task ber-branch dibanding titik cabangnya, jadi putaran yang sudah di-commit ikut terlihat
    const QString base = task->branch.hasWorktree() ? task->branch.baseCommit : QString();
    watcher->setFuture(QtConcurrent::run(&GitDiff::collect, workingDirectory, withMetrics, base));
}

void MainWindow::handleRunRequested(const QString &projectId, const QString &taskId) {
    // Data task (termasuk riwayat run untuk prompt serah-terima) diambil dari TaskManager
    const std::optional<TaskItem> found = m_tasks.task(taskId);
    if (!found) return;
    const TaskItem task = *found;

    const QString workingDirectory = ensureWorkingDirectory(projectId);
    if (workingDirectory.isEmpty()) {
        logTask(task, RunLogFormatter::rejectedLine(task, "folder kerja belum dipilih"));
        return;
    }
    if (m_gitBusy.contains(taskId)) {
        logTask(task, RunLogFormatter::rejectedLine(task, "git untuk task ini masih berjalan"));
        return;
    }

    // Git hanya disiapkan untuk run yang memang bisa jalan; sisanya ditolak coordinator seperti biasa.
    // Folder tanpa .git dicek tanpa proses, jadi run di folder biasa tetap langsung jalan.
    const StageProfile *profile = m_catalog.profile(task.stage);
    const bool runnable = profile && profile->agent() && m_swarm.state(taskId) == RunState::Idle;
    if (!runnable || (task.branch.isEmpty() && !TaskGit::looksLikeRepository(workingDirectory))) {
        startAgentRun(task, taskDirectory(task));
        return;
    }

    // Selalu pull dulu supaya agent bekerja di kode terbaru; branch/worktree dibuat atau dipasang
    // lagi, dan di QA kodenya di-commit + push. Fetch, checkout, dan push bisa lambat, jadi git
    // jalan di thread pool; selama itu kartu tampil antre.
    const bool branched = !task.branch.isEmpty() || TaskGit::startsBranch(task, m_catalog);
    const bool handoff = TaskGit::isHandoffStage(task.stage);
    const bool attached = task.branch.hasWorktree() && QFileInfo::exists(task.branch.worktree);
    m_gitBusy.insert(taskId);
    m_gitCancelled.remove(taskId);
    showRunState(task, RunState::Queued);
    QString step = QStringLiteral("pull dari origin sebelum run…");
    if (handoff) {
        step = QStringLiteral("pull, lalu serahkan kode ke QA: commit + push…");
    } else if (branched && !attached) {
        step = QStringLiteral("pull, lalu siapkan branch & worktree…");
    }
    logTask(task, RunLogFormatter::gitLine(task, step));

    const QString worktreePath = m_fileManager->worktreeDirectory(projectId, taskId);
    auto *watcher = new QFutureWatcher<TaskGit::Result>(this);
    connect(watcher, &QFutureWatcher<TaskGit::Result>::finished, this, [this, watcher, task]() {
        watcher->deleteLater();
        finishRunPreparation(task, watcher->result());
    });
    watcher->setFuture(QtConcurrent::run(&TaskGit::prepareRun, workingDirectory, worktreePath, task, branched, handoff));
}

void MainWindow::finishRunPreparation(const TaskItem &requested, const TaskGit::Result &result) {
    m_gitBusy.remove(requested.id);
    const bool cancelled = m_gitCancelled.remove(requested.id);

    const std::optional<TaskItem> current = m_tasks.task(requested.id);
    if (!current) {
        // Task (atau project-nya) dihapus selagi git berjalan: worktree yang barusan dibuat ikut
        // dibuang, dan kartu konsolnya tidak lagi tampil antre
        TaskItem removed = requested;
        removed.branch = result.branch;
        removeWorktreeLater(removed);
        ui->consolePanel->setRunState(requested, RunState::Idle);
        return;
    }
    logGitResult(*current, result);
    // Pull bisa memajukan branch yang aktif di folder kerja project
    refreshGitHead(current->projectId);
    if (result.branch != current->branch) {
        m_tasks.setBranch(requested.id, result.branch);
    }
    const TaskItem task = *m_tasks.task(requested.id);

    QString reason = result.error;
    if (reason.isEmpty() && cancelled) {
        reason = QStringLiteral("dibatalkan sebelum agent dijalankan");
    }
    if (!reason.isEmpty()) {
        logTask(task, RunLogFormatter::rejectedLine(task, reason));
    }
    if (!reason.isEmpty() || !startAgentRun(task, taskDirectory(task))) {
        showRunState(task, RunState::Idle);
    }
}

bool MainWindow::startAgentRun(const TaskItem &task, const QString &workingDirectory) {
    // Foto, isi dokumen lampiran, dan folder referensi dibaca sekarang supaya run memakai isi terbaru
    const TaskMaterials materials = TaskAttachments::materials(
        task, m_fileManager->attachmentDirectory(task.projectId, task.id),
        m_fileManager->referenceDirectories(task.projectId));
    for (const QString &warning : materials.warnings) {
        logTask(task, RunLogFormatter::warningLine(task, warning));
    }

    // Kartu baru berubah status lewat sinyal runQueued/runStarted dari coordinator
    QString reason;
    if (!m_swarm.run(task, workingDirectory, materials, &reason)) {
        logTask(task, RunLogFormatter::rejectedLine(task, reason));
        QString missing;
        if (!m_swarm.isRuntimeAvailable(&missing)) {
            showRuntimeNotice(m_swarm.diagnose(AgentResult::failure(QStringLiteral("failed_to_start"), missing)));
        }
        return false;
    }
    return true;
}

QString MainWindow::taskDirectory(const TaskItem &task) const {
    return task.branch.hasWorktree() ? task.branch.directory() : m_fileManager->workingDirectory(task.projectId);
}

void MainWindow::logGitResult(const TaskItem &task, const TaskGit::Result &result) {
    for (const QString &line : result.log) {
        logTask(task, RunLogFormatter::gitLine(task, line));
    }
    for (const QString &warning : result.warnings) {
        logTask(task, RunLogFormatter::gitLine(task, QStringLiteral("peringatan: ") + warning));
    }
}

void MainWindow::logTask(const TaskItem &task, const QString &line) {
    // Kartu konsol yang baru dibuat memakai data terbaru (pemanggil kadang memegang salinan lama,
    // mis. sebelum disetujui); task yang sudah dihapus memakai data terakhirnya
    const std::optional<TaskItem> current = m_tasks.task(task.id);
    ui->consolePanel->appendTaskLog(current ? *current : task, line);
}

void MainWindow::showRunState(const TaskItem &task, RunState state) {
    if (KanbanCardWidget *card = cardFor(task)) {
        card->setRunState(state);
    }
    const std::optional<TaskItem> current = m_tasks.task(task.id);
    ui->consolePanel->setRunState(current ? *current : task, state);
}

void MainWindow::removeWorktreeLater(const TaskItem &task) {
    if (!task.branch.hasWorktree()) return;

    const QString projectDir = m_fileManager->workingDirectory(task.projectId);
    auto *watcher = new QFutureWatcher<TaskGit::Result>(this);
    connect(watcher, &QFutureWatcher<TaskGit::Result>::finished, this, [this, watcher, task]() {
        watcher->deleteLater();
        const TaskGit::Result result = watcher->result();
        logGitResult(task, result);
        if (!result.error.isEmpty()) {
            logTask(task, RunLogFormatter::gitLine(task, QStringLiteral("gagal: ") + result.error));
            return;
        }
        // Task bisa saja sudah dihapus selagi ini berjalan (mis. dipanggil dari handleTaskRemoved);
        // setBranch cukup diam-diam gagal untuk task yang sudah tidak ada
        m_tasks.setBranch(task.id, result.branch);
    });
    watcher->setFuture(QtConcurrent::run(&TaskGit::removeWorktree, projectDir, task.branch));
}

QString MainWindow::ensureWorkingDirectory(const QString &projectId) {
    const QString saved = m_fileManager->workingDirectory(projectId);
    if (!saved.isEmpty() && QDir(saved).exists()) {
        return saved;
    }
    if (!saved.isEmpty()) {
        ui->consolePanel->appendLog(QString("[SYSTEM] Folder kerja %1 tidak ditemukan: %2").arg(projectId, saved));
    }
    return chooseWorkingDirectory(projectId);
}

QString MainWindow::chooseWorkingDirectory(const QString &projectId) {
    // `claude -p` melewati dialog workspace trust, jadi agent hanya boleh jalan
    // di folder yang dipilih pengguna sendiri di sini
    const QString current = m_fileManager->workingDirectory(projectId);
    const QString dir = QFileDialog::getExistingDirectory(
        this, QString("Folder kerja agent untuk %1").arg(projectId),
        current.isEmpty() ? QDir::homePath() : current);
    if (dir.isEmpty()) {
        return QString();
    }

    m_fileManager->setWorkingDirectory(projectId, dir);
    saveProject(projectId);
    if (auto *swimlane = m_swimlanes.value(projectId, nullptr)) {
        swimlane->setWorkingDirectory(dir);
    }
    refreshGitHead(projectId);
    if (BranchViewer *viewer = BranchViewer::find(projectId, this)) {
        viewer->setWorkingDirectory(dir);
    }
    ui->consolePanel->appendLog(QString("[SYSTEM] Folder kerja %1: %2").arg(projectId, dir));
    return dir;
}

void MainWindow::revealWorkingDirectory(const QString &projectId, FolderTarget target) {
    const QString dir = m_fileManager->workingDirectory(projectId);
    if (dir.isEmpty() || !QDir(dir).exists()) {
        ui->consolePanel->appendLog(QString("[SYSTEM] Folder kerja %1 belum dipilih atau sudah tidak ada").arg(projectId));
        return;
    }
    const bool terminal = target == FolderTarget::Terminal;
    if (!(terminal ? FolderLauncher::showInTerminal(dir) : FolderLauncher::showInExplorer(dir))) {
        ui->consolePanel->appendLog(QString("[SYSTEM] Gagal membuka folder kerja %1 di %2: %3")
                                        .arg(projectId, terminal ? QStringLiteral("terminal") : QStringLiteral("File Explorer"),
                                             QDir::toNativeSeparators(dir)));
    }
}

void MainWindow::showBranchViewer(const QString &projectId, const QString &branch, const QString &compareWith) {
    const QString dir = m_fileManager->workingDirectory(projectId);
    if (dir.isEmpty() || !QDir(dir).exists()) {
        ui->consolePanel->appendLog(QString("[SYSTEM] Folder kerja %1 belum dipilih atau sudah tidak ada").arg(projectId));
        return;
    }
    const bool opened = BranchViewer::find(projectId, this) != nullptr;
    BranchViewer *viewer = BranchViewer::showProject(projectId, dir, this, branch, compareWith);
    if (!opened) {
        // Jendela itu membaca HEAD folder kerja setiap dimuat ulang; tombol header ikut diperbarui
        connect(viewer, &BranchViewer::headRead, this, [this](const QString &id, const GitHead &head) {
            if (auto *swimlane = m_swimlanes.value(id, nullptr)) {
                swimlane->setGitHead(head);
            }
        });
    }
}

void MainWindow::refreshGitHead(const QString &projectId) {
    auto *swimlane = m_swimlanes.value(projectId, nullptr);
    if (!swimlane) return;

    // Folder biasa (tanpa .git di folder itu maupun induknya) tidak perlu menjalankan git
    const QString dir = m_fileManager->workingDirectory(projectId);
    if (dir.isEmpty() || !TaskGit::looksLikeRepository(dir)) {
        swimlane->setGitHead(GitHead());
        return;
    }
    auto *watcher = new QFutureWatcher<GitHead>(this);
    connect(watcher, &QFutureWatcher<GitHead>::finished, this, [this, watcher, projectId, dir]() {
        watcher->deleteLater();
        // Project bisa sudah ditutup, atau folder kerjanya sudah diganti selagi git berjalan
        auto *current = m_swimlanes.value(projectId, nullptr);
        if (current && m_fileManager->workingDirectory(projectId) == dir) {
            current->setGitHead(watcher->result());
        }
    });
    watcher->setFuture(QtConcurrent::run(&GitHistory::head, dir));
}

void MainWindow::chooseReferenceDirectory(const QString &projectId) {
    const QString working = m_fileManager->workingDirectory(projectId);
    const QStringList current = m_fileManager->referenceDirectories(projectId);
    const QString start = !current.isEmpty() ? current.last() : (!working.isEmpty() ? working : QDir::homePath());
    const QString dir = QFileDialog::getExistingDirectory(
        this, QString("Folder referensi untuk %1 (hanya dibaca agent)").arg(projectId), start);
    if (!dir.isEmpty()) {
        addReferenceDirectory(projectId, dir);
    }
}

bool MainWindow::addReferenceDirectory(const QString &projectId, const QString &dir) {
    const QString clean = QDir::cleanPath(dir);
    const QString shown = QDir::toNativeSeparators(clean);
    const QString working = m_fileManager->workingDirectory(projectId);
    QStringList dirs = m_fileManager->referenceDirectories(projectId);

    // Isi folder kerja sudah bisa dibaca (dan ditulis) agent; referensi hanya untuk folder di luarnya
    if (!working.isEmpty() && isSameOrInside(clean, working)) {
        ui->consolePanel->appendLog(QString("[SYSTEM] %1 tidak ditambahkan: sudah termasuk folder kerja %2")
                                        .arg(shown, projectId));
        return false;
    }
    for (const QString &existing : std::as_const(dirs)) {
        if (isSameOrInside(clean, existing)) {
            ui->consolePanel->appendLog(QString("[SYSTEM] %1 sudah termasuk folder referensi %2")
                                            .arg(shown, QDir::toNativeSeparators(existing)));
            return false;
        }
    }
    // Folder induk yang baru menggantikan subfoldernya yang sudah terdaftar
    dirs.removeIf([&clean](const QString &existing) { return isSameOrInside(existing, clean); });
    dirs.append(clean);

    m_fileManager->setReferenceDirectories(projectId, dirs);
    saveProject(projectId);
    if (auto *swimlane = m_swimlanes.value(projectId, nullptr)) {
        swimlane->setReferenceDirectories(dirs);
    }
    ui->consolePanel->appendLog(QString("[SYSTEM] Folder referensi %1 + %2").arg(projectId, shown));
    return true;
}

void MainWindow::removeReferenceDirectory(const QString &projectId, const QString &dir) {
    QStringList dirs = m_fileManager->referenceDirectories(projectId);
    if (!dirs.removeOne(dir)) {
        return;
    }
    m_fileManager->setReferenceDirectories(projectId, dirs);
    saveProject(projectId);
    if (auto *swimlane = m_swimlanes.value(projectId, nullptr)) {
        swimlane->setReferenceDirectories(dirs);
    }
    ui->consolePanel->appendLog(QString("[SYSTEM] Folder referensi %1 − %2").arg(projectId, QDir::toNativeSeparators(dir)));
}

void MainWindow::handleRunQueued(const TaskItem &task) {
    showRunState(task, RunState::Queued);
    logTask(task, RunLogFormatter::queuedLine(task));
}

void MainWindow::handleRunStarted(const TaskItem &task, const AgentLaunch &launch) {
    showRunState(task, RunState::Running);
    // Satu notifikasi: baris [RUN] jadi ringkasan kartu konsol, isi prompt-nya ikut di log
    logTask(task, RunLogFormatter::startLines(task, launch).join(QLatin1Char('\n')));

    // Drawer yang sedang menampilkan task ini beralih ke output Live
    if (m_drawerAnimator->isOpen() && m_drawer->taskId() == task.id) {
        if (const std::optional<TaskItem> current = m_tasks.task(task.id)) {
            m_drawer->showTask(*current, RunState::Running, m_tasks.nextStage(current->stage),
                               m_tasks.sendBackTargets(current->stage));
        }
        m_drawer->startLive();
    }
}

void MainWindow::handleRunEvent(const TaskItem &task, const AgentEvent &event) {
    const QString workingDirectory = taskDirectory(task);
    const QString line = RunLogFormatter::eventLine(task, event, workingDirectory);
    if (!line.isEmpty()) {
        ui->consolePanel->appendTaskOutput(task, line);
    }
    if (m_drawer->taskId() == task.id) {
        m_drawer->appendLive(event, workingDirectory);
    }
}

void MainWindow::handleRunFinished(const TaskItem &task, const AgentResult &result) {
    // Status task (maju / review / gagal) sudah diputuskan TaskManager::recordRun lewat
    // sambungan di main.cpp; di sini tinggal status run kartu dan notifikasi konsol
    showRunState(task, RunState::Idle);
    logTask(task, RunLogFormatter::finishLine(task, result));
    // Gagal karena Claude Code-nya sendiri (hilang, belum login): beri tahu cara memperbaikinya
    showRuntimeNotice(m_swarm.diagnose(result));
}

void MainWindow::showFontPicker() {
    auto *dialog = new FontPickerDialog(AppFonts::registerBundled(), AppFonts::saved(), this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &FontPickerDialog::fontChosen, this, [](const QString &family) {
        AppFonts::save(family);
        Theme::setUiFontFamily(family);
        Theme::apply(*qApp);
    });
    dialog->open();
}

void MainWindow::showIntegrations() {
    // Dialog yang baru ditutup masih ada sampai deleteLater-nya jalan: yang dihitung hanya yang tampil
    if (m_integrations && m_integrations->isVisible()) {
        m_integrations->raise();
        m_integrations->activateWindow();
        return;
    }
    auto *dialog = new IntegrationsDialog(AgentAccess::load(), m_swarm.createAccountLogin(nullptr), this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &IntegrationsDialog::accessSaved, this, [this](AgentAccess::Method method) {
        ui->consolePanel->appendLog(QStringLiteral("[SYSTEM] Akses agent: %1").arg(
            method == AgentAccess::Method::ApiKey ? QStringLiteral("API key Anthropic")
                                                  : QStringLiteral("login Claude Code")));
    });
    m_integrations = dialog;
    dialog->open();
}

void MainWindow::checkRuntime() {
    auto *watcher = new QFutureWatcher<RuntimeCheck>(this);
    connect(watcher, &QFutureWatcher<RuntimeCheck>::finished, this, [this, watcher]() {
        watcher->deleteLater();
        showRuntimeNotice(watcher->result());
    });
    SwarmCoordinator *swarm = &m_swarm;
    watcher->setFuture(QtConcurrent::run([swarm]() { return swarm->checkRuntime(); }));
}

void MainWindow::showRuntimeNotice(const RuntimeCheck &check) {
    if (check.status == RuntimeCheck::Status::Ok || m_runtimeNotice) {
        return;
    }
    auto *dialog = new RuntimeNoticeDialog(check, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    if (check.status == RuntimeCheck::Status::BadApiKey) {
        // Key diperbaiki di File > Integrations, bukan lewat halaman panduan
        connect(dialog, &QDialog::accepted, this, &MainWindow::showIntegrations);
    }
    m_runtimeNotice = dialog;
    // open(): modal ke jendela ini tanpa menahan event loop pemanggilnya
    dialog->open();
}

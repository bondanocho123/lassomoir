#include "MainWindow.h"
#include "ui_MainWindow.h"
#include "SwimlaneWidget.h"
#include "KanbanColumnWidget.h"
#include "KanbanCardWidget.h"
#include "TaskItem.h"
#include "ConsolePanelWidget.h"
#include "FileManager.h"
#include "NewTaskDialog.h"
#include "ResponseDrawer.h"
#include "RunLogFormatter.h"
#include "SplitterPaneAnimator.h"
#include "StageCatalog.h"
#include "SwarmCoordinator.h"
#include "TaskManager.h"

#include <QFileDialog>
#include <QInputDialog>
#include <QDateTime>
#include <QAbstractAnimation>
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

namespace {

QString badgeText(const TaskItem &task) {
    return QString("✓ %1").arg(task.approvedGates());
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
    connect(m_drawer, &ResponseDrawer::approveRequested, this, &MainWindow::handleApproveRequested);
    connect(m_drawer, &ResponseDrawer::revisionRequested, this, &MainWindow::handleRevisionRequested);
    connect(m_drawer, &ResponseDrawer::sendBackRequested, this, &MainWindow::handleSendBackRequested);

    // Simpan lebar minimum asli sidebar (dari .ui) sebelum animasi bisa mengubahnya
    m_sidebarMinWidth = ui->sidebarPanel->minimumWidth();
    m_sidebarMaxWidth = ui->sidebarPanel->maximumWidth();

    // Wordmark aplikasi di header: teks bergaya Latin/serif menggantikan logo gambar
    ui->labelAppName->setText("L'Assommoir");
    QFont brandFont("Garamond");
    brandFont.setStyleHint(QFont::Serif);
    brandFont.setItalic(true);
    brandFont.setPointSize(19);
    brandFont.setWeight(QFont::DemiBold);
    brandFont.setLetterSpacing(QFont::AbsoluteSpacing, 0.5);
    ui->labelAppName->setFont(brandFont);
    ui->labelAppName->setStyleSheet("color: #a9743f;");
    ui->labelAppName->setToolTip("L'Assommoir");
    ui->rootVerticalLayout->setStretch(1,1);
    QTimer::singleShot(0, this, [this]() {
        // sidebar : board : console, boleh disesuaikan
        ui->mainSplitter->setSizes({180, 820, 350});
    });

    // Sambungkan input teks dari panel konsol kanan
    connect(ui->consolePanel, &ConsolePanelWidget::commandSubmitted,
            this, &MainWindow::handleCommandSubmitted);

    // Jarak antar baris project di sidebar (spacing murni, bukan margin item,
    // supaya kotak hover/selected tetap pas dengan tinggi widget-nya)
    ui->projectList->setSpacing(4);

    // Pemilihan project di sidebar menentukan apa yang tampil di board
    connect(ui->projectList, &QListWidget::currentItemChanged,
            this, &MainWindow::handleProjectSelected);

    // Tombol hamburger untuk menciutkan / melebarkan sidebar
    connect(ui->btnToggleSidebar, &QPushButton::clicked,
            this, &MainWindow::toggleSidebar);

    // Tombol global New Project di top bar
    connect(ui->btnGlobalNewProject, &QPushButton::clicked, this, [this]() {
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
    });

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

    // Tangkap interaksi dari swimlane
    connect(swimlane, &SwimlaneWidget::cardMoved, this, &MainWindow::handleCardMoved);
    connect(swimlane, &SwimlaneWidget::closeProjectRequested, this, &MainWindow::handleCloseProjectRequested);
    connect(swimlane, &SwimlaneWidget::workingDirectoryChangeRequested, this, [this](const QString &id) {
        chooseWorkingDirectory(id);
    });

    // Board hanya menampilkan satu project; sisanya menganggur di dalam stack
    ui->boardStack->addWidget(swimlane);

    // Daftarkan project ke sidebar: nama project (kiri) sejajar dengan tombol "+" (kanan)
    auto *item = new QListWidgetItem(ui->projectList);
    item->setData(Qt::UserRole, projectId);
    QWidget *rowWidget = createProjectRowWidget(projectId);
    item->setSizeHint(rowWidget->sizeHint());
    ui->projectList->setItemWidget(item, rowWidget);

    m_swimlanes.insert(projectId, swimlane);
}

QWidget *MainWindow::createProjectRowWidget(const QString &projectId) {
    auto *row = new QWidget();
    row->setObjectName("projectRow");

    auto *label = new QLabel(projectId, row);
    label->setObjectName("projectRowLabel");

    auto *btnDelete = new QPushButton(row);
    btnDelete->setObjectName("btnProjectDelete");
    btnDelete->setIcon(QIcon(":/icons/trash.svg"));
    btnDelete->setIconSize(QSize(12, 12));
    btnDelete->setFixedSize(18, 18);
    btnDelete->setCursor(Qt::PointingHandCursor);
    btnDelete->setToolTip("Hapus project " + projectId);
    // Event filter dipakai untuk memutihkan icon selama hover (lihat eventFilter())
    btnDelete->installEventFilter(this);
    connect(btnDelete, &QPushButton::clicked, this, [this, projectId, btnDelete]() {
        showDeleteConfirmPopup(projectId, btnDelete);
    });

    auto *btnNewTask = new QPushButton(row);
    btnNewTask->setObjectName("btnProjectNewTask");
    btnNewTask->setIcon(QIcon(":/icons/plus.svg"));
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
    rowLayout->addWidget(label);
    rowLayout->addStretch(1);
    rowLayout->addWidget(btnDelete);
    rowLayout->addWidget(btnNewTask);

    return row;
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event) {
    auto *btn = qobject_cast<QPushButton*>(watched);
    if (btn) {
        const QString name = btn->objectName();
        const bool isRowButton = (name == "btnProjectDelete" || name == "btnProjectNewTask");

        if (isRowButton && event->type() == QEvent::Enter) {
            // Background hover-nya gelap, jadi icon versi putih yang dipakai
            btn->setIcon(QIcon(name == "btnProjectDelete" ? ":/icons/trash-white.svg"
                                                          : ":/icons/plus-white.svg"));
        } else if (isRowButton && event->type() == QEvent::Leave) {
            // Kembalikan ke warna sesuai status seleksi barisnya
            refreshProjectRowStyles();
        }
    }

    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::showDeleteConfirmPopup(const QString &projectId, QWidget *anchor) {
    // Qt::Popup: otomatis tertutup saat klik di luar area-nya, persis tooltip
    auto *popup = new QFrame(anchor, Qt::Popup);
    popup->setObjectName("deleteConfirmPopup");
    popup->setAttribute(Qt::WA_DeleteOnClose);

    auto *question = new QLabel(QString("Hapus project \"%1\"?").arg(projectId), popup);
    question->setObjectName("deleteConfirmLabel");

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
    popupLayout->addWidget(question);
    popupLayout->addLayout(actions);

    connect(btnCancel, &QPushButton::clicked, popup, &QWidget::close);
    connect(btnYes, &QPushButton::clicked, this, [this, popup, projectId]() {
        popup->close();
        // Dijadwalkan ke siklus event berikutnya: baris sidebar yang dibuang adalah
        // induk popup ini, jadi menghapusnya selagi sinyal klik masih berjalan
        // berarti membongkar widget yang sedang mengirim sinyal tersebut.
        QTimer::singleShot(0, this, [this, projectId]() {
            handleDeleteProjectRequested(projectId);
        });
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
            btn->setIcon(QIcon(white ? ":/icons/plus-white.svg" : ":/icons/plus.svg"));
        }
        if (auto *btn = rowWidget->findChild<QPushButton*>("btnProjectDelete")) {
            const bool white = selected || btn->underMouse();
            btn->setIcon(QIcon(white ? ":/icons/trash-white.svg" : ":/icons/trash.svg"));
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

void MainWindow::toggleSidebar() {
    animateSidebar(!ui->sidebarPanel->isVisible());
}

void MainWindow::animateSidebar(bool opening) {
    // Interupsi animasi sebelumnya (mis. tombol diklik cepat berulang) agar
    // sidebar melanjutkan geseran dari posisi saat ini, bukan melompat.
    if (m_sidebarAnimation) {
        m_sidebarAnimation->stop();
    }

    QList<int> sizes = ui->mainSplitter->sizes();
    if (sizes.size() != ui->mainSplitter->count() || sizes.size() < 3) return;

    // sidebar + board berbagi lebar ini; console tetap sebesar sebelumnya
    const int sidebarBoardWidth = sizes.at(0) + sizes.at(1);
    const int consoleWidth = sizes.at(2);

    // Selalu mulai dari lebar sidebar saat ini agar animasi yang diinterupsi
    // melanjutkan geseran, bukan melompat balik ke 0
    const int startWidth = sizes.at(0);
    int endWidth;

    // Lebar digiring lewat maximumWidth, bukan cuma minimumWidth: QSplitter
    // tetap menahan panel di minimumSizeHint layout-nya (~118px) sehingga sidebar
    // mentok di situ lalu melompat ke 0 saat disembunyikan (terlihat seperti resize 2x)
    ui->sidebarPanel->setMinimumWidth(0);

    if (opening) {
        endWidth = qBound(m_sidebarMinWidth, m_savedSidebarWidth, m_sidebarMaxWidth);

        // Kunci ke lebar awal sebelum panel di-show, kalau tidak splitter sempat
        // memberinya lebar penuh dulu lalu animasi membukanya lagi
        ui->sidebarPanel->setMaximumWidth(startWidth);
        ui->sidebarPanel->setVisible(true);
        ui->mainSplitter->setSizes({startWidth, qMax(0, sidebarBoardWidth - startWidth), consoleWidth});
    } else {
        m_savedSidebarWidth = startWidth;
        endWidth = 0;
    }

    auto *animation = new QVariantAnimation(this);
    animation->setStartValue(startWidth);
    animation->setEndValue(endWidth);
    animation->setDuration(220);
    animation->setEasingCurve(QEasingCurve::OutCubic);

    connect(animation, &QVariantAnimation::valueChanged, this,
            [this, sidebarBoardWidth, consoleWidth](const QVariant &value) {
        const int sidebarWidth = value.toInt();
        const int boardWidth = qMax(0, sidebarBoardWidth - sidebarWidth);
        ui->sidebarPanel->setMaximumWidth(sidebarWidth);
        ui->mainSplitter->setSizes({sidebarWidth, boardWidth, consoleWidth});
    });

    connect(animation, &QVariantAnimation::finished, this, [this, opening]() {
        // Sembunyikan dulu baru kembalikan batasan lebar; urutan sebaliknya
        // membuat splitter melebarkan sidebar sesaat sebelum panel hilang
        if (!opening) {
            ui->sidebarPanel->setVisible(false);
        }
        ui->sidebarPanel->setMinimumWidth(m_sidebarMinWidth);
        ui->sidebarPanel->setMaximumWidth(m_sidebarMaxWidth);
    });

    m_sidebarAnimation = animation;
    animation->start(QAbstractAnimation::DeleteWhenStopped);
}

void MainWindow::handleCardMoved(const QString &projectId, KanbanCardWidget *card, const QString &targetStage, int targetIndex) {
    // Kolom sudah memindahkan widget-nya; TaskManager yang memutuskan boleh atau tidak.
    // Bila ditolak (mis. maju melewati gate yang belum disetujui), handleTaskMoveRejected()
    // mengembalikan kartu ke kolom asalnya.
    m_userMoveInProgress = true;
    const bool moved = m_tasks.moveTask(card->id(), targetStage);
    m_userMoveInProgress = false;
    if (!moved) {
        return;
    }

    QString timeStr = QDateTime::currentDateTime().toString("hh:mm:ss");
    QString logEntry = QString("[%1] [%2] '%3' moved to %4 (index %5)")
                           .arg(timeStr, projectId, card->title(), targetStage)
                           .arg(targetIndex);
    ui->consolePanel->appendLog(logEntry);
}

void MainWindow::handleNewTaskRequested(const QString &projectId) {
    NewTaskDialog dialog(projectId, m_catalog.keys(), this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    auto *swimlane = m_swimlanes.value(projectId, nullptr);
    if (!swimlane) return;

    const TaskItem item = dialog.resultTask();
    if (!swimlane->column(item.stage)) {
        ui->consolePanel->appendLog(QString("[TASK CREATE WARN] %1: stage '%2' tidak dikenal").arg(projectId, item.stage));
        return;
    }

    // Kartu dibuat dan task dipersistenkan oleh handleTaskAdded()
    m_tasks.addTask(item);

    ui->consolePanel->appendLog(QString("[TASK CREATED] %1 -> %2: '%3'").arg(projectId, item.stage, item.title));
}

void MainWindow::handleEditTaskRequested(const QString &projectId, const QString &taskId) {
    const std::optional<TaskItem> task = m_tasks.task(taskId);
    if (!task) return;

    NewTaskDialog dialog(*task, m_catalog.keys(), this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    // Kartu dan file sesi ikut diperbarui lewat sinyal taskChanged
    const TaskItem item = dialog.resultTask();
    if (!m_tasks.updateDetails(taskId, item.title, item.category, item.subtext)) {
        return;
    }

    ui->consolePanel->appendLog(QString("[TASK EDITED] %1 -> %2: '%3'").arg(projectId, item.stage, item.title));
}

void MainWindow::removeProjectFromUi(const QString &projectId) {
    auto *widget = m_swimlanes.value(projectId, nullptr);
    if (!widget) return;

    // Agent yang masih antre/berjalan untuk project ini ikut dihentikan
    m_swarm.cancelProject(projectId);

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

void MainWindow::handleCloseProjectRequested(const QString &projectId) {
    if (!m_swimlanes.contains(projectId)) return;

    removeProjectFromUi(projectId);

    ui->consolePanel->appendLog(QString("[SYSTEM] Swimlane '%1' ditutup.").arg(projectId));
}

void MainWindow::handleDeleteProjectRequested(const QString &projectId) {
    if (!m_swimlanes.contains(projectId)) return;

    removeProjectFromUi(projectId);

    // Beda dengan "Close": data di disk ikut dibuang supaya project tidak
    // muncul lagi saat aplikasi dibuka berikutnya.
    QString error;
    if (!m_fileManager->deleteProject(projectId, &error)) {
        ui->consolePanel->appendLog(QString("[DELETE WARN] %1: %2").arg(projectId, error));
        return;
    }

    ui->consolePanel->appendLog(QString("[SYSTEM] Project '%1' dihapus permanen.").arg(projectId));
}

void MainWindow::handleCommandSubmitted(const QString &command) {
    if (command.toLower() == "clear") {
        // Handle clear jika diinginkan
    } else {
        ui->consolePanel->appendLog("[AGENT ECHO] Command diterima: " + command);
    }
}

void MainWindow::loadInitialMockData() {
    // Log awal konsol
    ui->consolePanel->appendLog("--- SYSTEM INITIALIZED ---");
    ui->consolePanel->appendLog("Belum ada project. Klik \"New Project\" untuk memulai.");
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
        m_swarm.cancel(taskId);
    });
    connect(card, &KanbanCardWidget::editRequested, this, [this, projectId](const QString &taskId) {
        handleEditTaskRequested(projectId, taskId);
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
}

KanbanCardWidget *MainWindow::cardFor(const TaskItem &task) const {
    auto *swimlane = m_swimlanes.value(task.projectId, nullptr);
    return swimlane ? swimlane->cardById(task.id) : nullptr;
}

void MainWindow::openDrawer(const QString &taskId) {
    const std::optional<TaskItem> task = m_tasks.task(taskId);
    if (!task) return;

    // Klik pada kartu yang belum pernah dijalankan tidak membuka apa-apa
    const RunState runState = m_swarm.state(taskId);
    if (task->runs.isEmpty() && runState == RunState::Idle) return;

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
            ui->consolePanel->appendLog(RunLogFormatter::gateLine(
                task, QString("menunggu review di %1 — klik 📋 di kartu").arg(task.stage)));
        }
        applyTaskToCard(card, task);
    }
    refreshDrawer(task);
    saveProject(task.projectId);
}

void MainWindow::handleTaskMoved(const TaskItem &task, const QString &fromStage) {
    auto *swimlane = m_swimlanes.value(task.projectId, nullptr);
    KanbanCardWidget *card = swimlane ? swimlane->cardById(task.id) : nullptr;
    if (!card) return;

    // Drag manual sudah memindahkan widget-nya; perpindahan lain (maju otomatis, keputusan review) belum
    if (swimlane->stageOf(card) != task.stage) {
        swimlane->addCardToStage(task.stage, card);
    }
    if (!m_userMoveInProgress) {
        ui->consolePanel->appendLog(QString("[TASK] %1/%2 %3 → %4")
                                        .arg(task.projectId, task.title, fromStage, task.stage));
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
    ui->consolePanel->appendLog(RunLogFormatter::gateLine(*task, QString("tidak bisa dipindah: %1").arg(reason)));
}

void MainWindow::handleApproveRequested(const QString &taskId, const QString &note) {
    const std::optional<TaskItem> before = m_tasks.task(taskId);
    if (!before) return;

    QString reason;
    m_userMoveInProgress = true;
    const bool approved = m_tasks.approve(taskId, note, &reason);
    m_userMoveInProgress = false;
    if (!approved) {
        ui->consolePanel->appendLog(RunLogFormatter::gateLine(*before, QString("gagal disetujui: %1").arg(reason)));
        return;
    }
    const std::optional<TaskItem> after = m_tasks.task(taskId);
    ui->consolePanel->appendLog(RunLogFormatter::gateLine(
        *before, QString("%1 disetujui → %2%3")
                     .arg(before->stage, after ? after->stage : QString(),
                          note.isEmpty() ? QString() : QStringLiteral(" (dengan catatan)"))));
}

void MainWindow::handleRevisionRequested(const QString &taskId, const QString &note) {
    const std::optional<TaskItem> task = m_tasks.task(taskId);
    if (!task) return;

    QString reason;
    if (!m_tasks.requestRevision(taskId, note, &reason)) {
        ui->consolePanel->appendLog(RunLogFormatter::gateLine(*task, QString("revisi gagal: %1").arg(reason)));
        return;
    }
    ui->consolePanel->appendLog(RunLogFormatter::gateLine(
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
    ui->consolePanel->appendLog(RunLogFormatter::gateLine(
        *task, sent ? QString("dikembalikan dari %1 ke %2").arg(task->stage, stage)
                    : QString("gagal dikembalikan: %1").arg(reason)));
}

void MainWindow::handleRunRequested(const QString &projectId, const QString &taskId) {
    // Data task (termasuk riwayat run untuk prompt serah-terima) diambil dari TaskManager
    const std::optional<TaskItem> found = m_tasks.task(taskId);
    if (!found) return;
    const TaskItem task = *found;

    const QString workingDirectory = ensureWorkingDirectory(projectId);
    if (workingDirectory.isEmpty()) {
        ui->consolePanel->appendLog(RunLogFormatter::rejectedLine(task, "folder kerja belum dipilih"));
        return;
    }

    // Kartu baru berubah status lewat sinyal runQueued/runStarted dari coordinator
    QString reason;
    if (!m_swarm.run(task, workingDirectory, &reason)) {
        ui->consolePanel->appendLog(RunLogFormatter::rejectedLine(task, reason));
    }
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
    ui->consolePanel->appendLog(QString("[SYSTEM] Folder kerja %1: %2").arg(projectId, dir));
    return dir;
}

void MainWindow::handleRunQueued(const TaskItem &task) {
    if (KanbanCardWidget *card = cardFor(task)) {
        card->setRunState(RunState::Queued);
    }
    ui->consolePanel->appendLog(RunLogFormatter::queuedLine(task));
}

void MainWindow::handleRunStarted(const TaskItem &task, const AgentLaunch &launch) {
    if (KanbanCardWidget *card = cardFor(task)) {
        card->setRunState(RunState::Running);
    }
    for (const QString &line : RunLogFormatter::startLines(task, launch)) {
        ui->consolePanel->appendLog(line);
    }

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
    const QString workingDirectory = m_fileManager->workingDirectory(task.projectId);
    const QString line = RunLogFormatter::eventLine(task, event, workingDirectory);
    if (!line.isEmpty()) {
        ui->consolePanel->appendLog(line);
    }
    if (m_drawer->taskId() == task.id) {
        m_drawer->appendLive(event, workingDirectory);
    }
}

void MainWindow::handleRunFinished(const TaskItem &task, const AgentResult &result) {
    // Status task (maju / review / gagal) sudah diputuskan TaskManager::recordRun lewat
    // sambungan di main.cpp; di sini tinggal status run kartu dan baris konsol
    if (KanbanCardWidget *card = cardFor(task)) {
        card->setRunState(RunState::Idle);
    }
    ui->consolePanel->appendLog(RunLogFormatter::finishLine(task, result));
}

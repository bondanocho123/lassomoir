#include "MainWindow.h"
#include "ui_MainWindow.h"
#include "SwimlaneWidget.h"
#include "KanbanColumnWidget.h"
#include "KanbanCardWidget.h"
#include "TaskItem.h"
#include "ConsolePanelWidget.h"
#include "FileManager.h"

#include <QInputDialog>
#include <QDateTime>
#include <QAbstractAnimation>
#include <QEasingCurve>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSize>
#include <QSplitter>
#include <QStackedWidget>
#include <QTimer>
#include <QVariantAnimation>
#include <QDir>
#include <QPixmap>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent),
    ui(new Ui::MainWindow) {
    m_fileManager = new FileManager(this);
    ui->setupUi(this);

    // Simpan lebar minimum asli sidebar (dari .ui) sebelum animasi bisa mengubahnya
    m_sidebarMinWidth = ui->sidebarPanel->minimumWidth();
    m_sidebarMaxWidth = ui->sidebarPanel->maximumWidth();

    // Logo menggantikan teks aplikasi di header
    QPixmap logo(":/logo.png");
    if (!logo.isNull()) {
        ui->labelAppName->setPixmap(
            logo.scaledToHeight(32, Qt::SmoothTransformation));
        ui->labelAppName->setToolTip("L'Assomoir");
    }
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
            m_fileManager->scheduleSave(projectId, collectTasksForProject(projectId));

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

        for (const TaskItem &task : tasks) {
            if (!swimlane->column(task.stage)) {
                ui->consolePanel->appendLog(QString("[LOAD WARN] %1: stage '%2' tidak dikenal, task '%3' dilewati")
                                                .arg(projectId, task.stage, task.title));
                continue;
            }
            auto *card = new KanbanCardWidget(swimlane);
            card->setCardData(task.id, task.category, task.title, task.subtext, task.badge);
            swimlane->addCardToStage(task.stage, card);
        }
    }

    if (!projectIds.isEmpty()) {
        ui->consolePanel->appendLog("--- SYSTEM INITIALIZED ---");
        ui->consolePanel->appendLog("Projects loaded: " + projectIds.join(", "));
    }
    return projectIds.size();
}

MainWindow::~MainWindow() {
    delete ui;
}

void MainWindow::addSwimlane(const QString &projectId) {
    if (m_swimlanes.contains(projectId)) return;

    auto *swimlane = new SwimlaneWidget(projectId, this);

    // Tangkap interaksi dari swimlane
    connect(swimlane, &SwimlaneWidget::cardMoved, this, &MainWindow::handleCardMoved);
    connect(swimlane, &SwimlaneWidget::closeProjectRequested, this, &MainWindow::handleCloseProjectRequested);

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

    auto *btnNewTask = new QPushButton(row);
    btnNewTask->setObjectName("btnProjectNewTask");
    btnNewTask->setIcon(QIcon(":/icons/plus.svg"));
    btnNewTask->setIconSize(QSize(12, 12));
    btnNewTask->setFixedSize(18, 18);
    btnNewTask->setCursor(Qt::PointingHandCursor);
    btnNewTask->setToolTip("New Task untuk " + projectId);
    connect(btnNewTask, &QPushButton::clicked, this, [this, projectId]() {
        handleNewTaskRequested(projectId);
    });

    // justify-between: label menempel kiri, tombol "+" menempel kanan
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(6, 3, 4, 3);
    rowLayout->setSpacing(4);
    rowLayout->addWidget(label);
    rowLayout->addStretch(1);
    rowLayout->addWidget(btnNewTask);

    return row;
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
            btn->setIcon(QIcon(selected ? ":/icons/plus-white.svg" : ":/icons/plus.svg"));
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
    QString timeStr = QDateTime::currentDateTime().toString("hh:mm:ss");
    QString logEntry = QString("[%1] [%2] '%3' moved to %4 (index %5)")
                           .arg(timeStr, projectId, card->title(), targetStage)
                           .arg(targetIndex);
    m_fileManager->scheduleSave(projectId, collectTasksForProject(projectId));

    ui->consolePanel->appendLog(logEntry);
}

void MainWindow::handleNewTaskRequested(const QString &projectId) {
    bool ok;
    QString title = QInputDialog::getText(this, "New Task",
                                          "Task Title for [" + projectId + "]:",
                                          QLineEdit::Normal, "", &ok);
    if (ok && !title.trimmed().isEmpty()) {
        auto *swimlane = m_swimlanes.value(projectId, nullptr);
        if (swimlane) {
            auto *card = new KanbanCardWidget(swimlane);
            QString taskId = QString::number(QDateTime::currentMSecsSinceEpoch());

            // Atur metadata task default
            card->setCardData(taskId, "component", title.trimmed(), "waiting in queue", "✓ 0");

            // Masukkan ke kolom pertama: WAITING
            swimlane->addCardToStage("WAITING", card);

            // Task baru ikut dipersistenkan; tanpa ini kartu hilang saat aplikasi ditutup
            m_fileManager->scheduleSave(projectId, collectTasksForProject(projectId));

            ui->consolePanel->appendLog(QString("[TASK CREATED] %1 -> WAITING: '%2'").arg(projectId, title.trimmed()));
        }
    }
}

void MainWindow::handleCloseProjectRequested(const QString &projectId) {
    auto *widget = m_swimlanes.value(projectId, nullptr);
    if (!widget) return;

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

    ui->consolePanel->appendLog(QString("[SYSTEM] Swimlane '%1' ditutup.").arg(projectId));
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

QMap<QString, TaskItem> MainWindow::collectTasksForProject(const QString &projectId) const {
    QMap<QString, TaskItem> tasks;
    auto *swimlane = m_swimlanes.value(projectId, nullptr);
    if (!swimlane) return tasks;

    for (KanbanColumnWidget *column : swimlane->columns()) {
        for (KanbanCardWidget *card : column->cards()) {
            TaskItem item;
            item.id = card->id();
            item.projectId = projectId;
            item.stage = column->stageName();
            item.category = card->category();
            item.title = card->title();
            item.subtext = card->subtext();
            item.badge = card->badge();
            tasks.insert(item.id, item);
        }
    }

    return tasks;
}

#include "MainWindow.h"
#include "ui_MainWindow.h"
#include "SwimlaneWidget.h"
#include "KanbanColumnWidget.h"
#include "KanbanCardWidget.h"
#include "TaskItem.h"
#include "ConsolePanelWidget.h"
#include "FileManager.h"
#include "NewTaskDialog.h"

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

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent),
    ui(new Ui::MainWindow) {
    m_fileManager = new FileManager(this);
    ui->setupUi(this);

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
            connect(card, &KanbanCardWidget::runRequested, this, [this](const QString &cardId) {
                ui->consolePanel->appendLog("[SYSTEM] Run task: " + cardId);
            });
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
    QString timeStr = QDateTime::currentDateTime().toString("hh:mm:ss");
    QString logEntry = QString("[%1] [%2] '%3' moved to %4 (index %5)")
                           .arg(timeStr, projectId, card->title(), targetStage)
                           .arg(targetIndex);
    m_fileManager->scheduleSave(projectId, collectTasksForProject(projectId));

    ui->consolePanel->appendLog(logEntry);
}

void MainWindow::handleNewTaskRequested(const QString &projectId) {
    NewTaskDialog dialog(projectId, this);
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

    auto *card = new KanbanCardWidget(swimlane);
    card->setCardData(item.id, item.category, item.title, item.subtext, item.badge);
    connect(card, &KanbanCardWidget::runRequested, this, [this](const QString &cardId) {
        ui->consolePanel->appendLog("[SYSTEM] Run task: " + cardId);
    });
    swimlane->addCardToStage(item.stage, card);

    // Task baru ikut dipersistenkan; tanpa ini kartu hilang saat aplikasi ditutup
    m_fileManager->scheduleSave(projectId, collectTasksForProject(projectId));

    ui->consolePanel->appendLog(QString("[TASK CREATED] %1 -> %2: '%3'").arg(projectId, item.stage, item.title));
}

void MainWindow::removeProjectFromUi(const QString &projectId) {
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

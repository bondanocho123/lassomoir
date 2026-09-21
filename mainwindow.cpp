#include "MainWindow.h"
#include "ui_MainWindow.h"
#include "SwimlaneWidget.h"
#include "KanbanCardWidget.h"
#include "ConsolePanelWidget.h"
#include "FileManager.h"

#include <QInputDialog>
#include <QDateTime>
#include <QVBoxLayout>
#include <QTimer>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent),
    ui(new Ui::MainWindow) {
    m_fileManager = new FileManager(this);
    ui->setupUi(this);
    ui->rootVerticalLayout->setStretch(1,1);
    QTimer::singleShot(0, this, [this]() {
        ui->mainSplitter->setSizes({1000, 350}); // swimlanes : console, boleh disesuaikan
    });

    // Sambungkan input teks dari panel konsol kanan
    connect(ui->consolePanel, &ConsolePanelWidget::commandSubmitted,
            this, &MainWindow::handleCommandSubmitted);

    // Tombol global New Project di top bar
    connect(ui->btnGlobalNewProject, &QPushButton::clicked, this, [this]() {
        bool ok;
        QString projectName = QInputDialog::getText(this, "New Project",
                                                    "Project Name / Target:",
                                                    QLineEdit::Normal, "", &ok);
        if (ok && !projectName.trimmed().isEmpty()) {
            addSwimlane(projectName.trimmed());
            ui->consolePanel->appendLog("[SYSTEM] Project swimlane baru dibuat: " + projectName.trimmed());
        }
    });

    loadInitialMockData();
}

MainWindow::~MainWindow() {
    delete ui;
}

void MainWindow::addSwimlane(const QString &projectId) {
    if (m_swimlanes.contains(projectId)) return;

    auto *swimlane = new SwimlaneWidget(projectId, ui->scrollAreaWidgetContents);

    // Tangkap interaksi dari swimlane
    connect(swimlane, &SwimlaneWidget::cardMoved, this, &MainWindow::handleCardMoved);
    connect(swimlane, &SwimlaneWidget::newTaskRequested, this, &MainWindow::handleNewTaskRequested);
    connect(swimlane, &SwimlaneWidget::closeProjectRequested, this, [this](const QString &projId) {
        if (m_swimlanes.contains(projId)) {
            auto *widget = m_swimlanes.take(projId);
            widget->deleteLater();
            ui->consolePanel->appendLog(QString("[SYSTEM] Swimlane '%1' ditutup.").arg(projId));
        }
    });

    // Sisipkan widget swimlane tepat di atas vertical spacer paling bawah
    auto *layout = qobject_cast<QVBoxLayout*>(ui->scrollAreaWidgetContents->layout());
    if (layout) {
        int insertPos = qMax(0, layout->count() - 1);
        layout->insertWidget(insertPos, swimlane);
    }

    m_swimlanes.insert(projectId, swimlane);
}

void MainWindow::handleCardMoved(const QString &projectId, KanbanCardWidget *card, const QString &targetStage, int targetIndex) {
    QString timeStr = QDateTime::currentDateTime().toString("hh:mm:ss");
    QString logEntry = QString("[%1] [%2] '%3' moved to %4 (index %5)")
                           .arg(timeStr, projectId, card->title(), targetStage)
                           .arg(targetIndex);

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

            ui->consolePanel->appendLog(QString("[TASK CREATED] %1 -> WAITING: '%2'").arg(projectId, title.trimmed()));
        }
    }
}

void MainWindow::handleCommandSubmitted(const QString &command) {
    if (command.toLower() == "clear") {
        // Handle clear jika diinginkan
    } else {
        ui->consolePanel->appendLog("[AGENT ECHO] Command diterima: " + command);
    }
}

void MainWindow::loadInitialMockData() {
    // 1. Swimlane TTT
    addSwimlane("TTT");
    auto *card1 = new KanbanCardWidget(this);
    card1->setCardData("task-1", "component", "unbeatable-ai", "12s 53ms | 107k", "✓ 1");
    card1->setFixedWidth(250);
    m_swimlanes["TTT"]->addCardToStage("CODER", card1);

    auto *card2 = new KanbanCardWidget(this);
    card2->setCardData("task-2", "component", "one-chance", "waiting in queue", "✓ 1");
    m_swimlanes["TTT"]->addCardToStage("CODER", card2);

    auto *card3 = new KanbanCardWidget(this);
    card3->setCardData("task-3", "component", "game-core", "run CRAP, DRY, coverage, and tests.", "✓ 2");
    m_swimlanes["TTT"]->addCardToStage("CLEANER", card3);

    // 2. Swimlane spacewar
    addSwimlane("spacewar");
    auto *card4 = new KanbanCardWidget(this);
    card4->setCardData("task-4", "utility", "geometry-epoc...", "waiting", "✓ 0");
    m_swimlanes["spacewar"]->addCardToStage("WAITING", card4);

    auto *card5 = new KanbanCardWidget(this);
    card5->setCardData("task-5", "utility", "shot-acquisition", "waiting", "✓ 0");
    m_swimlanes["spacewar"]->addCardToStage("WAITING", card5);

    auto *card6 = new KanbanCardWidget(this);
    card6->setCardData("task-6", "utility", "epoch-distance-memo", "02s 54ms | 354k", "✓ 0");
    m_swimlanes["spacewar"]->addCardToStage("CODER", card6);

    auto *card7 = new KanbanCardWidget(this);
    card7->setCardData("task-7", "utility", "entity-identity", "completed", "✓ 2");
    m_swimlanes["spacewar"]->addCardToStage("DONE", card7);

    // Log awal konsol
    ui->consolePanel->appendLog("--- SYSTEM INITIALIZED ---");
    ui->consolePanel->appendLog("Projects loaded: TTT, spacewar");
    ui->consolePanel->appendLog("Ready for instructions.");
}

QMap<QString, TaskItem> MainWindow::collectTasksForProject(const QString &projectId){
    QMap<QString, TaskItem> result;

    return result;
}
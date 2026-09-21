#include "SwimlaneWidget.h"
#include "KanbanColumnWidget.h"
#include "KanbanCardWidget.h"
#include "ui_SwimlaneWidget.h"

#include <QHBoxLayout>

// Daftar nama 8 kolom pipeline kanban
const QStringList PIPELINE_STAGES = {
    "WAITING", "SPECIFIER", "CODER", "CLEANER",
    "ARCHITECT", "HARDENER", "QA", "DONE"
};

SwimlaneWidget::SwimlaneWidget(const QString &projectId, QWidget *parent)
    : QWidget(parent),
    ui(new Ui::SwimlaneWidget),
    m_projectId(projectId) {
    ui->setupUi(this);

    // Hubungkan tombol di header swimlane
    if (ui->btnNewTask) {
        connect(ui->btnNewTask, &QPushButton::clicked, this, &SwimlaneWidget::onNewTaskClicked);
    }
    if (ui->btnClose) {
        connect(ui->btnClose, &QPushButton::clicked, this, &SwimlaneWidget::onCloseClicked);
    }

    initializeColumns();
    setProjectTitle(m_projectId);
}

SwimlaneWidget::~SwimlaneWidget() {
    delete ui;
}

void SwimlaneWidget::setProjectTitle(const QString &title) {
    if (ui->labelProjectTitle) {
        ui->labelProjectTitle->setText(title);
    }
}

void SwimlaneWidget::initializeColumns() {
    QLayout *layout = ui->columnsContainer->layout();
    auto *hLayout = qobject_cast<QHBoxLayout*>(layout);

    // Buat layout horizontal jika belum diset di Qt Designer
    if (!hLayout) {
        hLayout = new QHBoxLayout(ui->columnsContainer);
        hLayout->setSpacing(6);
        hLayout->setContentsMargins(0, 0, 0, 0);
    }


    // Bangun 8 kolom KanbanColumnWidget
    for (const QString &stage : PIPELINE_STAGES) {
        auto *colWidget = new KanbanColumnWidget(this);
        colWidget->setFixedWidth(400);
        colWidget->setStageName(stage);

        // Sambungkan sinyal drop kartu antar-kolom
        connect(colWidget, &KanbanColumnWidget::cardDropped,
                this, &SwimlaneWidget::handleCardDropped);

        hLayout->addWidget(colWidget);
        m_columns.insert(stage, colWidget);
    }
}

// Implementasi fungsi yang sebelumnya memicu undefined reference
void SwimlaneWidget::addCardToStage(const QString &stageName, KanbanCardWidget *card) {
    if (m_columns.contains(stageName) && card) {
        m_columns[stageName]->addCard(card);
    }
}

KanbanColumnWidget* SwimlaneWidget::column(const QString &stageName) const {
    return m_columns.value(stageName, nullptr);
}

void SwimlaneWidget::onNewTaskClicked() {
    emit newTaskRequested(m_projectId);
}

void SwimlaneWidget::onCloseClicked() {
    emit closeProjectRequested(m_projectId);
}

void SwimlaneWidget::handleCardDropped(KanbanCardWidget *card, const QString &targetStage, int targetIndex) {
    emit cardMoved(m_projectId, card, targetStage, targetIndex);
}
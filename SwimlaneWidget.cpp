#include "SwimlaneWidget.h"
#include "KanbanColumnWidget.h"
#include "KanbanCardWidget.h"
#include "ui_SwimlaneWidget.h"

#include <QHBoxLayout>
#include <QPushButton>
#include <QResizeEvent>

// Daftar nama 8 kolom pipeline kanban
const QStringList PIPELINE_STAGES = {
    "WAITING", "SPECIFIER", "CODER", "CLEANER",
    "ARCHITECT", "HARDENER", "QA", "DONE"
};

namespace {
// Ukuran dan jarak tombol New Task mengambang (FAB)
constexpr int kFabSize = 52;
// Margin cukup lebar agar FAB bebas dari scrollbar horizontal kolom
constexpr int kFabMargin = 24;
}

SwimlaneWidget::SwimlaneWidget(const QString &projectId, QWidget *parent)
    : QWidget(parent),
    ui(new Ui::SwimlaneWidget),
    m_projectId(projectId),
    m_btnNewTask(nullptr) {
    ui->setupUi(this);

    // Tombol New Task dibuat di kode (bukan di .ui) karena harus berada di luar
    // layout agar bisa mengambang di atas kolom-kolom kanban.
    m_btnNewTask = new QPushButton("+", this);
    m_btnNewTask->setObjectName("btnNewTask");
    m_btnNewTask->setFixedSize(kFabSize, kFabSize);
    m_btnNewTask->setCursor(Qt::PointingHandCursor);
    m_btnNewTask->setToolTip("New Task");
    m_btnNewTask->raise();
    connect(m_btnNewTask, &QPushButton::clicked, this, &SwimlaneWidget::onNewTaskClicked);

    if (ui->btnClose) {
        connect(ui->btnClose, &QPushButton::clicked, this, &SwimlaneWidget::onCloseClicked);
    }

    initializeColumns();
    setProjectTitle(m_projectId);

    // resizeEvent belum tentu terpanggil sebelum widget tampil pertama kali
    repositionNewTaskButton();
}

SwimlaneWidget::~SwimlaneWidget() {
    delete ui;
}

void SwimlaneWidget::setProjectTitle(const QString &title) {
    if (ui->labelProjectTitle) {
        ui->labelProjectTitle->setText(title);
    }
}

void SwimlaneWidget::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    repositionNewTaskButton();
}

void SwimlaneWidget::repositionNewTaskButton() {
    if (!m_btnNewTask) return;

    // Clamp agar FAB tidak pernah terdorong keluar widget saat pane dipersempit
    m_btnNewTask->move(qMax(0, width() - kFabSize - kFabMargin),
                       qMax(0, height() - kFabSize - kFabMargin));
    m_btnNewTask->raise();
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

#include "SwimlaneWidget.h"
#include "KanbanColumnWidget.h"
#include "KanbanCardWidget.h"
#include "StageCatalog.h"
#include "ui_SwimlaneWidget.h"

#include <QDir>
#include <QIcon>
#include <QHBoxLayout>

SwimlaneWidget::SwimlaneWidget(const QString &projectId, const StageCatalog &catalog, QWidget *parent)
    : QWidget(parent),
    ui(new Ui::SwimlaneWidget),
    m_projectId(projectId) {
    ui->setupUi(this);

    if (ui->btnClose) {
        connect(ui->btnClose, &QPushButton::clicked, this, &SwimlaneWidget::onCloseClicked);
    }
    // Ikon setinggi teks tombol. Font tombol baru berasal dari stylesheet setelah dipolish,
    // jadi polish dulu supaya yang diukur bukan font default aplikasi
    ui->btnWorkingDir->ensurePolished();
    const int iconSide = ui->btnWorkingDir->fontMetrics().height();
    ui->btnWorkingDir->setIconSize(QSize(iconSide, iconSide));
    connect(ui->btnWorkingDir, &QPushButton::clicked, this, [this]() {
        emit workingDirectoryChangeRequested(m_projectId);
    });

    initializeColumns(catalog);
    setProjectTitle(m_projectId);
    setWorkingDirectory(QString());
}

SwimlaneWidget::~SwimlaneWidget() {
    delete ui;
}

void SwimlaneWidget::setProjectTitle(const QString &title) {
    if (ui->labelProjectTitle) {
        ui->labelProjectTitle->setText(title);
    }
}

void SwimlaneWidget::setWorkingDirectory(const QString &path) {
    if (path.isEmpty()) {
        ui->btnWorkingDir->setIcon(QIcon(":/icons/folder-add.svg"));
        ui->btnWorkingDir->setText("Select");
        ui->btnWorkingDir->setToolTip("Folder tempat agent Claude Code bekerja untuk project ini");
        return;
    }

    const QString name = QDir(path).dirName();
    ui->btnWorkingDir->setIcon(QIcon(":/icons/folder.svg"));
    ui->btnWorkingDir->setText(name.isEmpty() ? path : name);
    ui->btnWorkingDir->setToolTip(QString("Folder kerja agent: %1\nKlik untuk mengganti")
                                      .arg(QDir::toNativeSeparators(path)));
}

KanbanCardWidget *SwimlaneWidget::cardById(const QString &taskId) const {
    for (KanbanColumnWidget *column : m_columns) {
        for (KanbanCardWidget *card : column->cards()) {
            if (card->id() == taskId) {
                return card;
            }
        }
    }
    return nullptr;
}

QString SwimlaneWidget::stageOf(const KanbanCardWidget *card) const {
    for (KanbanColumnWidget *column : m_columns) {
        for (KanbanCardWidget *candidate : column->cards()) {
            if (candidate == card) {
                return column->stageName();
            }
        }
    }
    return QString();
}

void SwimlaneWidget::initializeColumns(const StageCatalog &catalog) {
    QLayout *layout = ui->columnsContainer->layout();
    auto *hLayout = qobject_cast<QHBoxLayout*>(layout);

    // Buat layout horizontal jika belum diset di Qt Designer
    if (!hLayout) {
        hLayout = new QHBoxLayout(ui->columnsContainer);
        hLayout->setSpacing(6);
        hLayout->setContentsMargins(0, 0, 0, 0);
    }


    // Satu KanbanColumnWidget per stage, urut sesuai pipeline
    const QStringList stages = catalog.keys();
    for (const QString &stage : stages) {
        auto *colWidget = new KanbanColumnWidget(this);
        colWidget->setFixedWidth(400);
        colWidget->setStageName(stage);

        // Tombol run kartu hanya aktif di stage yang punya agent
        const StageProfile *profile = catalog.profile(stage);
        colWidget->setRunnable(profile && profile->agent());

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

void SwimlaneWidget::onCloseClicked() {
    emit closeProjectRequested(m_projectId);
}

void SwimlaneWidget::handleCardDropped(KanbanCardWidget *card, const QString &targetStage, int targetIndex) {
    emit cardMoved(m_projectId, card, targetStage, targetIndex);
}

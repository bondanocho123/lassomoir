#include "KanbanCardWidget.h"
#include "ui_KanbanCardWidget.h"

#include <QApplication>
#include <QMouseEvent>
#include <QDrag>
#include <QMimeData>
#include <QPainter>
#include <QPixmap>
#include <QStyle>

KanbanCardWidget::KanbanCardWidget(QWidget *parent) : QWidget(parent), ui(new Ui::KanbanCardWidget){
    ui->setupUi(this);

    // WA_StyledBackground wajib agar background/border-radius dari styles.qss
    // ikut dilukis; tanpa ini QWidget biasa mengabaikannya dan sudut tetap kotak
    setAttribute(Qt::WA_StyledBackground, true);

    // Pastikan widget kartu menangkap event hover/klik dengan baik
    setAttribute(Qt::WA_Hover, true);

    // Diwariskan ke semua anak (label, frame); tombol run sudah punya cursor sendiri dari .ui
    setCursor(Qt::PointingHandCursor);

    connect(ui->btnCardRun, &QPushButton::clicked, this, &KanbanCardWidget::onRunButtonClicked);
    refreshRunButton();
}

void KanbanCardWidget::setRunEnabled(bool enabled) {
    m_runEnabled = enabled;
    refreshRunButton();
}

void KanbanCardWidget::setRunState(RunState state) {
    if (m_runState == state) {
        return;
    }
    m_runState = state;
    refreshStateProperty();
    refreshRunButton();
}

void KanbanCardWidget::setTaskState(TaskState state, const QString &detail) {
    m_taskState = state;
    m_taskStateDetail = detail;
    refreshStateProperty();
    refreshRunButton();
}

void KanbanCardWidget::refreshStateProperty() {
    // Run yang aktif lebih penting ditampilkan daripada status task di stage-nya
    QString name = QStringLiteral("idle");
    if (m_runState == RunState::Queued) {
        name = QStringLiteral("queued");
    } else if (m_runState == RunState::Running) {
        name = QStringLiteral("running");
    } else if (m_taskState != TaskState::Idle) {
        name = taskStateKey(m_taskState);
    }
    if (property("state").toString() == name) {
        return;
    }

    // Garis kartu diatur styles.qss lewat dynamic property "state"; nilai yang berubah
    // setelah stylesheet terpasang butuh repolish supaya aturannya dihitung ulang
    setProperty("state", name);
    style()->unpolish(this);
    style()->polish(this);
}

void KanbanCardWidget::onRunButtonClicked() {
    if (m_runState != RunState::Idle) {
        emit cancelRequested(m_id);
    } else if (m_taskState == TaskState::AwaitingReview) {
        emit reviewRequested(m_id);
    } else {
        emit runRequested(m_id);
    }
}

void KanbanCardWidget::refreshRunButton() {
    if (m_runState != RunState::Idle) {
        // Run yang antre/berjalan selalu bisa dihentikan
        ui->btnCardRun->setIcon(QIcon(":/icons/stop.svg"));
        ui->btnCardRun->setEnabled(true);
        ui->btnCardRun->setToolTip(m_runState == RunState::Queued ? "Batalkan antrean" : "Hentikan agent");
        return;
    }

    if (m_taskState == TaskState::AwaitingReview) {
        ui->btnCardRun->setIcon(QIcon(":/icons/review.svg"));
        ui->btnCardRun->setEnabled(true);
        ui->btnCardRun->setToolTip("Review hasil agent: setujui, minta revisi, atau kembalikan");
        return;
    }

    ui->btnCardRun->setIcon(QIcon(":/icons/run.svg"));
    ui->btnCardRun->setEnabled(m_runEnabled);
    if (!m_runEnabled) {
        ui->btnCardRun->setToolTip("Stage ini tidak menjalankan agent. Pindahkan kartu ke SPECIFIER–QA dulu.");
    } else if (m_taskState == TaskState::Failed) {
        ui->btnCardRun->setToolTip(m_taskStateDetail.isEmpty()
                                       ? QStringLiteral("Coba lagi")
                                       : QStringLiteral("Coba lagi — %1").arg(m_taskStateDetail));
    } else {
        ui->btnCardRun->setToolTip("Jalankan agent stage ini");
    }
}

KanbanCardWidget::~KanbanCardWidget() {
    delete ui;
}

void KanbanCardWidget::setCardData(const QString &id,
                                   const QString &category,
                                   const QString &title,
                                   const QString &subtext,
                                   const QString &badge){
    m_id = id;
    m_category = category;
    m_title = title;
    m_subtext = subtext;
    m_badge = badge;

    updateUI();
}

void KanbanCardWidget::updateUI() {
    // Sesuaikan nama objek QLabel ini dengan nama yang Anda buat di KanbanCardWidget.ui
    if (ui->labelCategory) {
        ui->labelCategory->setText(m_category);
        ui->labelCategory->setVisible(!m_category.isEmpty());
    }

    if (ui->labelBadge) {
        ui->labelBadge->setText(m_badge);
        ui->labelBadge->setVisible(!m_badge.isEmpty());
    }

    if (ui->labelTitle) {
        ui->labelTitle->setText(m_title);
        ui->labelTitle->setVisible(!m_title.isEmpty());
    }

    if (ui->labelSubtext) {
        ui->labelSubtext->setText(m_subtext);
        ui->labelSubtext->setVisible(!m_subtext.isEmpty());
    }
}


void KanbanCardWidget::mousePressEvent(QMouseEvent *event){
    if (event->button() == Qt::LeftButton){
        m_dragStartPosition = event->pos();
        m_dragStarted = false;
        emit cardClicked(m_id);
    }
    QWidget::mousePressEvent(event);
}

void KanbanCardWidget::mouseReleaseEvent(QMouseEvent *event){
    // Klik biasa (tanpa drag): buka hasil agent di drawer
    if (event->button() == Qt::LeftButton && !m_dragStarted
        && (event->pos() - m_dragStartPosition).manhattanLength() < QApplication::startDragDistance()) {
        emit detailsRequested(m_id);
    }
    QWidget::mouseReleaseEvent(event);
}

void KanbanCardWidget::mouseDoubleClickEvent(QMouseEvent *event){
    if (event->button() == Qt::LeftButton){
        emit editRequested(m_id);
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void KanbanCardWidget::mouseMoveEvent(QMouseEvent *event){
    //Pastikan drag hanya aktif jika tombol kiri ditekan
    if (!(event->buttons() == Qt::LeftButton)){
        return;
    }

    // Kartu yang agent-nya antre/berjalan, atau yang menunggu review, dikunci di stage-nya
    if (m_runState != RunState::Idle || m_taskState == TaskState::AwaitingReview) {
        return;
    }

    // Hindari trigger drag instan sebelum melewati ambang batas jarak klik (drag threshold)
    if ((event->pos() - m_dragStartPosition).manhattanLength() < QApplication::startDragDistance()){
        return;
    }
    m_dragStarted = true;

    auto *drag = new QDrag(this);
    auto *mimeData = new QMimeData();

    // Serialisasikan alamat memori pointer 'this' ke MIME data
    QByteArray itemData;
    QDataStream dataStream(&itemData, QIODevice::WriteOnly);
    dataStream << reinterpret_cast<quintptr>(this);

    mimeData->setData("application/x-kanbancard", itemData);
    drag->setMimeData(mimeData);

    // Buat visual preview kartu semi-transparan saat melayang
    QPixmap pixmap = this->grab();
    QPixmap ghostPixmap(pixmap.size());
    ghostPixmap.fill(Qt::transparent);

    QPainter painter(&ghostPixmap);
    painter.setOpacity(0.75);
    painter.drawPixmap(0, 0, pixmap);
    painter.end();

    drag->setPixmap(ghostPixmap);
    drag->setHotSpot(event->pos());

    //sembunyikan kartu di slot asalnya saat sedang ditarik
    this->hide();

    // eksekusi proses drag
    Qt::DropAction dropAction = drag->exec(Qt::MoveAction);

    // Jika drop dibatalkan atau gagal ditaruh pada kolom yang valid, tampilkan kembali di posisi semula
    if (dropAction != Qt::MoveAction) {
        this->show();
    }
}




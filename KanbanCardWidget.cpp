#include "KanbanCardWidget.h"
#include "ui_KanbanCardWidget.h"

#include <QApplication>
#include <QMouseEvent>
#include <QDrag>
#include <QMimeData>
#include <QPainter>
#include <QPixmap>

KanbanCardWidget::KanbanCardWidget(QWidget *parent) : QWidget(parent), ui(new Ui::KanbanCardWidget){
    ui->setupUi(this);

    // WA_StyledBackground wajib agar background/border-radius dari styles.qss
    // ikut dilukis; tanpa ini QWidget biasa mengabaikannya dan sudut tetap kotak
    setAttribute(Qt::WA_StyledBackground, true);

    // Pastikan widget kartu menangkap event hover/klik dengan baik
    setAttribute(Qt::WA_Hover, true);

    connect(ui->btnCardRun, &QPushButton::clicked, this, [this]() {
        emit runRequested(m_id);
    });
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
        emit cardClicked(m_id);
    }
    QWidget::mousePressEvent(event);
}

void KanbanCardWidget::mouseMoveEvent(QMouseEvent *event){
    //Pastikan drag hanya aktif jika tombol kiri ditekan
    if (!(event->buttons() == Qt::LeftButton)){
        return;
    }

    // Hindari trigger drag instan sebelum melewati ambang batas jarak klik (drag threshold)
    if ((event->pos() - m_dragStartPosition).manhattanLength() < QApplication::startDragDistance()){
        return;
    }

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




#include "KanbanColumnWidget.h"
#include "KanbanCardWidget.h"
#include "ui_KanbanColumnWidget.h"


#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QVBoxLayout>


KanbanColumnWidget::KanbanColumnWidget(QWidget *parent) : QWidget(parent), ui(new Ui::KanbanColumnWidget) {
    ui->setupUi(this);

    setAttribute(Qt::WA_StyledBackground, true);

    //aktifkan penerimaan drop
    setAcceptDrops(true);

    //ambil referensi ke layout penampung kartu; buat kalau .ui belum menyediakannya
    m_cardListLayout = qobject_cast<QVBoxLayout*>(ui->cardsContainer->layout());
    if (!m_cardListLayout) {
        m_cardListLayout = new QVBoxLayout(ui->cardsContainer);
        m_cardListLayout->setContentsMargins(4, 4, 4, 4);
        m_cardListLayout->setSpacing(6);
        m_cardListLayout->addStretch(1);   // spacer bawah: kartu menumpuk ke atas
    }
}

KanbanColumnWidget::~KanbanColumnWidget() {
    delete ui;
}

void KanbanColumnWidget::setStageName(const QString &name){
    m_stageName = name;
    if (ui->labelStageTitle){
        ui->labelStageTitle->setText(m_stageName);
    }
}

void KanbanColumnWidget::setRunnable(bool runnable) {
    m_runnable = runnable;
    for (KanbanCardWidget *card : cards()) {
        card->setRunEnabled(m_runnable);
    }
}

void KanbanColumnWidget::addCard(KanbanCardWidget *card) {
    if (!card || !m_cardListLayout) return;
    //Sisipkan sebelum spacer terbawah (Jika ada spacer di index terakhir)
    int targetIndex = qMax(0, m_cardListLayout->count()-1);
    m_cardListLayout->insertWidget(targetIndex, card);
    card->setRunEnabled(m_runnable);
    card->show();
}

void KanbanColumnWidget::insertCard(int index, KanbanCardWidget *card){
    if (!card || !m_cardListLayout) return;
    m_cardListLayout->insertWidget(index, card);
    // Kartu yang di-drop ke kolom lain ikut menyesuaikan tombol run-nya
    card->setRunEnabled(m_runnable);
    card->show();
}

void KanbanColumnWidget::removeCard(KanbanCardWidget *card) {
    if (!card || !m_cardListLayout) return;
    m_cardListLayout->removeWidget(card);
}

int KanbanColumnWidget::cardCount() const {
    if (!m_cardListLayout) return 0;
    int count = 0;
    for (int i = 0; i < m_cardListLayout->count(); i++){
        if (qobject_cast<KanbanCardWidget*>(m_cardListLayout->itemAt(i)->widget())){
            count++;
        }
    }
    return count;
}

QList<KanbanCardWidget *> KanbanColumnWidget::cards() const {
    QList<KanbanCardWidget *> result;
    if (!m_cardListLayout) return result;
    for (int i = 0; i < m_cardListLayout->count(); i++){
        if (auto *card = qobject_cast<KanbanCardWidget*>(m_cardListLayout->itemAt(i)->widget())){
            result.append(card);
        }
    }
    return result;
}

int KanbanColumnWidget::calculateInsertIndex(int dropY, const KanbanCardWidget *exclude) const {
    if (!m_cardListLayout) return 0;

    int insertIdx = 0;
    for (int i = 0; i < m_cardListLayout->count(); i++){
        QWidget *widget = m_cardListLayout->itemAt(i)->widget();
        auto *card = qobject_cast<KanbanCardWidget*>(widget);
        // if (!card) continue;
        if (!card || card == exclude) continue;

        //Map posisi Y kartu ke koordinat lokal kolom
        QPoint cardPosInColumn = card->mapTo(this, QPoint(0,0));
        int cardMiddleY = cardPosInColumn.y() + (card->height() / 2);

        if (dropY < cardMiddleY) {
            // return i;
            return insertIdx;
        }

        // insertIdx = i + 1;
        insertIdx++;
    }
    return insertIdx;
}

void KanbanColumnWidget::dragEnterEvent(QDragEnterEvent *event){
    if (event->mimeData()->hasFormat("application/x-kanbancard")){
        event->acceptProposedAction();
    }
}

void KanbanColumnWidget::dragMoveEvent(QDragMoveEvent *event){
    if (event->mimeData()->hasFormat("application/x-kanbancard")){
        event->acceptProposedAction();
    }
}

void KanbanColumnWidget::dragLeaveEvent(QDragLeaveEvent *event){
    event->accept();
}

void KanbanColumnWidget::dropEvent(QDropEvent *event){
    const QMimeData *mime = event->mimeData();
    if (!mime->hasFormat("application/x-kanbancard")) return;

    // Baca pointer widget kartu dari MIME data
    QByteArray itemData = mime->data("application/x-kanbancard");
    QDataStream stream(&itemData, QIODevice::ReadOnly);
    quintptr ptr;
    stream >> ptr;

    auto *draggedCard = reinterpret_cast<KanbanCardWidget*>(ptr);
    if (draggedCard){
        int targetIndex = calculateInsertIndex(event->position().toPoint().y(), draggedCard);

        //Pindahkan kartu ke layout kolom ini
        insertCard(targetIndex, draggedCard);

        emit cardDropped(draggedCard, m_stageName, targetIndex);
        event->acceptProposedAction();
    }
}


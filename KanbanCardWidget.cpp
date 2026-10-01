#include "KanbanCardWidget.h"
#include "ui_KanbanCardWidget.h"
#include "RunPulse.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QMouseEvent>
#include <QDrag>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QPixmap>
#include <QStyle>

namespace {

constexpr int kSummaryLines = 3;
constexpr int kSummaryChars = 140;

// Subtext bisa berupa prompt panjang; kartu cukup menampilkan beberapa baris pertamanya
QString summaryOf(const QString &text, bool *truncated) {
    QStringList lines;
    *truncated = false;
    const QStringList all = text.split(QLatin1Char('\n'));
    for (const QString &raw : all) {
        const QString line = raw.trimmed();
        if (line.isEmpty()) {
            continue;
        }
        if (lines.size() == kSummaryLines) {
            *truncated = true;
            break;
        }
        lines.append(line);
    }
    QString summary = lines.join(QLatin1Char('\n'));
    if (summary.size() > kSummaryChars) {
        summary = summary.left(kSummaryChars - 1).trimmed();
        *truncated = true;
    }
    return *truncated ? summary + QChar(0x2026) : summary;   // …
}

}

KanbanCardWidget::KanbanCardWidget(QWidget *parent)
    : QWidget(parent), ui(new Ui::KanbanCardWidget), m_pulse(new RunPulse(this)) {
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
    setAttachments(0, 0);
    setProperty("done", false);
    refreshDoneMarker();
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
    m_pulse->setActive(state == RunState::Running);
    refreshStateProperty();
    refreshRunButton();
    refreshDoneMarker();
}

void KanbanCardWidget::setCompletedStage(const QString &stage) {
    m_completedStage = stage;
    refreshDoneMarker();
}

void KanbanCardWidget::refreshDoneMarker() {
    const bool done = !m_completedStage.isEmpty() && m_runState == RunState::Idle;
    // Di ujung pipeline cukup "Selesai"; di stage lain sebut stage yang baru saja selesai
    ui->labelDone->setText(m_completedStage == QLatin1String("DONE")
                               ? QStringLiteral("✓ Selesai")
                               : QStringLiteral("✓ %1 selesai").arg(m_completedStage));
    ui->labelDone->setVisible(done);
    if (property("done").toBool() == done) {
        return;
    }
    setProperty("done", done);
    style()->unpolish(this);
    style()->polish(this);
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
        bool truncated = false;
        ui->labelSubtext->setText(summaryOf(m_subtext, &truncated));
        // Teks lengkap tetap bisa dibaca lewat tooltip; klik dua kali untuk mengubahnya
        ui->labelSubtext->setToolTip(truncated ? m_subtext.trimmed() : QString());
        ui->labelSubtext->setVisible(!m_subtext.trimmed().isEmpty());
    }
}

void KanbanCardWidget::setAttachments(int images, int documents, const QStringList &names) {
    QStringList parts;
    if (images > 0) {
        parts.append(QStringLiteral("%1 foto").arg(images));
    }
    if (documents > 0) {
        parts.append(QStringLiteral("%1 file").arg(documents));
    }
    ui->labelAttachments->setText(parts.join(QStringLiteral(" · ")));
    ui->labelAttachments->setToolTip(names.join(QLatin1Char('\n')));
    ui->labelAttachments->setVisible(!parts.isEmpty());
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

void KanbanCardWidget::contextMenuEvent(QContextMenuEvent *event){
    // popup() tidak memblokir (beda dengan exec()); menu membuang dirinya sendiri saat tertutup
    auto *menu = new QMenu(this);
    menu->setObjectName("cardContextMenu");
    menu->setAttribute(Qt::WA_DeleteOnClose);

    QAction *edit = menu->addAction(QStringLiteral("Edit task…"));
    edit->setObjectName("actionEditTask");
    connect(edit, &QAction::triggered, this, [this]() { emit editRequested(m_id); });

    // Sama dengan drag: kartu yang agent-nya antre/berjalan, atau yang menunggu review, dikunci
    QMenu *move = menu->addMenu(QStringLiteral("Pindah ke stage"));
    move->setObjectName("cardMoveMenu");
    move->menuAction()->setObjectName("actionMoveTask");
    move->setEnabled(!m_moveTargets.isEmpty() && m_runState == RunState::Idle
                     && m_taskState != TaskState::AwaitingReview);
    for (const QString &stage : std::as_const(m_moveTargets)) {
        QAction *target = move->addAction(stage);
        target->setObjectName(QStringLiteral("actionMoveTo_") + stage);
        // Stage sekarang tetap tampil (bertanda ✓) supaya posisi kartu di pipeline terlihat
        if (stage == m_stage) {
            target->setCheckable(true);
            target->setChecked(true);
            target->setEnabled(false);
            continue;
        }
        connect(target, &QAction::triggered, this, [this, stage]() { emit moveRequested(m_id, stage); });
    }

    menu->addSeparator();
    QAction *remove = menu->addAction(QIcon(":/icons/trash.svg"), QStringLiteral("Hapus task…"));
    remove->setObjectName("actionDeleteTask");
    connect(remove, &QAction::triggered, this, [this]() { emit deleteRequested(m_id); });

    menu->popup(event->globalPos());
    event->accept();
}

void KanbanCardWidget::paintEvent(QPaintEvent *event){
    QWidget::paintEvent(event);
    if (m_pulse->isActive()) {
        QPainter painter(this);
        // Radius sama dengan border-radius kartu di styles.qss
        m_pulse->paint(painter, rect(), 10.0);
    }
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




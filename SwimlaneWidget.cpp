#include "SwimlaneWidget.h"
#include "GitHistory.h"
#include "KanbanColumnWidget.h"
#include "KanbanCardWidget.h"
#include "StageCatalog.h"
#include "StageInfo.h"
#include "ui_SwimlaneWidget.h"
#include "Theme.h"

#include <QDir>
#include <QFrame>
#include <QGuiApplication>
#include <QIcon>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScreen>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace {
// Lebar tetap tiap kolom stage (px logis)
constexpr int kColumnWidth = 300;
}

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
    ui->btnReferenceDirs->setIconSize(QSize(iconSide, iconSide));
    ui->btnReferenceDirs->setIcon(Theme::icon(":/icons/folders.svg"));
    connect(ui->btnReferenceDirs, &QPushButton::clicked, this, &SwimlaneWidget::showReferencePopup);
    ui->btnBranch->ensurePolished();
    ui->btnBranch->setIconSize(QSize(iconSide, iconSide));
    ui->btnBranch->setIcon(Theme::icon(":/icons/branch.svg"));
    connect(ui->btnBranch, &QPushButton::clicked, this, [this]() {
        emit branchViewRequested(m_projectId);
    });

    initializeColumns(catalog);
    setProjectTitle(m_projectId);
    setWorkingDirectory(QString());
    setGitHead(GitHead());
    setReferenceDirectories(QStringList());
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
        ui->btnWorkingDir->setIcon(Theme::icon(":/icons/folder-add.svg"));
        ui->btnWorkingDir->setText("Select");
        ui->btnWorkingDir->setToolTip("Folder tempat agent Claude Code bekerja untuk project ini");
        return;
    }

    const QString name = QDir(path).dirName();
    ui->btnWorkingDir->setIcon(Theme::icon(":/icons/folder.svg"));
    ui->btnWorkingDir->setText(name.isEmpty() ? path : name);
    ui->btnWorkingDir->setToolTip(QString("Folder kerja agent: %1\nKlik untuk mengganti")
                                      .arg(QDir::toNativeSeparators(path)));
}

void SwimlaneWidget::setGitHead(const GitHead &head) {
    ui->btnBranch->setVisible(head.repository);
    if (!head.repository) {
        return;
    }

    QString text;
    QString tip;
    if (!head.branch.isEmpty()) {
        text = head.branch;
        tip = head.commit.isEmpty() ? QStringLiteral("Folder kerja di branch %1 (belum punya commit)").arg(head.branch)
                                    : QStringLiteral("Folder kerja sedang di branch %1 (%2)").arg(head.branch, head.commit);
    } else {
        text = QStringLiteral("HEAD %1").arg(head.commit);
        tip = QStringLiteral("Folder kerja tidak di branch mana pun (detached HEAD di %1)").arg(head.commit);
    }
    // Nama branch task bisa panjang: dipendekkan di tengah, lengkapnya di tooltip. "&" digandakan
    // supaya tidak dibaca sebagai penanda shortcut.
    text = ui->btnBranch->fontMetrics().elidedText(text, Qt::ElideMiddle, 180);
    ui->btnBranch->setText(text.replace(QLatin1Char('&'), QStringLiteral("&&")));
    ui->btnBranch->setToolTip(tip + QStringLiteral("\nKlik untuk melihat branch, daftar commit, dan membandingkan perubahan"));
}

void SwimlaneWidget::setReferenceDirectories(const QStringList &dirs) {
    m_referenceDirs = dirs;
    ui->btnReferenceDirs->setText(dirs.isEmpty() ? QStringLiteral("Referensi")
                                                 : QStringLiteral("Referensi · %1").arg(dirs.size()));
    QStringList tip = {QStringLiteral("Folder referensi: dibaca agent sebagai acuan, isinya tidak diubah")};
    for (const QString &dir : dirs) {
        tip.append(QStringLiteral("• %1").arg(QDir::toNativeSeparators(dir)));
    }
    if (dirs.isEmpty()) {
        tip.append(QStringLiteral("Klik untuk menambah folder di luar folder kerja"));
    }
    ui->btnReferenceDirs->setToolTip(tip.join(QLatin1Char('\n')));

    // Popup yang terbuka (mis. tombol × di dalamnya baru diklik) diisi ulang di tempat. Menutup lalu
    // membuka popup baru tidak dipakai: aktivasi ulang jendela utama ikut menutup popup baru itu.
    if (m_referencePopup && m_referencePopup->isVisible()) {
        fillReferencePopup(m_referencePopup);
        placeReferencePopup(m_referencePopup);
    }
}

void SwimlaneWidget::showReferencePopup() {
    QPushButton *anchor = ui->btnReferenceDirs;
    if (!anchor->isVisible()) {
        return;
    }
    if (m_referencePopup) {
        m_referencePopup->close();
    }

    // Qt::Popup: tertutup sendiri saat klik di luar, seperti popup konfirmasi hapus project
    auto *popup = new QFrame(anchor, Qt::Popup);
    popup->setObjectName("referencePopup");
    popup->setAttribute(Qt::WA_DeleteOnClose);
    popup->setMinimumWidth(320);
    auto *layout = new QVBoxLayout(popup);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(6);
    m_referencePopup = popup;

    fillReferencePopup(popup);
    placeReferencePopup(popup);
    popup->show();
}

void SwimlaneWidget::fillReferencePopup(QFrame *popup) {
    // Isi lama (berisi tombol × yang mungkin sedang mengirim sinyal klik) disembunyikan dan dihapus
    // belakangan. Isi baru dirakit dalam wadah yang belum tampil lalu ditampilkan sekaligus, supaya
    // ukurannya sudah benar saat popup diukur ulang.
    QLayout *popupLayout = popup->layout();
    while (QLayoutItem *item = popupLayout->takeAt(0)) {
        if (QWidget *old = item->widget()) {
            old->hide();
            old->deleteLater();
        }
        delete item;
    }
    auto *content = new QWidget(popup);
    content->setObjectName("referencePopupContent");
    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    auto *title = new QLabel(QStringLiteral("FOLDER REFERENSI"), content);
    title->setObjectName("referencePopupTitle");
    auto *hint = new QLabel(QStringLiteral("Folder di luar folder kerja yang boleh dibaca agent sebagai acuan. "
                                           "Agent tidak bisa mengubah isinya."),
                            content);
    hint->setObjectName("referencePopupHint");
    hint->setWordWrap(true);
    layout->addWidget(title);
    layout->addWidget(hint);

    if (m_referenceDirs.isEmpty()) {
        auto *empty = new QLabel(QStringLiteral("Belum ada folder referensi."), content);
        empty->setObjectName("referencePopupEmpty");
        layout->addWidget(empty);
    }
    for (const QString &dir : std::as_const(m_referenceDirs)) {
        auto *row = new QWidget(content);
        row->setObjectName("referenceRow");

        auto *icon = new QLabel(row);
        icon->setPixmap(Theme::icon(":/icons/folder.svg").pixmap(QSize(14, 14)));
        const QString name = QDir(dir).dirName();
        auto *label = new QLabel(name.isEmpty() ? QDir::toNativeSeparators(dir) : name, row);
        label->setObjectName("referenceRowName");
        auto *path = new QLabel(row);
        path->setObjectName("referenceRowPath");
        path->ensurePolished();
        path->setText(path->fontMetrics().elidedText(QDir::toNativeSeparators(dir), Qt::ElideMiddle, 280));
        path->setToolTip(QDir::toNativeSeparators(dir));

        auto *remove = new QToolButton(row);
        remove->setObjectName("btnReferenceRemove");
        remove->setIcon(Theme::icon(":/icons/close.svg"));
        remove->setIconSize(QSize(10, 10));
        remove->setCursor(Qt::PointingHandCursor);
        remove->setToolTip(QStringLiteral("Hapus dari folder referensi"));
        connect(remove, &QToolButton::clicked, this, [this, dir]() {
            emit referenceDirectoryRemoveRequested(m_projectId, dir);
        });

        auto *text = new QVBoxLayout();
        text->setSpacing(0);
        text->addWidget(label);
        text->addWidget(path);
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 2, 0, 2);
        rowLayout->setSpacing(8);
        rowLayout->addWidget(icon);
        rowLayout->addLayout(text, 1);
        rowLayout->addWidget(remove);
        layout->addWidget(row);
    }

    auto *add = new QPushButton(Theme::icon(":/icons/folder-add.svg"), QStringLiteral("Tambah folder…"), content);
    add->setObjectName("btnReferenceAdd");
    add->setCursor(Qt::PointingHandCursor);
    connect(add, &QPushButton::clicked, this, [this, popup]() {
        popup->close();
        // Pemilih folder (dialog modal) dibuka setelah klik ini selesai diproses
        QTimer::singleShot(0, this, [this]() { emit referenceDirectoryAddRequested(m_projectId); });
    });
    auto *actions = new QHBoxLayout();
    actions->addStretch(1);
    actions->addWidget(add);
    layout->addLayout(actions);

    popupLayout->addWidget(content);
    content->show();
}

void SwimlaneWidget::placeReferencePopup(QFrame *popup) {
    // Ukuran dari isi yang baru; isi lama yang menunggu deleteLater sudah keluar dari layout
    popup->layout()->activate();
    popup->resize(popup->sizeHint().expandedTo(popup->minimumSize()));

    // Tepi kanan popup sejajar tepi kanan tombol, menggantung di bawahnya; tetap di dalam layar
    QPushButton *anchor = ui->btnReferenceDirs;
    const QPoint anchorBottomRight = anchor->mapToGlobal(QPoint(anchor->width(), anchor->height()));
    QPoint pos(anchorBottomRight.x() - popup->width(), anchorBottomRight.y() + 6);
    if (QScreen *screen = QGuiApplication::screenAt(anchorBottomRight)) {
        const QRect available = screen->availableGeometry();
        pos.setX(qBound(available.left() + 4, pos.x(), available.right() - popup->width() - 4));
        if (pos.y() + popup->height() > available.bottom()) {
            pos.setY(anchor->mapToGlobal(QPoint(0, 0)).y() - popup->height() - 6);
        }
    }
    popup->move(pos);
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
        colWidget->setFixedWidth(kColumnWidth);
        colWidget->setStageName(stage);
        colWidget->setStageInfo(StageInfo::html(catalog, stage));

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

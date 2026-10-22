#include "SwimlaneWidget.h"
#include "GitHistory.h"
#include "KanbanColumnWidget.h"
#include "KanbanCardWidget.h"
#include "ProjectGuidelines.h"
#include "StageCatalog.h"
#include "StageInfo.h"
#include "ui_SwimlaneWidget.h"
#include "Theme.h"

#include <QDesktopServices>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QFrame>
#include <QGuiApplication>
#include <QIcon>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QScreen>
#include <QTimer>
#include <QUrl>
#include <QToolButton>
#include <QVBoxLayout>

#include <functional>

namespace {
// Lebar tetap tiap kolom stage (px logis)
constexpr int kColumnWidth = 300;
// Judul project boleh dipendekkan sampai selebar ini sebelum tombol header tinggal ikon
constexpr int kTitleMinWidth = 60;
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
    ui->btnGuidelines->setIconSize(QSize(iconSide, iconSide));
    ui->btnGuidelines->setIcon(Theme::icon(":/icons/guidelines.svg"));
    connect(ui->btnGuidelines, &QPushButton::clicked, this, &SwimlaneWidget::showGuidelinePopup);
    ui->btnBranch->ensurePolished();
    ui->btnBranch->setIconSize(QSize(iconSide, iconSide));
    ui->btnBranch->setIcon(Theme::icon(":/icons/branch.svg"));
    connect(ui->btnBranch, &QPushButton::clicked, this, [this]() {
        emit branchViewRequested(m_projectId);
    });
    ui->btnCanvas->setIconSize(QSize(iconSide, iconSide));
    ui->btnCanvas->setIcon(Theme::icon(":/icons/canvas.svg"));
    connect(ui->btnCanvas, &QPushButton::clicked, this, [this]() {
        emit canvasRequested(m_projectId);
    });

    // Judul menyempit (terpotong "…") lebih dulu daripada mendorong tombol keluar dari header
    ui->labelProjectTitle->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
    m_headerButtonMinWidth = ui->btnClose->minimumWidth();
    setHeaderText(ui->btnCanvas, ui->btnCanvas->text());

    initializeColumns(catalog);
    setProjectTitle(m_projectId);
    setWorkingDirectory(QString());
    setGitHead(GitHead());
    setReferenceDirectories(QStringList());
    setGuidelines(QString(), QStringList());
}

SwimlaneWidget::~SwimlaneWidget() {
    delete ui;
}

void SwimlaneWidget::setProjectTitle(const QString &title) {
    if (ui->labelProjectTitle) {
        ui->labelProjectTitle->setFullText(title);
    }
}

void SwimlaneWidget::setWorkingDirectory(const QString &path) {
    if (path.isEmpty()) {
        ui->btnWorkingDir->setIcon(Theme::icon(":/icons/folder-add.svg"));
        setHeaderText(ui->btnWorkingDir, "Select");
        ui->btnWorkingDir->setToolTip("Folder tempat agent Claude Code bekerja untuk project ini");
        return;
    }

    const QString name = QDir(path).dirName();
    ui->btnWorkingDir->setIcon(Theme::icon(":/icons/folder.svg"));
    setHeaderText(ui->btnWorkingDir, name.isEmpty() ? path : name);
    ui->btnWorkingDir->setToolTip(QString("Folder kerja agent: %1\nKlik untuk mengganti")
                                      .arg(QDir::toNativeSeparators(path)));
}

void SwimlaneWidget::setGitHead(const GitHead &head) {
    ui->btnBranch->setVisible(head.repository);
    if (!head.repository) {
        remeasureHeader();
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
    setHeaderText(ui->btnBranch, text.replace(QLatin1Char('&'), QStringLiteral("&&")));
    ui->btnBranch->setToolTip(tip + QStringLiteral("\nKlik untuk melihat branch, daftar commit, dan membandingkan perubahan"));
}

void SwimlaneWidget::setReferenceDirectories(const QStringList &dirs) {
    m_referenceDirs = dirs;
    setHeaderText(ui->btnReferenceDirs, dirs.isEmpty() ? QStringLiteral("Referensi")
                                                       : QStringLiteral("Referensi · %1").arg(dirs.size()));
    QStringList tip = {QStringLiteral("Folder referensi: dibaca agent sebagai acuan, isinya tidak diubah")};
    for (const QString &dir : dirs) {
        tip.append(QStringLiteral("• %1").arg(QDir::toNativeSeparators(dir)));
    }
    if (dirs.isEmpty()) {
        tip.append(QStringLiteral("Klik untuk menambah folder di luar folder kerja"));
    }
    ui->btnReferenceDirs->setToolTip(tip.join(QLatin1Char('\n')));
    refreshListPopup(ListPopup::References);
}

void SwimlaneWidget::setGuidelines(const QString &directory, const QStringList &names) {
    m_guidelineDir = directory;
    m_guidelines = names;
    setHeaderText(ui->btnGuidelines, names.isEmpty() ? QStringLiteral("Pedoman")
                                                     : QStringLiteral("Pedoman · %1").arg(names.size()));
    QStringList tip = {QStringLiteral("Pedoman project: batasan & guideline (framework, arsitektur, gaya kode) "
                                      "yang wajib diikuti agent di semua task")};
    for (const QString &name : names) {
        tip.append(QStringLiteral("• %1").arg(name));
    }
    if (names.isEmpty()) {
        tip.append(QStringLiteral("Klik untuk menambah file Markdown, Word, PDF, atau teks"));
    }
    ui->btnGuidelines->setToolTip(tip.join(QLatin1Char('\n')));
    refreshListPopup(ListPopup::Guidelines);
}

void SwimlaneWidget::refreshListPopup(ListPopup kind) {
    // Popup yang terbuka (mis. tombol × di dalamnya baru diklik) diisi ulang di tempat. Menutup lalu
    // membuka popup baru tidak dipakai: aktivasi ulang jendela utama ikut menutup popup baru itu.
    if (m_listPopup && m_listPopup->isVisible() && m_listPopupKind == kind) {
        fillListPopup(m_listPopup);
        placeListPopup(m_listPopup);
    }
}

void SwimlaneWidget::setHeaderText(QPushButton *button, const QString &text) {
    m_headerTexts.insert(button, text);
    remeasureHeader();
}

void SwimlaneWidget::remeasureHeader() {
    m_fullHeaderWidth = -1;
    updateHeaderMode();
}

void SwimlaneWidget::updateHeaderMode() {
    // Stylesheet dari file .ui terpasang sebelum tombol-tombolnya dibuat (lihat changeEvent)
    if (m_headerTexts.isEmpty()) {
        return;
    }
    if (m_fullHeaderWidth < 0) {
        // Diukur dengan teks tombolnya terpasang: judul sependek-pendeknya, lalu tiap tombol yang
        // tampil beserta jaraknya dari isi di kirinya
        setHeaderCompact(false);
        const int spacing = ui->headerLayout->spacing();
        int width = kTitleMinWidth + spacing;
        const QList<QPushButton *> buttons = {ui->btnCanvas, ui->btnWorkingDir, ui->btnBranch, ui->btnReferenceDirs,
                                              ui->btnGuidelines, ui->btnClose};
        for (const QPushButton *button : buttons) {
            if (!button->isHidden()) {
                width += spacing + qMax(button->sizeHint().width(), button->minimumWidth());
            }
        }
        m_fullHeaderWidth = width;
    }
    const QMargins margins = ui->verticalLayout->contentsMargins();
    setHeaderCompact(width() - margins.left() - margins.right() < m_fullHeaderWidth);
}

void SwimlaneWidget::setHeaderCompact(bool compact) {
    for (auto it = m_headerTexts.cbegin(); it != m_headerTexts.cend(); ++it) {
        it.key()->setText(compact ? QString() : it.value());
        // Selama berteks, tombol tidak ikut dijepit layout saat baris header pas-pasan: yang
        // menyempit hanya judul (sampai kTitleMinWidth, lihat updateHeaderMode)
        it.key()->setMinimumWidth(compact ? 0 : qMax(m_headerButtonMinWidth, it.key()->sizeHint().width()));
    }
    // "Close" tidak berikon: teksnya tetap, tombolnya saja yang boleh selebar teks itu
    ui->btnClose->setMinimumWidth(compact ? 0 : m_headerButtonMinWidth);
}

void SwimlaneWidget::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    updateHeaderMode();
}

void SwimlaneWidget::changeEvent(QEvent *event) {
    QWidget::changeEvent(event);
    // Font tombol berasal dari stylesheet; lebar bentuk utuh header ikut berubah
    if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) {
        remeasureHeader();
    }
}

void SwimlaneWidget::showReferencePopup() {
    showListPopup(ListPopup::References);
}

void SwimlaneWidget::showGuidelinePopup() {
    showListPopup(ListPopup::Guidelines);
}

QPushButton *SwimlaneWidget::listPopupAnchor() const {
    return m_listPopupKind == ListPopup::Guidelines ? ui->btnGuidelines : ui->btnReferenceDirs;
}

void SwimlaneWidget::showListPopup(ListPopup kind) {
    QPushButton *anchor = kind == ListPopup::Guidelines ? ui->btnGuidelines : ui->btnReferenceDirs;
    if (!anchor->isVisible()) {
        return;
    }
    if (m_listPopup) {
        m_listPopup->close();
    }

    // Qt::Popup: tertutup sendiri saat klik di luar, seperti popup konfirmasi hapus project
    auto *popup = new QFrame(anchor, Qt::Popup);
    popup->setObjectName(kind == ListPopup::Guidelines ? "guidelinePopup" : "referencePopup");
    popup->setAttribute(Qt::WA_DeleteOnClose);
    popup->setMinimumWidth(320);
    auto *layout = new QVBoxLayout(popup);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(6);
    m_listPopup = popup;
    m_listPopupKind = kind;

    fillListPopup(popup);
    placeListPopup(popup);
    popup->show();
}

void SwimlaneWidget::fillListPopup(QFrame *popup) {
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

    // Satu baris daftar: ikon, nama tebal, keterangan kecil di bawahnya, lalu tombol-tombolnya
    struct Row {
        QString icon;
        QString name;
        QString detail;
        QString detailTip;
        QString openTip;     // kosong = tanpa tombol buka
        QString removeTip;
        std::function<void()> open;
        std::function<void()> remove;
    };
    const bool guidelines = m_listPopupKind == ListPopup::Guidelines;
    const QString prefix = guidelines ? QStringLiteral("guideline") : QStringLiteral("reference");
    QList<Row> rows;
    if (guidelines) {
        const QDir dir(m_guidelineDir);
        for (const QString &name : std::as_const(m_guidelines)) {
            const QString path = dir.filePath(name);
            const QString size = QLocale().formattedDataSize(QFileInfo(path).size());
            const bool pdf = ProjectGuidelines::isPdf(name);
            Row row;
            row.icon = QStringLiteral(":/icons/note.svg");
            row.name = name;
            row.detail = pdf ? QStringLiteral("PDF · %1 · dibaca agent dengan Read").arg(size)
                             : QStringLiteral("%1 · isinya ikut prompt").arg(size);
            row.detailTip = QDir::toNativeSeparators(path);
            row.openTip = QStringLiteral("Buka salinan pedoman ini");
            row.removeTip = QStringLiteral("Hapus dari pedoman project (file aslinya tidak terhapus)");
            row.open = [path]() { QDesktopServices::openUrl(QUrl::fromLocalFile(path)); };
            row.remove = [this, name]() { emit guidelineRemoveRequested(m_projectId, name); };
            rows.append(row);
        }
    } else {
        for (const QString &dir : std::as_const(m_referenceDirs)) {
            const QString name = QDir(dir).dirName();
            Row row;
            row.icon = QStringLiteral(":/icons/folder.svg");
            row.name = name.isEmpty() ? QDir::toNativeSeparators(dir) : name;
            row.detail = QDir::toNativeSeparators(dir);
            row.detailTip = row.detail;
            row.removeTip = QStringLiteral("Hapus dari folder referensi");
            row.remove = [this, dir]() { emit referenceDirectoryRemoveRequested(m_projectId, dir); };
            rows.append(row);
        }
    }

    auto *content = new QWidget(popup);
    content->setObjectName(prefix + "PopupContent");
    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    auto *title = new QLabel(guidelines ? QStringLiteral("PEDOMAN PROJECT") : QStringLiteral("FOLDER REFERENSI"), content);
    title->setObjectName(prefix + "PopupTitle");
    auto *hint = new QLabel(guidelines ? QStringLiteral("Batasan dan guideline (framework, arsitektur, gaya kode) yang "
                                                        "wajib diikuti agent di semua task project ini. File disalin; "
                                                        "tambahkan lagi file bernama sama untuk memperbaruinya.")
                                       : QStringLiteral("Folder di luar folder kerja yang boleh dibaca agent sebagai "
                                                        "acuan. Agent tidak bisa mengubah isinya."),
                            content);
    hint->setObjectName(prefix + "PopupHint");
    hint->setWordWrap(true);
    layout->addWidget(title);
    layout->addWidget(hint);

    if (rows.isEmpty()) {
        auto *empty = new QLabel(guidelines ? QStringLiteral("Belum ada pedoman.") : QStringLiteral("Belum ada folder referensi."),
                                 content);
        empty->setObjectName(prefix + "PopupEmpty");
        layout->addWidget(empty);
    }
    const QString buttonPrefix = guidelines ? QStringLiteral("btnGuideline") : QStringLiteral("btnReference");
    for (const Row &entry : std::as_const(rows)) {
        auto *row = new QWidget(content);
        row->setObjectName(prefix + "Row");
        auto *icon = new QLabel(row);
        icon->setPixmap(Theme::icon(entry.icon).pixmap(QSize(14, 14)));
        auto *label = new QLabel(entry.name, row);
        label->setObjectName(prefix + "RowName");
        auto *detail = new QLabel(row);
        detail->setObjectName(prefix + "RowPath");
        detail->ensurePolished();
        detail->setText(detail->fontMetrics().elidedText(entry.detail, Qt::ElideMiddle, 280));
        detail->setToolTip(entry.detailTip);

        auto *text = new QVBoxLayout();
        text->setSpacing(0);
        text->addWidget(label);
        text->addWidget(detail);
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 2, 0, 2);
        rowLayout->setSpacing(8);
        rowLayout->addWidget(icon);
        rowLayout->addLayout(text, 1);

        if (entry.open) {
            auto *open = new QToolButton(row);
            open->setObjectName(buttonPrefix + "Open");
            open->setIcon(Theme::icon(":/icons/preview.svg"));
            open->setIconSize(QSize(12, 12));
            open->setCursor(Qt::PointingHandCursor);
            open->setToolTip(entry.openTip);
            connect(open, &QToolButton::clicked, this, entry.open);
            rowLayout->addWidget(open);
        }
        auto *remove = new QToolButton(row);
        remove->setObjectName(buttonPrefix + "Remove");
        remove->setIcon(Theme::icon(":/icons/close.svg"));
        remove->setIconSize(QSize(10, 10));
        remove->setCursor(Qt::PointingHandCursor);
        remove->setToolTip(entry.removeTip);
        connect(remove, &QToolButton::clicked, this, entry.remove);
        rowLayout->addWidget(remove);
        layout->addWidget(row);
    }

    auto *add = guidelines
                    ? new QPushButton(Theme::icon(":/icons/plus-navy.svg"), QStringLiteral("Tambah file…"), content)
                    : new QPushButton(Theme::icon(":/icons/folder-add.svg"), QStringLiteral("Tambah folder…"), content);
    add->setObjectName(buttonPrefix + "Add");
    add->setCursor(Qt::PointingHandCursor);
    connect(add, &QPushButton::clicked, this, [this, popup, guidelines]() {
        popup->close();
        // Pemilih file/folder (dialog modal) dibuka setelah klik ini selesai diproses
        QTimer::singleShot(0, this, [this, guidelines]() {
            if (guidelines) {
                emit guidelineAddRequested(m_projectId);
            } else {
                emit referenceDirectoryAddRequested(m_projectId);
            }
        });
    });
    auto *actions = new QHBoxLayout();
    actions->addStretch(1);
    actions->addWidget(add);
    layout->addLayout(actions);

    popupLayout->addWidget(content);
    content->show();
}

void SwimlaneWidget::placeListPopup(QFrame *popup) {
    // Ukuran dari isi yang baru; isi lama yang menunggu deleteLater sudah keluar dari layout
    popup->layout()->activate();
    popup->resize(popup->sizeHint().expandedTo(popup->minimumSize()));

    // Tepi kanan popup sejajar tepi kanan tombol, menggantung di bawahnya; tetap di dalam layar
    QPushButton *anchor = listPopupAnchor();
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

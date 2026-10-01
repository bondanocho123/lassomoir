#include "BranchViewer.h"
#include "DiffView.h"
#include "Theme.h"
#include "WorkspaceDiff.h"

#include <QComboBox>
#include <QCompleter>
#include <QDir>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>

namespace {

enum ItemRole {
    KindRole = Qt::UserRole,   // ItemKind
    CommitRole,                // indeks commit di m_commits
    MetaRole,                  // baris kedua: hash · penulis · waktu
    RefsRole,                  // branch & tag yang menunjuk commit ini
};

enum ItemKind { CommitItem, AllChangesItem };

// Jeda sebelum perubahan dibaca: menelusuri daftar dengan panah tidak menjalankan git tiap baris
constexpr int kSelectionDelayMs = 120;
constexpr int kMaxBodyLines = 8;
constexpr int kMaxBadges = 3;
constexpr int kRowPadding = 6;
constexpr int kLineGap = 2;

// Palet styles.qss
constexpr QRgb kRowBackground = 0xffffff;
constexpr QRgb kSelectedBackground = 0xf2e3d1;
constexpr QRgb kHoverBackground = 0xf8f1e6;
constexpr QRgb kRule = 0xefe6d8;
constexpr QRgb kText = 0x3d3730;
constexpr QRgb kMetaText = 0x8a7f70;
constexpr QRgb kNavy = 0x33517a;
constexpr QRgb kAccent = 0xa9743f;
constexpr QRgb kBranchBadge = 0xe3ebf6;
constexpr QRgb kTagBadge = 0xf2e3d1;
constexpr QRgb kTagText = 0x8a5a2f;

QFont smallerFont(const QFont &base) {
    QFont font = base;
    if (base.pixelSize() > 0) {
        font.setPixelSize(qMax(9, base.pixelSize() - 1));
    } else {
        font.setPointSizeF(base.pointSizeF() * 0.9);
    }
    return font;
}

// Baris daftar commit: lencana branch/tag lalu judul di atas, hash · penulis · waktu di bawah.
// Baris "Semua perubahan" (ada pembanding) bergaris coklat di kiri dan berjudul tebal.
class CommitDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        painter->save();
        const QRect rect = option.rect;
        const bool selected = option.state & QStyle::State_Selected;
        const bool hovered = option.state & QStyle::State_MouseOver;
        painter->fillRect(rect, Theme::fill(selected ? kSelectedBackground : hovered ? kHoverBackground : kRowBackground));
        painter->setPen(Theme::fill(kRule));
        painter->drawLine(rect.bottomLeft(), rect.bottomRight());

        const bool all = index.data(KindRole).toInt() == AllChangesItem;
        if (all) {
            painter->fillRect(QRect(rect.left(), rect.top(), 3, rect.height()), Theme::fill(kAccent));
        }

        const QRect content = rect.adjusted(10, kRowPadding, -10, -kRowPadding);
        QFont titleFont = option.font;
        titleFont.setBold(all);
        const QFont metaFont = smallerFont(option.font);
        const QFontMetrics titleMetrics(titleFont);
        const QFontMetrics metaMetrics(metaFont);
        const int titleHeight = titleMetrics.height();

        // Lencana branch & tag; kelebihannya jadi "+N". Judul tetap kebagian tempat.
        const QStringList refs = index.data(RefsRole).toStringList();
        QStringList badges = refs.mid(0, kMaxBadges);
        if (refs.size() > kMaxBadges) {
            badges.append(QStringLiteral("+%1").arg(refs.size() - kMaxBadges));
        }
        int x = content.left();
        painter->setFont(metaFont);
        painter->setRenderHint(QPainter::Antialiasing, true);
        for (const QString &ref : std::as_const(badges)) {
            const bool tag = ref.startsWith(QLatin1String("tag: "));
            const QString text = tag ? ref.mid(5) : ref;
            const int width = metaMetrics.horizontalAdvance(text) + 10;
            if (x + width > content.right() - 80) {
                break;
            }
            const int height = metaMetrics.height() + 2;
            const QRect badge(x, content.top() + (titleHeight - height) / 2, width, height);
            painter->setPen(Qt::NoPen);
            painter->setBrush(Theme::fill(tag ? kTagBadge : kBranchBadge));
            painter->drawRoundedRect(badge, 3, 3);
            painter->setPen(Theme::text(tag ? kTagText : kNavy));
            painter->drawText(badge, Qt::AlignCenter, text);
            x += width + 4;
        }

        painter->setFont(titleFont);
        painter->setPen(Theme::text(all ? kNavy : kText));
        const int titleWidth = qMax(0, content.right() - x);
        painter->drawText(QRect(x, content.top(), titleWidth, titleHeight), Qt::AlignLeft | Qt::AlignVCenter,
                          titleMetrics.elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideRight, titleWidth));

        painter->setFont(metaFont);
        painter->setPen(Theme::text(kMetaText));
        const QRect meta(content.left(), content.top() + titleHeight + kLineGap, content.width(), metaMetrics.height());
        painter->drawText(meta, Qt::AlignLeft | Qt::AlignVCenter,
                          metaMetrics.elidedText(index.data(MetaRole).toString(), Qt::ElideRight, meta.width()));
        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        Q_UNUSED(index);
        QFont titleFont = option.font;
        titleFont.setBold(true);
        const int height = kRowPadding + QFontMetrics(titleFont).height() + kLineGap
                           + QFontMetrics(smallerFont(option.font)).height() + kRowPadding + 1;
        return QSize(120, height);
    }
};

// Combo branch yang bisa diketik untuk mencari: daftar saran berisi branch yang namanya memuat teks
// itu. Teks yang tidak berakhir jadi pilihan dikembalikan ke branch yang sedang terpilih.
void makeSearchable(QComboBox *combo) {
    combo->setEditable(true);
    combo->setInsertPolicy(QComboBox::NoInsert);
    combo->setMaxVisibleItems(20);
    combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    combo->setMinimumContentsLength(22);
    combo->lineEdit()->setPlaceholderText(QStringLiteral("Cari branch…"));
    QCompleter *completer = combo->completer();
    completer->setCompletionMode(QCompleter::PopupCompletion);
    completer->setFilterMode(Qt::MatchContains);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    QObject::connect(combo->lineEdit(), &QLineEdit::editingFinished, combo, [combo]() {
        const int index = combo->findText(combo->currentText(), Qt::MatchFixedString);
        if (index >= 0 && index != combo->currentIndex()) {
            combo->setCurrentIndex(index);
        } else {
            combo->setEditText(combo->itemText(combo->currentIndex()));
        }
    });
}

QString branchTooltip(const GitBranch &branch) {
    QStringList lines = {branch.name};
    if (branch.current) {
        lines.append(QStringLiteral("Sedang aktif di folder kerja project"));
    }
    lines.append(QStringLiteral("%1 · %2 · %3")
                     .arg(branch.commit.left(7), branch.subject, GitHistory::relativeTime(branch.date)));
    if (!branch.upstream.isEmpty()) {
        lines.append(branch.track.isEmpty() ? QStringLiteral("Mengikuti %1 (sama)").arg(branch.upstream)
                                            : QStringLiteral("Mengikuti %1 (%2)").arg(branch.upstream, branch.track));
    }
    if (!branch.worktree.isEmpty() && !branch.current) {
        lines.append(QStringLiteral("Di-checkout di worktree %1").arg(QDir::toNativeSeparators(branch.worktree)));
    }
    return lines.join(QLatin1Char('\n'));
}

QString fullDate(const QDateTime &date) {
    return date.toLocalTime().toString(QStringLiteral("dd/MM/yyyy HH:mm"));
}

}

BranchViewer::BranchViewer(const QString &projectId, const QString &workingDirectory, QWidget *parent)
    : QDialog(parent), m_projectId(projectId), m_workingDirectory(workingDirectory) {
    setObjectName("branchViewer");
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(QStringLiteral("Branch & commit — %1").arg(projectId));
    // Bisa dimaksimalkan; tanpa minimize, sama seperti penampil diagram (jendela milik jendela utama)
    setWindowFlag(Qt::WindowMaximizeButtonHint, true);

    auto sectionLabel = [this](const QString &text) {
        auto *label = new QLabel(text, this);
        label->setObjectName("branchSectionLabel");
        return label;
    };

    // Baris atas: branch yang dilihat, pembandingnya, tukar keduanya, muat ulang
    m_branch = new QComboBox(this);
    m_branch->setObjectName("branchSelector");
    m_branch->setToolTip(QStringLiteral("Branch yang riwayat commit-nya ditampilkan. Ketik untuk mencari.\n"
                                        "Hanya untuk dilihat: folder kerja tidak di-checkout ke branch ini."));
    makeSearchable(m_branch);
    m_compare = new QComboBox(this);
    m_compare->setObjectName("branchCompareSelector");
    m_compare->setToolTip(QStringLiteral("Bandingkan dengan branch lain, seperti merge request: yang tampil hanya commit "
                                         "dan perubahan yang belum ada di branch pembanding. Ketik untuk mencari."));
    makeSearchable(m_compare);
    m_swap = new QToolButton(this);
    m_swap->setObjectName("btnBranchSwap");
    m_swap->setText(QStringLiteral("⇄"));
    m_swap->setCursor(Qt::PointingHandCursor);
    m_swap->setToolTip(QStringLiteral("Tukar branch dan pembandingnya"));
    m_swap->setEnabled(false);
    m_reload = new QPushButton(QStringLiteral("Muat ulang"), this);
    m_reload->setObjectName("btnBranchReload");
    m_reload->setCursor(Qt::PointingHandCursor);
    m_reload->setToolTip(QStringLiteral("Baca ulang branch dan commit dari repository (F5)"));
    m_reload->setAutoDefault(false);
    m_location = new QLabel(this);
    m_location->setObjectName("branchLocation");
    m_location->setWordWrap(true);
    m_location->setTextInteractionFlags(Qt::TextSelectableByMouse);

    auto *toolbar = new QWidget(this);
    toolbar->setObjectName("branchToolbar");
    toolbar->setAttribute(Qt::WA_StyledBackground, true);
    auto *selectors = new QHBoxLayout();
    selectors->setSpacing(6);
    selectors->addWidget(sectionLabel(QStringLiteral("BRANCH")));
    selectors->addWidget(m_branch);
    selectors->addSpacing(12);
    selectors->addWidget(sectionLabel(QStringLiteral("BANDINGKAN DENGAN")));
    selectors->addWidget(m_compare);
    selectors->addWidget(m_swap);
    selectors->addStretch(1);
    selectors->addWidget(m_reload);
    auto *tools = new QVBoxLayout(toolbar);
    tools->setContentsMargins(12, 10, 12, 8);
    tools->setSpacing(6);
    tools->addLayout(selectors);
    tools->addWidget(m_location);

    // Kiri: daftar commit
    m_listTitle = new QLabel(this);
    m_listTitle->setObjectName("branchListTitle");
    m_listMessage = new QLabel(this);
    m_listMessage->setObjectName("branchListMessage");
    m_listMessage->setWordWrap(true);
    m_listMessage->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_listMessage->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    m_list = new QTreeWidget(this);
    m_list->setObjectName("branchCommitList");
    m_list->setColumnCount(1);
    m_list->setHeaderHidden(true);
    m_list->setRootIsDecorated(false);
    m_list->setIndentation(0);
    m_list->setUniformRowHeights(true);
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setMouseTracking(true);
    m_list->viewport()->setAttribute(Qt::WA_Hover);
    m_list->setItemDelegate(new CommitDelegate(m_list));
    m_more = new QPushButton(this);
    m_more->setObjectName("btnBranchMore");
    m_more->setCursor(Qt::PointingHandCursor);
    m_more->setAutoDefault(false);
    m_more->hide();

    auto *left = new QWidget(this);
    auto *leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(6);
    leftLayout->addWidget(m_listTitle);
    leftLayout->addWidget(m_listMessage);
    leftLayout->addWidget(m_list, 1);
    leftLayout->addWidget(m_more);

    // Kanan: commit / perbandingan yang dipilih, lalu perubahannya
    m_detailTitle = new QLabel(this);
    m_detailTitle->setObjectName("branchDetailTitle");
    m_detailTitle->setWordWrap(true);
    m_detailTitle->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_detailMeta = new QLabel(this);
    m_detailMeta->setObjectName("branchDetailMeta");
    m_detailMeta->setWordWrap(true);
    m_detailMeta->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_detailBody = new QLabel(this);
    m_detailBody->setObjectName("branchDetailBody");
    m_detailBody->setWordWrap(true);
    m_detailBody->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_diffView = new DiffView(this);
    // Commit tidak berubah sesudah dibuat; yang dibaca ulang adalah seluruh jendela (Muat ulang)
    m_diffView->setRefreshVisible(false);
    // Jendela ini lebar: sebelum | sesudah berdampingan lebih mudah dibandingkan
    m_diffView->setSideBySide(true);

    auto *right = new QWidget(this);
    auto *rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(4);
    rightLayout->addWidget(m_detailTitle);
    rightLayout->addWidget(m_detailMeta);
    rightLayout->addWidget(m_detailBody);
    rightLayout->addSpacing(4);
    rightLayout->addWidget(m_diffView, 1);

    auto *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setObjectName("branchSplitter");
    splitter->setHandleWidth(10);
    splitter->setChildrenCollapsible(false);
    splitter->addWidget(left);
    splitter->addWidget(right);
    splitter->setStretchFactor(1, 1);
    left->setMinimumWidth(260);
    right->setMinimumWidth(360);

    auto *body = new QVBoxLayout();
    body->setContentsMargins(12, 10, 12, 12);
    body->addWidget(splitter);
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addWidget(toolbar);
    root->addLayout(body, 1);

    m_selectionTimer = new QTimer(this);
    m_selectionTimer->setSingleShot(true);
    m_selectionTimer->setInterval(kSelectionDelayMs);
    connect(m_selectionTimer, &QTimer::timeout, this, &BranchViewer::showSelection);
    connect(m_list, &QTreeWidget::itemSelectionChanged, m_selectionTimer, qOverload<>(&QTimer::start));
    connect(m_branch, &QComboBox::currentIndexChanged, this, &BranchViewer::loadCommits);
    connect(m_compare, &QComboBox::currentIndexChanged, this, &BranchViewer::loadCommits);
    connect(m_swap, &QToolButton::clicked, this, [this]() {
        const QString source = branch();
        const QString target = compareTarget();
        if (!target.isEmpty()) {
            fillBranchSelectors(target, source);
            loadCommits();
        }
    });
    connect(m_reload, &QPushButton::clicked, this, &BranchViewer::reload);
    connect(new QShortcut(QKeySequence::Refresh, this), &QShortcut::activated, this, &BranchViewer::reload);
    connect(m_more, &QPushButton::clicked, this, &BranchViewer::loadMoreCommits);

    // Seukuran 1200 × 780, tetap di dalam layar, di tengah jendela utama
    const QScreen *screen = parent ? parent->screen() : QGuiApplication::primaryScreen();
    const QRect available = screen ? screen->availableGeometry() : QRect(0, 0, 1280, 800);
    resize(QSize(1200, 780).boundedTo(QSize(int(available.width() * 0.94), int(available.height() * 0.9))));
    splitter->setSizes({400, width() - 400});
    QRect placed(QPoint(0, 0), size());
    placed.moveCenter(parent ? parent->window()->frameGeometry().center() : available.center());
    placed.moveLeft(std::clamp(placed.left(), available.left(), std::max(available.left(), available.right() - placed.width())));
    placed.moveTop(std::clamp(placed.top(), available.top(), std::max(available.top(), available.bottom() - placed.height())));
    move(placed.topLeft());

    m_list->setFocus();
    reload();
}

BranchViewer *BranchViewer::find(const QString &projectId, QWidget *parent) {
    QWidget *owner = parent ? parent->window() : nullptr;
    if (!owner) {
        return nullptr;
    }
    const QList<BranchViewer *> viewers = owner->findChildren<BranchViewer *>(Qt::FindDirectChildrenOnly);
    for (BranchViewer *viewer : viewers) {
        // Jendela yang ditutup sudah tersembunyi, tinggal menunggu deleteLater
        if (viewer->projectId() == projectId && viewer->isVisible()) {
            return viewer;
        }
    }
    return nullptr;
}

BranchViewer *BranchViewer::showProject(const QString &projectId, const QString &workingDirectory, QWidget *parent,
                                        const QString &branch, const QString &compareWith) {
    BranchViewer *viewer = find(projectId, parent);
    if (viewer) {
        if (viewer->workingDirectory() != workingDirectory) {
            viewer->setWorkingDirectory(workingDirectory);
        }
        viewer->setWindowState(viewer->windowState() & ~Qt::WindowMinimized);
    } else {
        viewer = new BranchViewer(projectId, workingDirectory, parent ? parent->window() : nullptr);
        viewer->show();
    }
    if (!branch.isEmpty()) {
        viewer->selectBranch(branch, compareWith);
    }
    viewer->raise();
    viewer->activateWindow();
    return viewer;
}

QString BranchViewer::branch() const {
    return m_branch->currentData().toString();
}

QString BranchViewer::compareTarget() const {
    return m_compare->currentData().toString();
}

template <typename T>
void BranchViewer::runGit(std::function<T()> work, std::function<void(const T &)> apply) {
    // Watcher milik jendela ini: jendela ditutup = jawabannya tidak pernah diterapkan
    auto *watcher = new QFutureWatcher<T>(this);
    connect(watcher, &QFutureWatcher<T>::finished, this, [watcher, apply]() {
        watcher->deleteLater();
        apply(watcher->result());
    });
    watcher->setFuture(QtConcurrent::run(std::move(work)));
}

void BranchViewer::selectBranch(const QString &branch, const QString &compareWith) {
    m_pendingBranch = branch;
    m_pendingCompare = compareWith;
    m_hasPending = true;
    if (m_loadingBranches) {
        return;   // dipilih begitu daftar branch terbaca
    }
    if (m_branches.find(branch) && (compareWith.isEmpty() || m_branches.find(compareWith))) {
        m_hasPending = false;
        fillBranchSelectors(branch, compareWith);
        loadCommits();
        return;
    }
    // Branch-nya belum ada di daftar yang terakhir dibaca (mis. branch task yang baru dibuat)
    reload();
}

void BranchViewer::setWorkingDirectory(const QString &directory) {
    m_workingDirectory = directory;
    m_branches = GitBranchList();
    m_hasPending = false;
    {
        const QSignalBlocker branchBlocker(m_branch);
        const QSignalBlocker compareBlocker(m_compare);
        m_branch->clear();
        m_compare->clear();
    }
    reload();
}

void BranchViewer::reload() {
    const int request = ++m_branchRequest;
    // Daftar commit dan perubahan yang masih dibaca tidak berlaku lagi
    ++m_logRequest;
    ++m_diffRequest;
    m_selectionTimer->stop();
    m_loadingBranches = true;
    m_reload->setEnabled(false);
    m_list->clear();
    m_commits.clear();
    m_more->hide();
    m_listTitle->setText(QStringLiteral("COMMIT"));
    showListMessage(QStringLiteral("Membaca branch…"), false);
    setDetail(QString(), QString(), QString());
    m_diffView->showMessage(QString());

    const QString directory = m_workingDirectory;
    runGit<GitBranchList>([directory]() { return GitHistory::branches(directory); },
                          [this, request](const GitBranchList &list) {
        if (request == m_branchRequest) {
            m_loadingBranches = false;
            m_reload->setEnabled(true);
            applyBranches(list);
        }
    });
}

void BranchViewer::applyBranches(const GitBranchList &list) {
    // Pilihan sebelum dibaca ulang dipertahankan, kecuali ada permintaan pilihan baru
    QString wantedBranch = m_hasPending ? m_pendingBranch : branch();
    QString wantedCompare = m_hasPending ? m_pendingCompare : compareTarget();
    m_hasPending = false;
    m_branches = list;
    emit headRead(m_projectId, list.head);

    QString notice;
    if (list.error.isEmpty() && !list.branches.isEmpty()) {
        if (!wantedBranch.isEmpty() && !list.find(wantedBranch)) {
            notice = QStringLiteral("Branch %1 tidak ada di repository.").arg(wantedBranch);
            wantedBranch.clear();
        }
        if (wantedBranch.isEmpty()) {
            wantedBranch = list.find(list.head.branch) ? list.head.branch : list.branches.first().name;
        }
        if (!wantedCompare.isEmpty() && !list.find(wantedCompare)) {
            notice += QStringLiteral(" Branch pembanding %1 tidak ada di repository.").arg(wantedCompare);
            wantedCompare.clear();
        }
    }
    fillBranchSelectors(wantedBranch, wantedCompare);
    updateLocation();
    if (!notice.isEmpty()) {
        m_location->setText(notice.trimmed() + QLatin1Char('\n') + m_location->text());
    }

    if (!list.error.isEmpty()) {
        showListMessage(list.error, true);
        return;
    }
    if (list.branches.isEmpty()) {
        showListMessage(QStringLiteral("Repository ini belum punya commit, jadi belum ada branch untuk ditampilkan."), false);
        return;
    }
    loadCommits();
}

void BranchViewer::fillBranchSelectors(const QString &branch, const QString &compareWith) {
    const QSignalBlocker branchBlocker(m_branch);
    const QSignalBlocker compareBlocker(m_compare);
    m_branch->clear();
    m_compare->clear();
    m_compare->addItem(QStringLiteral("Tidak dibandingkan"), QString());

    // Branch lokal dulu (yang aktif di folder kerja ditebalkan), lalu branch remote; garis pemisah
    // di antara kelompok dan sesudah "Tidak dibandingkan"
    auto addGroup = [this](bool remote) {
        for (const GitBranch &item : std::as_const(m_branches.branches)) {
            if (item.remote != remote) {
                continue;
            }
            for (QComboBox *combo : {m_branch, m_compare}) {
                combo->addItem(item.name, item.name);
                const int row = combo->count() - 1;
                combo->setItemData(row, branchTooltip(item), Qt::ToolTipRole);
                if (item.current) {
                    QFont bold = combo->font();
                    bold.setBold(true);
                    combo->setItemData(row, bold, Qt::FontRole);
                }
            }
        }
    };
    const auto &all = m_branches.branches;
    const bool locals = std::any_of(all.cbegin(), all.cend(), [](const GitBranch &item) { return !item.remote; });
    const bool remotes = std::any_of(all.cbegin(), all.cend(), [](const GitBranch &item) { return item.remote; });
    if (locals || remotes) {
        m_compare->insertSeparator(m_compare->count());
    }
    addGroup(false);
    if (locals && remotes) {
        m_branch->insertSeparator(m_branch->count());
        m_compare->insertSeparator(m_compare->count());
    }
    addGroup(true);

    const int branchIndex = m_branch->findData(branch);
    m_branch->setCurrentIndex(branchIndex >= 0 ? branchIndex : (m_branch->count() > 0 ? 0 : -1));
    const int compareIndex = compareWith.isEmpty() ? 0 : m_compare->findData(compareWith);
    m_compare->setCurrentIndex(compareIndex >= 0 ? compareIndex : 0);
    m_branch->setEnabled(m_branch->count() > 0);
    m_compare->setEnabled(m_branch->count() > 0);
    m_swap->setEnabled(!compareTarget().isEmpty());
}

const GitBranch *BranchViewer::selectedBranch() const {
    return m_branches.find(branch());
}

const GitBranch *BranchViewer::selectedTarget() const {
    const QString target = compareTarget();
    return target.isEmpty() ? nullptr : m_branches.find(target);
}

void BranchViewer::loadCommits() {
    const GitBranch *source = selectedBranch();
    if (!source) {
        return;
    }
    const GitBranch *target = selectedTarget();
    m_swap->setEnabled(target != nullptr);
    ++m_diffRequest;
    m_selectionTimer->stop();
    m_commits.clear();
    m_comparison = GitComparison();
    m_hasMore = false;
    m_total = -1;
    m_list->clear();
    m_more->hide();
    m_listTitle->setText(QStringLiteral("COMMIT"));
    showListMessage(QStringLiteral("Membaca commit…"), false);
    setDetail(QString(), QString(), QString());
    m_diffView->showMessage(QString());

    const int request = ++m_logRequest;
    const QString directory = m_workingDirectory;
    if (!target) {
        m_revision = source->ref;
        const QString revision = m_revision;
        runGit<GitLog>([directory, revision]() { return GitHistory::log(directory, revision); },
                       [this, request](const GitLog &log) {
            if (request == m_logRequest) {
                applyLog(log, false);
            }
        });
        return;
    }
    const QString targetRef = target->ref;
    const QString sourceRef = source->ref;
    m_revision = targetRef + QStringLiteral("..") + sourceRef;
    runGit<GitComparison>([directory, targetRef, sourceRef]() { return GitHistory::compare(directory, targetRef, sourceRef); },
                          [this, request](const GitComparison &comparison) {
        if (request == m_logRequest) {
            applyComparison(comparison);
        }
    });
}

void BranchViewer::loadMoreCommits() {
    if (!m_hasMore || m_revision.isEmpty()) {
        return;
    }
    m_more->setEnabled(false);
    m_more->setText(QStringLiteral("Membaca commit…"));
    const int request = ++m_logRequest;
    const QString directory = m_workingDirectory;
    const QString revision = m_revision;
    const int skip = int(m_commits.size());
    runGit<GitLog>([directory, revision, skip]() { return GitHistory::log(directory, revision, skip); },
                   [this, request](const GitLog &log) {
        if (request == m_logRequest) {
            applyLog(log, true);
        }
    });
}

void BranchViewer::applyLog(const GitLog &log, bool append) {
    m_more->setEnabled(true);
    if (!log.error.isEmpty()) {
        if (append) {
            // Commit yang sudah tampil tetap ada; tombolnya jadi tempat mencoba lagi
            m_more->setText(QStringLiteral("Gagal membaca — coba lagi"));
            m_more->setToolTip(log.error);
        } else {
            showListMessage(log.error, true);
        }
        return;
    }
    const int from = int(m_commits.size());
    m_commits += log.commits;
    m_hasMore = log.hasMore;
    if (!append) {
        m_total = log.total;
    }
    addCommitItems(from);
    updateListTitle();
    if (m_commits.isEmpty()) {
        showListMessage(m_branches.subdir.isEmpty()
                            ? QStringLiteral("Branch ini belum punya commit.")
                            : QStringLiteral("Belum ada commit di branch ini yang mengubah folder kerja %1.")
                                  .arg(m_branches.subdir),
                        false);
        return;
    }
    showListMessage(QString(), false);
    if (!append) {
        m_list->setCurrentItem(m_list->topLevelItem(0));
    }
}

void BranchViewer::applyComparison(const GitComparison &comparison) {
    if (!comparison.error.isEmpty()) {
        showListMessage(comparison.error, true);
        return;
    }
    m_comparison = comparison;
    m_commits = comparison.commits;
    m_hasMore = comparison.hasMore;
    m_total = comparison.ahead;
    showListMessage(QString(), false);

    // "Semua perubahan" selalu ada, juga tanpa commit di depan: perubahannya kosong, dan
    // keterangannya menjelaskan kenapa
    const GitBranch *target = selectedTarget();
    QStringList meta = {QStringLiteral("%1 commit di depan · %2 di belakang %3")
                            .arg(comparison.ahead)
                            .arg(comparison.behind)
                            .arg(target ? target->name : QString())};
    meta.append(comparison.mergeBase.isEmpty() ? QStringLiteral("tanpa titik cabang bersama")
                                               : QStringLiteral("titik cabang %1").arg(comparison.mergeBase.left(7)));
    auto *all = new QTreeWidgetItem();
    all->setText(0, QStringLiteral("Semua perubahan"));
    all->setData(0, KindRole, AllChangesItem);
    all->setData(0, MetaRole, meta.join(QStringLiteral(" · ")));
    all->setToolTip(0, QStringLiteral("Seluruh perubahan branch ini sejak bercabang dari %1, seperti di merge request")
                           .arg(target ? target->name : QString()));
    m_list->addTopLevelItem(all);
    addCommitItems(0);
    updateListTitle();
    m_list->setCurrentItem(all);
}

void BranchViewer::showListMessage(const QString &text, bool error) {
    m_listMessage->setText(text);
    m_listMessage->setVisible(!text.isEmpty());
    if (m_listMessage->property("error").toBool() != error) {
        m_listMessage->setProperty("error", error);
        m_listMessage->style()->unpolish(m_listMessage);
        m_listMessage->style()->polish(m_listMessage);
    }
    // Pesan (memuat / gagal / kosong) menggantikan daftar yang belum ada isinya
    m_list->setVisible(text.isEmpty() || m_list->topLevelItemCount() > 0);
}

void BranchViewer::addCommitItems(int from) {
    QList<QTreeWidgetItem *> items;
    for (int i = from; i < m_commits.size(); ++i) {
        const GitCommit &commit = m_commits.at(i);
        auto *item = new QTreeWidgetItem();
        item->setText(0, commit.subject.isEmpty() ? QStringLiteral("(tanpa judul)") : commit.subject);
        item->setData(0, KindRole, CommitItem);
        item->setData(0, CommitRole, i);
        QString meta = QStringLiteral("%1 · %2 · %3")
                           .arg(commit.shortHash(), commit.author, GitHistory::relativeTime(commit.date));
        if (commit.parents.size() > 1) {
            meta += QStringLiteral(" · merge");
        }
        item->setData(0, MetaRole, meta);
        item->setData(0, RefsRole, commit.refs);
        QStringList tip = {commit.subject, QStringLiteral("%1 · %2 <%3>").arg(commit.shortHash(), commit.author, commit.email),
                           fullDate(commit.date)};
        if (!commit.refs.isEmpty()) {
            tip.append(commit.refs.join(QStringLiteral(", ")));
        }
        item->setToolTip(0, tip.join(QLatin1Char('\n')));
        items.append(item);
    }
    m_list->addTopLevelItems(items);

    m_more->setVisible(m_hasMore);
    m_more->setToolTip(QString());
    m_more->setText(QStringLiteral("Muat %1 commit lagi").arg(GitHistory::kPageSize));
}

void BranchViewer::updateListTitle() {
    const GitBranch *target = selectedTarget();
    const QString count = m_total >= 0 ? QString::number(m_total)
                                       : QString::number(m_commits.size()) + (m_hasMore ? QStringLiteral("+") : QString());
    m_listTitle->setText(target ? QStringLiteral("COMMIT · %1 belum ada di %2").arg(count, target->name)
                                : QStringLiteral("COMMIT · %1").arg(count));
}

void BranchViewer::showSelection() {
    // Urut baris daftar (terbaru dulu), bukan urutan klik
    QList<QTreeWidgetItem *> selected;
    for (int row = 0; row < m_list->topLevelItemCount(); ++row) {
        if (m_list->topLevelItem(row)->isSelected()) {
            selected.append(m_list->topLevelItem(row));
        }
    }
    const bool withAll = std::any_of(selected.cbegin(), selected.cend(), [](QTreeWidgetItem *item) {
        return item->data(0, KindRole).toInt() == AllChangesItem;
    });
    const GitBranch *source = selectedBranch();
    const GitBranch *target = selectedTarget();

    if (selected.size() == 1 && withAll && source && target) {
        QString meta = QStringLiteral("%1 commit di depan · %2 di belakang %3")
                           .arg(m_comparison.ahead)
                           .arg(m_comparison.behind)
                           .arg(target->name);
        meta += m_comparison.mergeBase.isEmpty()
                    ? QString()
                    : QStringLiteral(" · titik cabang %1").arg(m_comparison.mergeBase.left(7));
        setDetail(QStringLiteral("%1 → %2").arg(source->name, target->name), meta,
                  QStringLiteral("Perubahan yang dibawa %1 sejak bercabang dari %2, seperti yang tampil di merge request. "
                                 "Perubahan yang masuk ke %2 sesudahnya tidak ikut.")
                      .arg(source->name, target->name));
        loadDiff(target->ref, source->ref, true);
        return;
    }

    if (selected.size() == 1 && !withAll) {
        const int index = selected.first()->data(0, CommitRole).toInt();
        if (index < 0 || index >= m_commits.size()) {
            return;
        }
        const GitCommit &commit = m_commits.at(index);
        QStringList meta = {QStringLiteral("%1 · %2 <%3> · %4 (%5)")
                                .arg(commit.hash, commit.author, commit.email, fullDate(commit.date),
                                     GitHistory::relativeTime(commit.date))};
        if (commit.parents.isEmpty()) {
            meta.append(QStringLiteral("Commit pertama repository"));
        } else if (commit.parents.size() == 1) {
            meta.append(QStringLiteral("Induk %1").arg(commit.parents.first().left(7)));
        } else {
            QStringList parents;
            for (const QString &parent : commit.parents) {
                parents.append(parent.left(7));
            }
            meta.append(QStringLiteral("Merge dari %1 · perubahan dibanding induk pertama")
                            .arg(parents.join(QStringLiteral(" + "))));
        }
        if (!commit.refs.isEmpty()) {
            meta.append(commit.refs.join(QStringLiteral(", ")));
        }
        setDetail(commit.subject, meta.join(QLatin1Char('\n')), commit.body);
        loadDiff(commit.parents.value(0), commit.hash, false);
        return;
    }

    if (selected.size() == 2 && !withAll) {
        // Daftar terbaru dulu: baris atas = commit baru, baris bawah = commit lama
        const int newerIndex = selected.at(0)->data(0, CommitRole).toInt();
        const int olderIndex = selected.at(1)->data(0, CommitRole).toInt();
        if (qMax(newerIndex, olderIndex) >= m_commits.size()) {
            return;
        }
        const GitCommit &newer = m_commits.at(newerIndex);
        const GitCommit &older = m_commits.at(olderIndex);
        setDetail(QStringLiteral("%1 → %2").arg(older.shortHash(), newer.shortHash()),
                  QStringLiteral("Dari %1 · %2\nke %3 · %4")
                      .arg(older.shortHash(), older.subject, newer.shortHash(), newer.subject),
                  QString());
        loadDiff(older.hash, newer.hash, false);
        return;
    }

    ++m_diffRequest;
    setDetail(QString(), QString(), QString());
    m_diffView->showMessage(selected.isEmpty()
                                ? QStringLiteral("Pilih commit di daftar untuk melihat perubahannya. Ctrl + klik dua "
                                                 "commit untuk membandingkan keduanya.")
                                : QStringLiteral("Pilih satu commit, atau dua commit (Ctrl + klik) untuk dibandingkan."));
}

void BranchViewer::loadDiff(const QString &from, const QString &to, bool fromMergeBase) {
    const int request = ++m_diffRequest;
    m_diffView->showLoading(QStringLiteral("Membaca perubahan…"));
    const QString directory = m_workingDirectory;
    runGit<WorkspaceDiff>([directory, from, to, fromMergeBase]() {
        return GitDiff::between(directory, from, to, fromMergeBase);
    }, [this, request](const WorkspaceDiff &diff) {
        if (request == m_diffRequest) {
            m_diffView->showDiff(diff);
        }
    });
}

void BranchViewer::setDetail(const QString &title, const QString &meta, const QString &body) {
    m_detailTitle->setText(title);
    m_detailTitle->setVisible(!title.isEmpty());
    m_detailMeta->setText(meta);
    m_detailMeta->setVisible(!meta.isEmpty());
    // Pesan commit yang panjang dipotong; isi lengkapnya di tooltip
    QStringList lines = body.split(QLatin1Char('\n'));
    const bool cut = lines.size() > kMaxBodyLines;
    if (cut) {
        lines = lines.mid(0, kMaxBodyLines);
        lines.append(QStringLiteral("…"));
    }
    m_detailBody->setText(lines.join(QLatin1Char('\n')));
    m_detailBody->setToolTip(cut ? body : QString());
    m_detailBody->setVisible(!body.isEmpty());
}

void BranchViewer::updateLocation() {
    QString text = QStringLiteral("Folder kerja %1").arg(QDir::toNativeSeparators(m_workingDirectory));
    const GitHead &head = m_branches.head;
    if (m_branches.error.isEmpty() && head.repository) {
        text += head.branch.isEmpty() ? QStringLiteral(" · HEAD terlepas di commit %1").arg(head.commit)
                                      : QStringLiteral(" · sedang di branch %1").arg(head.branch);
    }
    if (!m_branches.subdir.isEmpty()) {
        text += QStringLiteral(" · riwayat dan perubahan dibatasi ke folder %1 di repository").arg(m_branches.subdir);
    }
    m_location->setText(text);
}

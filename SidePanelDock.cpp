#include "SidePanelDock.h"
#include "VerticalTabButton.h"

#include <QAbstractAnimation>
#include <QApplication>
#include <QCursor>
#include <QEasingCurve>
#include <QEvent>
#include <QGuiApplication>
#include <QSplitter>
#include <QSplitterHandle>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariantAnimation>

#include <utility>

namespace {

// Lebar rel saat panel tinggal tab; ikut melebar bila font tab lebih tinggi
constexpr int kRailWidth = 26;
// Celah antara tab dan pane di kirinya, dan jarak tab dari tepi atas rel
constexpr int kTabSideMargin = 4;
constexpr int kTabTopMargin = 8;
constexpr int kAnimationMs = 220;
// Selang pemeriksaan kursor selama panel tampil sementara
constexpr int kFlyoutPollMs = 120;

}

SidePanelDock::SidePanelDock(QSplitter *splitter, int paneIndex, QWidget *overlayParent, const QString &name,
                             const QString &caption, QObject *parent)
    : QObject(parent),
      m_splitter(splitter),
      m_index(paneIndex),
      m_panel(splitter->widget(paneIndex)),
      m_overlay(overlayParent),
      m_panelMinWidth(m_panel->minimumWidth()),
      m_panelMaxWidth(m_panel->maximumWidth()) {
    m_rail = new QWidget;
    m_rail->setObjectName(name + QStringLiteral("Rail"));
    m_rail->setMinimumWidth(m_panelMinWidth);
    m_rail->setMaximumWidth(m_panelMaxWidth);

    m_tab = new VerticalTabButton(caption, m_rail);
    m_tab->setObjectName(name + QStringLiteral("Tab"));
    m_tab->setCursor(Qt::PointingHandCursor);
    m_tab->hide();
    m_tab->installEventFilter(this);
    connect(m_tab, &VerticalTabButton::clicked, this, &SidePanelDock::handleTabClicked);

    // Tab menempel tepi kanan rel: selama rel menciut dari lebar panel, tab tidak ikut bergeser
    auto *railLayout = new QVBoxLayout(m_rail);
    railLayout->setContentsMargins(0, kTabTopMargin, 0, 0);
    railLayout->addWidget(m_tab, 0, Qt::AlignRight);
    railLayout->addStretch(1);

    // Rel menggantikan panel di splitter; panelnya pindah ke atas overlayParent dan mengikuti
    // geometri rel lewat eventFilter(), jadi batas lebarnya kini milik rel
    m_splitter->replaceWidget(m_index, m_rail);
    m_panel->setParent(m_overlay);
    m_panel->setMinimumWidth(0);
    m_panel->setMaximumWidth(QWIDGETSIZE_MAX);
    m_panel->show();
    m_rail->installEventFilter(this);
    m_splitter->installEventFilter(this);

    m_flyoutTimer = new QTimer(this);
    m_flyoutTimer->setInterval(kFlyoutPollMs);
    connect(m_flyoutTimer, &QTimer::timeout, this, &SidePanelDock::pollFlyout);

    syncPanelGeometry();
}

void SidePanelDock::setMode(Mode mode) {
    if (mode == m_mode) {
        return;
    }
    closeFlyout();

    // Lebar terpasang diingat untuk pemasangan berikutnya dan untuk lebar tampil sementara.
    // Animasi pasang yang dipotong belum sampai lebar penuh: lebar yang lama tetap berlaku.
    if (m_mode == Mode::Pinned && !isAnimating()) {
        m_pinnedWidth = paneWidth();
    }
    m_mode = mode;
    if (mode != Mode::Hidden) {
        m_shownMode = mode;
    }

    const bool pinned = mode == Mode::Pinned;
    // Rel tab lebarnya tetap, jadi handle di kirinya tidak menawarkan geser
    QSplitterHandle *handle = m_splitter->handle(m_index);
    handle->setEnabled(pinned);
    if (pinned) {
        handle->setCursor(Qt::SplitHCursor);
    } else {
        handle->unsetCursor();
    }
    m_tab->setVisible(mode == Mode::Tab);

    if (pinned) {
        // Panel langsung tampil selebar tujuannya, menempel tepi kanan; rel melebar di bawahnya.
        // Isinya (kartu berteks panjang) jadi tidak ditata ulang tiap frame.
        m_heldWidth = pinnedWidth();
        animateRail(m_heldWidth);
        syncPanelGeometry();
        m_panel->show();
        m_panel->raise();
    } else {
        // Panel hilang seketika; yang menciut tinggal rel kosong tanpa latar
        m_heldWidth = 0;
        m_panel->hide();
        animateRail(mode == Mode::Tab ? tabRailWidth() : 0);
    }

    emit modeChanged(mode);
}

void SidePanelDock::setPanelVisible(bool visible) {
    if (visible != isPanelVisible()) {
        setMode(visible ? m_shownMode : Mode::Hidden);
    }
}

void SidePanelDock::showFlyout() {
    if (m_mode != Mode::Tab || m_flyoutOpen || isAnimating()) {
        return;
    }
    m_flyoutOpen = true;
    m_flyoutMisses = 0;
    m_flyoutDragging = false;
    m_flyoutPressed = QGuiApplication::mouseButtons() != Qt::NoButton;
    m_tab->setActive(true);
    syncPanelGeometry();
    m_panel->show();
    m_panel->raise();
    // Dibuka tanpa kursor di atasnya (lewat keyboard): baru menutup sendiri setelah kursor sempat
    // masuk lalu pergi
    m_flyoutArmed = cursorOverFlyout();
    m_flyoutTimer->start();
}

void SidePanelDock::closeFlyout() {
    if (!m_flyoutOpen) {
        return;
    }
    m_flyoutTimer->stop();
    m_flyoutOpen = false;
    m_tab->setActive(false);
    m_panel->hide();
}

void SidePanelDock::handleTabClicked() {
    if (!m_flyoutOpen) {
        showFlyout();
    } else if (!cursorOverFlyout()) {
        // Dengan mouse kursor selalu di atas tab, dan klik tidak boleh menutup panel yang baru saja
        // dibuka hover-nya. Tanpa kursor di atasnya (keyboard), aktivasi kedua menutup lagi.
        closeFlyout();
    }
}

void SidePanelDock::pollFlyout() {
    if (!m_flyoutOpen) {
        m_flyoutTimer->stop();
        return;
    }
    // Seretan yang berawal di atas panel (seleksi teks log, scrollbar) boleh keluar dari areanya.
    // Yang dihitung hanya tombol mouse yang terlihat mulai ditekan selagi kursor di atas rel +
    // panel: seretan dari luar (kartu kanban) dan tombol yang sudah tertekan sejak sebelum panel
    // tampil tidak ikut menahannya.
    const bool pressed = QGuiApplication::mouseButtons() != Qt::NoButton;
    const bool over = cursorOverFlyout();
    if (!pressed) {
        m_flyoutDragging = false;
    } else if (over && !m_flyoutPressed) {
        m_flyoutDragging = true;
    }
    m_flyoutPressed = pressed;

    if (over) {
        m_flyoutArmed = true;
        m_flyoutMisses = 0;
        return;
    }
    // Menu konteks log juga boleh keluar dari area panel
    if (!m_flyoutArmed || m_flyoutDragging || QApplication::activePopupWidget()) {
        m_flyoutMisses = 0;
        return;
    }
    // Baru ditutup setelah dua pemeriksaan berturut-turut di luar: kursor yang meleset sebentar
    // tidak langsung menghilangkan panel
    if (++m_flyoutMisses >= 2) {
        closeFlyout();
    }
}

bool SidePanelDock::cursorOverFlyout() const {
    // Rel + panel dihitung satu area, supaya celah handle di antara keduanya tidak menutup panel
    const QRect rail(m_rail->mapTo(m_overlay, QPoint(0, 0)), m_rail->size());
    return m_panel->geometry().united(rail).contains(m_overlay->mapFromGlobal(QCursor::pos()));
}

bool SidePanelDock::eventFilter(QObject *watched, QEvent *event) {
    if ((watched == m_rail || watched == m_splitter)
        && (event->type() == QEvent::Resize || event->type() == QEvent::Move)) {
        syncPanelGeometry();
    }
    if (watched == m_tab && event->type() == QEvent::Enter) {
        showFlyout();
    }
    return QObject::eventFilter(watched, event);
}

void SidePanelDock::animateRail(int target) {
    if (m_animation) {
        // Putuskan dulu supaya penutup animasi lama tidak berjalan dengan keadaan yang baru
        m_animation->disconnect(this);
        m_animation->stop();
    }

    // Mulai dari lebar sekarang, supaya animasi yang dipotong melanjutkan geseran. Selama animasi
    // lebar digiring lewat maximumWidth: QSplitter menahan pane di lebar minimumnya, jadi
    // setSizes() saja tidak cukup untuk menciutkannya.
    const int start = m_rail->isHidden() ? 0 : paneWidth();
    m_rail->setMinimumWidth(0);
    m_rail->setMaximumWidth(start);
    setRailShown(true);
    setPaneWidth(start);

    auto *animation = new QVariantAnimation(this);
    animation->setStartValue(start);
    animation->setEndValue(target);
    animation->setDuration(kAnimationMs);
    animation->setEasingCurve(QEasingCurve::OutCubic);
    connect(animation, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
        const int width = value.toInt();
        m_rail->setMaximumWidth(width);
        setPaneWidth(width);
    });
    connect(animation, &QVariantAnimation::finished, this, &SidePanelDock::settle);

    m_animation = animation;
    animation->start(QAbstractAnimation::DeleteWhenStopped);
}

void SidePanelDock::settle() {
    m_heldWidth = 0;
    switch (m_mode) {
    case Mode::Pinned:
        m_rail->setMinimumWidth(m_panelMinWidth);
        m_rail->setMaximumWidth(m_panelMaxWidth);
        break;
    case Mode::Tab:
        m_rail->setFixedWidth(tabRailWidth());
        break;
    case Mode::Hidden:
        // Sembunyikan dulu baru lepaskan batas lebarnya; urutan sebaliknya membuat splitter sempat
        // melebarkan rel sesaat sebelum hilang
        setRailShown(false);
        m_rail->setMaximumWidth(QWIDGETSIZE_MAX);
        break;
    }
    syncPanelGeometry();
}

void SidePanelDock::setRailShown(bool shown) {
    if (shown != m_rail->isHidden()) {
        return;
    }
    // Pane yang muncul atau hilang membawa handle-nya, dan QSplitter membagi selisih lebar itu ke
    // semua pane menurut porsinya. Supaya hanya pane di kiri rel yang berubah, tata letaknya
    // diselesaikan sekarang, lalu lebar pane lainnya dikembalikan.
    const QList<int> before = m_splitter->sizes();
    m_rail->setVisible(shown);
    QCoreApplication::sendPostedEvents(m_splitter, QEvent::LayoutRequest);

    QList<int> sizes = m_splitter->sizes();
    if (m_index < 1 || m_index >= sizes.size() || sizes.size() != before.size()) {
        return;
    }
    int total = 0;
    for (int size : std::as_const(sizes)) {
        total += size;
    }
    // Rel sendiri berlebar 0 di kedua arah: baru akan melebar, atau sudah selesai menciut
    int others = 0;
    for (int i = 0; i < sizes.size(); ++i) {
        if (i == m_index - 1) {
            continue;
        }
        sizes[i] = i == m_index ? 0 : before.at(i);
        others += sizes.at(i);
    }
    sizes[m_index - 1] = qMax(0, total - others);
    m_splitter->setSizes(sizes);
}

bool SidePanelDock::isAnimating() const {
    return m_animation && m_animation->state() == QAbstractAnimation::Running;
}

int SidePanelDock::paneWidth() const {
    return m_splitter->sizes().value(m_index);
}

void SidePanelDock::setPaneWidth(int width) {
    // Lebar pane dibaca tiap kali, bukan disimpan di awal animasi: pane lain bisa sedang berubah
    // lebar sendiri (animasi sidebar, jendela diubah ukurannya)
    QList<int> sizes = m_splitter->sizes();
    if (m_index < 1 || m_index >= sizes.size()) {
        return;
    }
    sizes[m_index - 1] = qMax(0, sizes.at(m_index - 1) + sizes.at(m_index) - width);
    sizes[m_index] = width;
    m_splitter->setSizes(sizes);
}

int SidePanelDock::pinnedWidth() const {
    return qBound(m_panelMinWidth, m_pinnedWidth, m_panelMaxWidth);
}

int SidePanelDock::tabRailWidth() const {
    return qMax(kRailWidth, m_tab->sizeHint().width() + kTabSideMargin);
}

void SidePanelDock::syncPanelGeometry() {
    QRect rect(m_rail->mapTo(m_overlay, QPoint(0, 0)), m_rail->size());
    if (m_flyoutOpen) {
        // Di kiri rel, melewati handle splitter, menimpa pane sebelahnya; tidak melewati tepi
        // kiri splitter bila jendelanya sempit
        const int handle = m_splitter->handleWidth();
        const int room = rect.left() - handle - m_splitter->mapTo(m_overlay, QPoint(0, 0)).x();
        const int width = qMax(0, qMin(pinnedWidth(), room));
        rect.setRect(rect.left() - handle - width, rect.top(), width, rect.height());
    } else if (m_heldWidth > rect.width()) {
        // Sedang dipasang: panel sudah selebar tujuannya, rel masih melebar di bawahnya
        rect.setLeft(rect.right() + 1 - m_heldWidth);
    }
    m_panel->setGeometry(rect);
}

#include "CanvasPreviewDrawer.h"
#include "ElidedLabel.h"
#include "MarkdownView.h"
#include "Theme.h"

#include <QEasingCurve>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLinearGradient>
#include <QPainter>
#include <QPushButton>
#include <QScrollBar>
#include <QShortcut>
#include <QVBoxLayout>
#include <QVariantAnimation>

namespace {

// Lebar drawer: sebagian ruang di kiri panel kanan, dalam batas yang nyaman untuk membaca. Ruang
// yang lebih sempit dari batas bawahnya dipakai seluruhnya, begitu juga bila sisanya tinggal
// secarik yang tidak berguna lagi bagi kanvas atau Pustaka di bawahnya.
constexpr int kWidthPercent = 55;
constexpr int kMinWidth = 420;
constexpr int kMaxWidth = 720;
constexpr int kMinLeftover = 120;
constexpr int kShadowWidth = 8;
constexpr int kAnimationMs = 200;

}

CanvasPreviewDrawer::CanvasPreviewDrawer(MermaidRenderer *renderer, QWidget *parent) : QWidget(parent) {
    setObjectName("canvasPreviewDrawer");

    m_panel = new QFrame(this);
    m_panel->setObjectName("canvasPreviewPanel");
    m_kind = new QLabel(m_panel);
    m_kind->setObjectName("canvasPanelTitle");
    m_title = new ElidedLabel(QString(), m_panel);
    m_title->setObjectName("canvasPreviewTitle");

    m_expand = new QPushButton(m_panel);
    m_expand->setObjectName("btnCanvasPreviewExpand");
    m_expand->setCursor(Qt::PointingHandCursor);
    m_expand->setFixedSize(26, 26);
    m_expand->setIconSize(QSize(14, 14));
    connect(m_expand, &QPushButton::clicked, this, [this]() { setExpanded(!m_expanded); });
    auto *close = new QPushButton(QStringLiteral("✕"), m_panel);
    close->setObjectName("btnCanvasPreviewClose");
    close->setCursor(Qt::PointingHandCursor);
    close->setToolTip(QStringLiteral("Tutup pratinjau (Esc)"));
    close->setFixedSize(26, 26);
    connect(close, &QPushButton::clicked, this, &CanvasPreviewDrawer::closeRequested);

    m_view = new MarkdownView(renderer, m_panel);

    auto *titleBox = new QVBoxLayout();
    titleBox->setSpacing(2);
    titleBox->addWidget(m_kind);
    titleBox->addWidget(m_title);
    auto *header = new QHBoxLayout();
    header->setSpacing(2);
    header->addLayout(titleBox, 1);
    header->addWidget(m_expand, 0, Qt::AlignTop);
    header->addWidget(close, 0, Qt::AlignTop);
    auto *root = new QVBoxLayout(m_panel);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(8);
    root->addLayout(header);
    root->addWidget(m_view, 1);

    auto *escape = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    escape->setContext(Qt::WidgetWithChildrenShortcut);
    connect(escape, &QShortcut::activated, this, &CanvasPreviewDrawer::closeRequested);

    updateExpandButton();
    hide();
}

void CanvasPreviewDrawer::showDocument(const QString &documentId, const QString &kind, const QString &title,
                                       const QString &markdown) {
    m_kind->setText(kind);
    m_title->setFullText(title);
    m_title->setVisible(!title.isEmpty());
    const bool otherDocument = documentId != m_documentId;
    m_documentId = documentId;
    if (otherDocument || m_view->markdown() != markdown) {
        m_view->showMarkdown(markdown);
    }
    if (otherDocument) {
        m_view->verticalScrollBar()->setValue(0);
    }
}

QString CanvasPreviewDrawer::markdown() const {
    return m_view->markdown();
}

void CanvasPreviewDrawer::setBounds(const QRect &area, int dockRight) {
    if (area == m_area && dockRight == m_dockRight) {
        return;
    }
    m_area = area;
    m_dockRight = dockRight;
    // Tertutup: batas baru dipakai saat dibuka. Halaman berubah ukuran selagi drawer bergerak:
    // langsung ke keadaan akhirnya.
    if (m_open || m_animation) {
        finishAnimation();
    }
}

void CanvasPreviewDrawer::slideIn() {
    if (m_open) {
        return;
    }
    m_open = true;
    const QRect target = restingRect(&m_shadow);
    // Dari tepi kanannya dengan lebar nol; animasi tutup yang masih berjalan dilanjutkan dari lebarnya sekarang
    if (isHidden()) {
        setGeometry(target.right() + 1, target.top(), 0, target.height());
        show();
    }
    raise();
    animateTo(target, target.width() - m_shadow);
    emit openChanged(true);
}

void CanvasPreviewDrawer::slideOut() {
    if (!m_open) {
        return;
    }
    m_open = false;
    if (m_expanded) {
        // Dibuka lagi nanti dengan lebar biasanya
        m_expanded = false;
        updateExpandButton();
    }
    const QRect from = geometry();
    animateTo(QRect(from.right() + 1, from.top(), 0, from.height()), qMax(0, from.width() - m_shadow));
    emit openChanged(false);
}

void CanvasPreviewDrawer::setExpanded(bool expanded) {
    if (expanded == m_expanded || !m_open) {
        return;
    }
    m_expanded = expanded;
    updateExpandButton();
    // Panel mengikuti lebar drawer: isinya ditata ulang selagi melebar atau menyempit
    const QRect target = restingRect(&m_shadow);
    layoutPanel();
    animateTo(target, 0);
}

void CanvasPreviewDrawer::updateExpandButton() {
    m_expand->setIcon(Theme::icon(m_expanded ? QStringLiteral(":/icons/collapse.svg") : QStringLiteral(":/icons/expand.svg")));
    m_expand->setToolTip(m_expanded ? QStringLiteral("Kembalikan ukuran") : QStringLiteral("Perluas"));
}

QRect CanvasPreviewDrawer::restingRect(int *shadow) const {
    int pad = 0;
    QRect rect = m_area;
    if (!m_expanded) {
        const int available = qMax(0, m_dockRight - m_area.left());
        int width = qMin(available, qBound(kMinWidth, available * kWidthPercent / 100, kMaxWidth));
        if (available - width < kMinLeftover) {
            width = available;
        }
        // Pita bayangan hanya bila masih ada ruang di kiri panel
        pad = qMin(kShadowWidth, available - width);
        rect = QRect(m_dockRight - width - pad, m_area.top(), width + pad, m_area.height());
    }
    if (shadow) {
        *shadow = pad;
    }
    return rect;
}

void CanvasPreviewDrawer::animateTo(const QRect &target, int slideWidth) {
    if (m_animation) {
        // Putuskan dulu supaya penutup animasi lama tidak berjalan dengan keadaan yang baru
        m_animation->disconnect(this);
        m_animation->stop();
    }
    m_slideWidth = slideWidth;
    layoutPanel();

    auto *animation = new QVariantAnimation(this);
    animation->setStartValue(geometry());
    animation->setEndValue(target);
    animation->setDuration(kAnimationMs);
    animation->setEasingCurve(QEasingCurve::OutCubic);
    connect(animation, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
        setGeometry(value.toRect());
    });
    connect(animation, &QVariantAnimation::finished, this, &CanvasPreviewDrawer::finishAnimation);
    m_animation = animation;
    animation->start(QAbstractAnimation::DeleteWhenStopped);
}

void CanvasPreviewDrawer::finishAnimation() {
    if (m_animation) {
        m_animation->disconnect(this);
        m_animation->stop();
        m_animation = nullptr;
    }
    m_slideWidth = 0;
    if (!m_open) {
        hide();
        return;
    }
    setGeometry(restingRect(&m_shadow));
    layoutPanel();
    update();
}

void CanvasPreviewDrawer::layoutPanel() {
    m_panel->setGeometry(m_shadow, 0, qMax(width() - m_shadow, m_slideWidth), height());
}

void CanvasPreviewDrawer::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    layoutPanel();
}

void CanvasPreviewDrawer::paintEvent(QPaintEvent *event) {
    Q_UNUSED(event);
    if (m_shadow <= 0) {
        return;
    }
    // Bayangan tipis di kiri panel: drawer terlihat melayang di atas kanvas. Tidak sampai ke
    // ujung atas dan bawah, di sana sudut panelnya membulat.
    QPainter painter(this);
    QLinearGradient gradient(0, 0, m_shadow, 0);
    gradient.setColorAt(0, QColor(0x24, 0x20, 0x1b, 0));
    gradient.setColorAt(1, QColor(0x24, 0x20, 0x1b, 44));
    painter.fillRect(QRect(0, 8, m_shadow, height() - 16), gradient);
}

#include "HoverInfoPopup.h"

#include <QEvent>
#include <QGuiApplication>
#include <QLabel>
#include <QScreen>
#include <QVBoxLayout>

namespace {
constexpr int kWidth = 360;         // lebar popup (px), termasuk padding dari styles.qss
constexpr int kGap = 6;             // jarak popup dari jangkarnya
constexpr int kHideDelayMs = 150;   // cukup untuk kursor menyeberang dari judul ke ikon
}

HoverInfoPopup::HoverInfoPopup(QWidget *owner)
    : QWidget(owner, Qt::ToolTip | Qt::FramelessWindowHint | Qt::WindowTransparentForInput) {
    setObjectName("hoverInfoPopup");
    // Sudut membulat butuh jendela transparan. Latar & garis dari styles.qss dilukis di label anak:
    // Qt tidak melukis latar stylesheet pada jendela transparan itu sendiri.
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setFixedWidth(kWidth);

    m_content = new QLabel(this);
    m_content->setObjectName("hoverInfoContent");
    m_content->setTextFormat(Qt::RichText);
    m_content->setWordWrap(true);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_content);

    m_hideTimer.setSingleShot(true);
    m_hideTimer.setInterval(kHideDelayMs);
    connect(&m_hideTimer, &QTimer::timeout, this, &QWidget::hide);
}

void HoverInfoPopup::addAnchor(QWidget *anchor) {
    m_anchors.append(anchor);
    anchor->installEventFilter(this);
}

QString HoverInfoPopup::info() const {
    return m_content->text();
}

void HoverInfoPopup::setInfo(const QString &html) {
    m_content->setText(html);
    if (html.isEmpty()) {
        hide();
    } else if (isVisible()) {
        place();
    }
}

bool HoverInfoPopup::eventFilter(QObject *watched, QEvent *event) {
    switch (event->type()) {
    case QEvent::Enter:
        popup();
        break;
    case QEvent::Leave:
        m_hideTimer.start();
        break;
    case QEvent::MouseButtonPress:
    case QEvent::Hide:
        m_hideTimer.stop();
        hide();
        break;
    default:
        break;
    }
    return QWidget::eventFilter(watched, event);
}

void HoverInfoPopup::popup() {
    m_hideTimer.stop();
    if (info().isEmpty()) {
        return;
    }
    if (!isVisible()) {
        place();
        show();
    }
}

void HoverInfoPopup::place() {
    QWidget *anchor = nullptr;
    for (const QPointer<QWidget> &candidate : std::as_const(m_anchors)) {
        if (candidate) {
            anchor = candidate;
            break;
        }
    }
    if (!anchor) {
        return;
    }

    // Padding styles.qss baru terpasang setelah polish, dan ikut menentukan tinggi
    ensurePolished();
    resize(kWidth, heightForWidth(kWidth));

    QPoint pos = anchor->mapToGlobal(QPoint(0, anchor->height() + kGap));
    QScreen *screen = QGuiApplication::screenAt(pos);
    if (!screen) {
        screen = anchor->screen();
    }
    if (screen) {
        const QRect available = screen->availableGeometry();
        pos.setX(qBound(available.left() + 4, pos.x(), available.right() - width() - 4));
        if (pos.y() + height() > available.bottom()) {
            pos.setY(anchor->mapToGlobal(QPoint(0, 0)).y() - height() - kGap);
        }
    }
    move(pos);
}

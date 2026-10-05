#include "VerticalTabButton.h"

#include <QEvent>
#include <QFontMetrics>
#include <QPainter>
#include <QStyle>

namespace {

// Ruang di kiri-kanan teks (melintang tab) dan di ujung-ujungnya (sepanjang tab)
constexpr int kPaddingAcross = 4;
constexpr int kPaddingAlong = 10;
// Selebar tombol ikon di kepala panel, supaya rel dan kepala panel terlihat satu keluarga
constexpr int kMinimumWidth = 22;

}

VerticalTabButton::VerticalTabButton(const QString &text, QWidget *parent)
    : QAbstractButton(parent) {
    setText(text);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    // Tanpa WA_StyledBackground latar dan sudut membulat dari styles.qss tidak ikut dilukis;
    // WA_Hover supaya latar :hover dilukis ulang saat kursor masuk/keluar
    setAttribute(Qt::WA_StyledBackground, true);
    setAttribute(Qt::WA_Hover, true);
}

void VerticalTabButton::setActive(bool active) {
    if (m_active == active) {
        return;
    }
    m_active = active;
    // Property yang dibaca selector styles.qss berubah setelah stylesheet terpasang: aturannya
    // baru dihitung ulang lewat repolish
    style()->unpolish(this);
    style()->polish(this);
    update();
}

QSize VerticalTabButton::sizeHint() const {
    ensurePolished();
    const QFontMetrics metrics(font());
    return QSize(qMax(kMinimumWidth, metrics.height() + 2 * kPaddingAcross),
                 metrics.horizontalAdvance(text()) + 2 * kPaddingAlong);
}

QSize VerticalTabButton::minimumSizeHint() const {
    return sizeHint();
}

void VerticalTabButton::paintEvent(QPaintEvent *event) {
    Q_UNUSED(event);
    // Latar dan garisnya sudah dilukis dari styles.qss (WA_StyledBackground); di sini tinggal teks
    QPainter painter(this);
    painter.setFont(font());
    painter.setPen(palette().color(isEnabled() ? QPalette::Active : QPalette::Disabled, foregroundRole()));
    // Diputar 90° ke kiri: sumbu teks berjalan dari tepi bawah tombol ke tepi atasnya
    painter.translate(0, height());
    painter.rotate(-90);
    painter.drawText(QRect(0, 0, height(), width()), Qt::AlignCenter, text());
}

void VerticalTabButton::changeEvent(QEvent *event) {
    QAbstractButton::changeEvent(event);
    if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) {
        updateGeometry();
        update();
    }
}

#include "ElidedLabel.h"

#include <QEvent>
#include <QResizeEvent>

ElidedLabel::ElidedLabel(const QString &text, QWidget *parent, Qt::TextElideMode mode)
    : QLabel(parent), m_fullText(text), m_mode(mode) {
    // Ignored: layout boleh mempersempit label sampai 0, sisa lebar dipakai widget di sebelahnya
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    setMinimumWidth(0);
    refresh();
}

void ElidedLabel::setFullText(const QString &text) {
    m_fullText = text;
    refresh();
}

QSize ElidedLabel::minimumSizeHint() const {
    return QSize(0, QLabel::minimumSizeHint().height());
}

void ElidedLabel::resizeEvent(QResizeEvent *event) {
    QLabel::resizeEvent(event);
    refresh();
}

void ElidedLabel::changeEvent(QEvent *event) {
    QLabel::changeEvent(event);
    // Font dari stylesheet baru terpasang saat polish; ukuran teks ikut berubah
    if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) {
        refresh();
    }
}

void ElidedLabel::refresh() {
    const int available = contentsRect().width();
    const QString shown = available > 0 ? fontMetrics().elidedText(m_fullText, m_mode, available) : m_fullText;
    if (text() != shown) {
        setText(shown);
    }
    setToolTip(shown == m_fullText ? QString() : m_fullText);
}

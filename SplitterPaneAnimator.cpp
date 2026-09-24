#include "SplitterPaneAnimator.h"

#include <QEasingCurve>
#include <QSplitter>
#include <QVariantAnimation>

SplitterPaneAnimator::SplitterPaneAnimator(QSplitter *splitter, int paneIndex, QObject *parent)
    : QObject(parent),
      m_splitter(splitter),
      m_index(paneIndex),
      m_minimumWidth(splitter->widget(paneIndex)->minimumWidth()) {
}

void SplitterPaneAnimator::open(int defaultWidth) {
    QWidget *pane = m_splitter->widget(m_index);
    if (m_open && pane->isVisible() && !m_animation) {
        return;
    }

    // Lanjutkan dari lebar sekarang bila animasi tutup sedang berjalan
    const int start = pane->isVisible() ? m_splitter->sizes().value(m_index) : 0;
    const int target = m_lastWidth > 0 ? m_lastWidth : defaultWidth;

    pane->setMinimumWidth(0);
    pane->setMaximumWidth(start);
    pane->show();
    setPaneWidth(start);

    m_open = true;
    animate(start, target);
}

void SplitterPaneAnimator::close() {
    QWidget *pane = m_splitter->widget(m_index);
    if (!m_open || !pane->isVisible()) {
        m_open = false;
        return;
    }

    const int start = m_splitter->sizes().value(m_index);
    if (m_expanded) {
        // Pane lain muncul lagi selagi drawer menciut; yang diingat lebar sebelum expand
        m_lastWidth = m_restoreWidth;
        m_expanded = false;
        squeezeOtherPane(start);
        emit expandedChanged(false);
    } else if (!m_animation) {
        // Ingat lebar hasil geseran pengguna untuk pembukaan berikutnya
        m_lastWidth = start;
    }
    pane->setMinimumWidth(0);

    m_open = false;
    animate(start, 0);
}

void SplitterPaneAnimator::setExpanded(bool expanded) {
    QWidget *pane = m_splitter->widget(m_index);
    if (expanded == m_expanded || !m_open || !pane->isVisible()) {
        return;
    }

    const int start = m_splitter->sizes().value(m_index);
    if (expanded) {
        // Tombol bisa ditekan selagi drawer masih membuka: lebar tujuannya yang diingat
        m_restoreWidth = m_animation ? m_animationTarget : start;
    }
    m_expanded = expanded;
    squeezeOtherPane(start);
    animate(start, expanded ? availableWidth() : m_restoreWidth);
    emit expandedChanged(expanded);
}

void SplitterPaneAnimator::animate(int from, int to) {
    if (m_animation) {
        // Putuskan dulu supaya penutup animasi lama tidak berjalan dengan keadaan yang baru
        m_animation->disconnect(this);
        m_animation->stop();
    }

    auto *animation = new QVariantAnimation(this);
    animation->setStartValue(from);
    animation->setEndValue(to);
    animation->setDuration(200);
    animation->setEasingCurve(QEasingCurve::OutCubic);
    m_animationTarget = to;

    connect(animation, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
        const int width = value.toInt();
        m_splitter->widget(m_index)->setMaximumWidth(width);
        setPaneWidth(width);
    });
    connect(animation, &QVariantAnimation::finished, this, [this]() {
        QWidget *pane = m_splitter->widget(m_index);
        // Sembunyikan dulu baru pulihkan batasan lebar; urutan sebaliknya membuat
        // splitter sempat melebarkan pane sesaat sebelum hilang
        if (!m_open) {
            pane->hide();
        }
        if (m_squeezeOther) {
            QWidget *other = m_splitter->widget(m_index == 0 ? 1 : 0);
            if (m_expanded) {
                other->hide();
            }
            other->setMaximumWidth(QWIDGETSIZE_MAX);
            m_squeezeOther = false;
        }
        pane->setMaximumWidth(QWIDGETSIZE_MAX);
        pane->setMinimumWidth(m_minimumWidth);
    });

    m_animation = animation;
    animation->start(QAbstractAnimation::DeleteWhenStopped);
}

void SplitterPaneAnimator::setPaneWidth(int width) {
    if (m_squeezeOther) {
        squeezeOtherPane(width);
    }

    QList<int> sizes = m_splitter->sizes();
    if (m_index >= sizes.size()) {
        return;
    }
    int total = 0;
    for (int size : std::as_const(sizes)) {
        total += size;
    }
    // Selisih lebar diambil dari / dikembalikan ke pane pertama selain pane ini
    const int other = m_index == 0 ? 1 : 0;
    sizes[m_index] = width;
    if (other < sizes.size()) {
        sizes[other] = qMax(0, total - width);
    }
    m_splitter->setSizes(sizes);
}

void SplitterPaneAnimator::squeezeOtherPane(int paneWidth) {
    QWidget *other = m_splitter->widget(m_index == 0 ? 1 : 0);
    if (!other) {
        return;
    }
    other->setMaximumWidth(qMax(0, availableWidth() - paneWidth));
    other->show();
    m_squeezeOther = true;
}

int SplitterPaneAnimator::availableWidth() const {
    return m_splitter->contentsRect().width() - m_splitter->handleWidth();
}

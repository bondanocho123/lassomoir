#include "RunPulse.h"
#include "Theme.h"

#include <QColor>
#include <QPainter>
#include <QPen>
#include <QRectF>
#include <QWidget>
#include <QtMath>

namespace {

constexpr int kPeriodMs = 1200;   // satu kali terang-redup
constexpr int kFrameMs = 40;      // ~25 fps: cukup halus untuk kedip pelan, ringan walau banyak kartu berjalan
// Navy, sama dengan garis kartu berstatus "running" di styles.qss
constexpr QRgb kColor = 0xff33517a;

}

RunPulse::RunPulse(QWidget *target)
    : QObject(target),
      m_target(target) {
    m_timer.setInterval(kFrameMs);
    connect(&m_timer, &QTimer::timeout, m_target, qOverload<>(&QWidget::update));
}

void RunPulse::setActive(bool active) {
    if (active == isActive()) {
        return;
    }
    if (active) {
        m_clock.start();
        m_timer.start();
    } else {
        m_timer.stop();
    }
    // Saat berhenti, sisa kedip terakhir ikut hilang dari kartu
    m_target->update();
}

qreal RunPulse::level() const {
    if (!isActive()) {
        return 0.0;
    }
    const qreal phase = qreal(m_clock.elapsed() % kPeriodMs) / kPeriodMs;
    // Mulai dari redup, paling terang di tengah siklus
    return 0.5 - 0.5 * qCos(2.0 * M_PI * phase);
}

void RunPulse::paint(QPainter &painter, const QRectF &rect, qreal radius) const {
    if (!isActive()) {
        return;
    }
    const qreal level = this->level();
    // Di mode gelap garis navy menjadi periwinkle supaya kedipnya tetap terlihat
    QColor fill = Theme::text(kColor);
    fill.setAlphaF(0.16 * level);
    QColor line = Theme::text(kColor);
    line.setAlphaF(0.2 + 0.8 * level);

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(line, 2.0));
    painter.setBrush(fill);
    // Garis 2px menutupi garis 1px styles.qss di tepi kartu
    painter.drawRoundedRect(rect.adjusted(1.0, 1.0, -1.0, -1.0), radius - 1.0, radius - 1.0);
    painter.restore();
}

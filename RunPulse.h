#ifndef RUNPULSE_H
#define RUNPULSE_H

#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>

class QPainter;
class QRectF;
class QWidget;

// Kedip "agent sedang bekerja" untuk kartu task (kanban dan daftar task di konsol): latar kartu
// disaput navy dan garisnya menebal, lalu memudar lagi, terus-menerus selama run berjalan.
// Kartu memanggil paint() dari paintEvent-nya; RunPulse yang menjadwalkan repaint selama aktif.
class RunPulse : public QObject {
    Q_OBJECT

public:
    explicit RunPulse(QWidget *target);

    void setActive(bool active);
    bool isActive() const { return m_timer.isActive(); }

    // Terang kedip saat ini: 0 (redup) .. 1 (paling terang); selalu 0 bila tidak aktif
    qreal level() const;

    // Saputan latar + garis kedip di dalam rect (sudut membulat radius). Dipanggil dari paintEvent
    // kartu: jatuhnya sesudah latar styles.qss dan sebelum anak widget, jadi teks tetap di atasnya.
    void paint(QPainter &painter, const QRectF &rect, qreal radius) const;

private:
    QWidget *m_target;
    QTimer m_timer;          // jadwal repaint selama aktif
    QElapsedTimer m_clock;   // fase kedip dihitung dari waktu, bukan dari jumlah frame
};

#endif // RUNPULSE_H

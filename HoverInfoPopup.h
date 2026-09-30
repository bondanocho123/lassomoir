#ifndef HOVERINFOPOPUP_H
#define HOVERINFOPOPUP_H

#pragma once

#include <QList>
#include <QPointer>
#include <QTimer>
#include <QWidget>

class QLabel;

// Penjelasan panjang (teks kaya) yang muncul seketika saat kursor berada di atas salah satu
// jangkarnya (mis. judul stage dan ikon info di sebelahnya), dan hilang begitu kursor meninggalkan
// semuanya. Pengganti QToolTip: QToolTip baru muncul setelah jeda, menutup-membuka lagi (berkedip)
// saat kursor pindah antar widget bertooltip sama, dan warnanya mengikuti sistem, bukan skin
// aplikasi. Gayanya di styles.qss (QLabel#hoverInfoContent).
class HoverInfoPopup : public QWidget {
    Q_OBJECT

public:
    // Jendela popup milik owner: ikut terhapus bersamanya
    explicit HoverInfoPopup(QWidget *owner);

    // Widget yang memunculkan popup saat di-hover. Posisi popup selalu di bawah jangkar pertama,
    // jadi berpindah antar jangkar tidak menggesernya.
    void addAnchor(QWidget *anchor);

    // Kosong = popup tidak pernah muncul
    void setInfo(const QString &html);
    QString info() const;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    // Ukur tinggi untuk lebar tetap, lalu gantung di bawah jangkar pertama; tetap di dalam layar
    void place();
    void popup();

    QLabel *m_content;   // kartu bergaya skin; jendela ini sendiri transparan
    QList<QPointer<QWidget>> m_anchors;
    QTimer m_hideTimer;   // jeda singkat saat kursor keluar, supaya pindah ke jangkar lain tidak berkedip
};

#endif // HOVERINFOPOPUP_H

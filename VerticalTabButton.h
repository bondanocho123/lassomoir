#ifndef VERTICALTABBUTTON_H
#define VERTICALTABBUTTON_H

#pragma once

#include <QAbstractButton>

// Tombol tab tegak untuk rel sempit di sisi jendela: caption-nya diputar 90° ke kiri, jadi dibaca
// dari bawah ke atas. Latar, garis, warna, dan font-nya dari styles.qss; property "active"
// menandai tab yang panelnya sedang tampil (selector [active="true"]).
class VerticalTabButton : public QAbstractButton {
    Q_OBJECT
    Q_PROPERTY(bool active READ isActive WRITE setActive)

public:
    explicit VerticalTabButton(const QString &text, QWidget *parent = nullptr);

    bool isActive() const { return m_active; }
    void setActive(bool active);

    // Lebar dan tinggi tertukar terhadap teksnya: teks berjalan tegak
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;
    // Font dari stylesheet baru terpasang saat polish, dan bisa berganti lewat File > Preferences
    void changeEvent(QEvent *event) override;

private:
    bool m_active = false;
};

#endif // VERTICALTABBUTTON_H

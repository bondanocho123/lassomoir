#ifndef SPLITTERPANEANIMATOR_H
#define SPLITTERPANEANIMATOR_H

#pragma once

#include <QObject>
#include <QPointer>

class QSplitter;
class QVariantAnimation;

// Animasi buka/tutup satu pane QSplitter (mis. drawer hasil agent).
// Lebar digiring lewat maximumWidth selama animasi: QSplitter menahan pane di
// minimumSizeHint layout-nya, jadi setSizes() saja tidak cukup untuk menciutkannya mulus.
class SplitterPaneAnimator : public QObject {
    Q_OBJECT

public:
    SplitterPaneAnimator(QSplitter *splitter, int paneIndex, QObject *parent = nullptr);

    // Buka ke lebar terakhir (bila pernah digeser pengguna) atau defaultWidth
    void open(int defaultWidth);
    void close();
    bool isOpen() const { return m_open; }

    // Lebarkan pane sampai memenuhi splitter (pane lain disembunyikan), atau kembalikan ke
    // lebar sebelumnya. Diabaikan bila pane tidak sedang terbuka; close() otomatis mengembalikan.
    void setExpanded(bool expanded);
    bool isExpanded() const { return m_expanded; }

signals:
    void expandedChanged(bool expanded);

private:
    void animate(int from, int to);
    void setPaneWidth(int width);
    void squeezeOtherPane(int paneWidth);   // pane lain ikut digiring lewat maximumWidth
    int availableWidth() const;             // lebar splitter di luar handle antar dua pane

    QSplitter *m_splitter;
    int m_index;
    int m_minimumWidth;                 // minimumWidth asli pane, dipulihkan setelah animasi
    int m_lastWidth = 0;
    int m_restoreWidth = 0;             // lebar sebelum di-expand
    int m_animationTarget = 0;
    bool m_open = false;
    bool m_expanded = false;
    bool m_squeezeOther = false;
    QPointer<QVariantAnimation> m_animation;
};

#endif // SPLITTERPANEANIMATOR_H

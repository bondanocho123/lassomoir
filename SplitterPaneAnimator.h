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

private:
    void animate(int from, int to);
    void setPaneWidth(int width);

    QSplitter *m_splitter;
    int m_index;
    int m_minimumWidth;                 // minimumWidth asli pane, dipulihkan setelah animasi
    int m_lastWidth = 0;
    bool m_open = false;
    QPointer<QVariantAnimation> m_animation;
};

#endif // SPLITTERPANEANIMATOR_H

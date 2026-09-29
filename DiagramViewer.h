#ifndef DIAGRAMVIEWER_H
#define DIAGRAMVIEWER_H

#pragma once

#include <QDialog>
#include <QImage>
#include <QString>

class DiagramCanvas;
class QLabel;
class QPushButton;

// Jendela untuk membaca satu diagram Mermaid: zoom dengan Ctrl + scroll atau pinch touchpad (titik
// di bawah kursor tetap di tempat), tombol −/+ · Paskan · 100%, geser dengan menyeret, klik ganda
// untuk beralih Paskan ↔ 100%. Non-modal, jadi bisa dibuka di samping drawer; satu jendela per
// diagram. 100% = ukuran asli diagram (devicePixelRatio gambar dari renderer).
class DiagramViewer : public QDialog {
    Q_OBJECT

public:
    DiagramViewer(const QString &key, const QImage &image, const QString &title, QWidget *parent = nullptr);

    // Munculkan lagi penampil diagram `key` yang sudah terbuka di jendela `parent`, atau buka baru
    static DiagramViewer *showDiagram(const QString &key, const QImage &image, const QString &title, QWidget *parent);
    // Judul dari jenis diagram di baris pertama kodenya ("classDiagram" → "Diagram kelas")
    static QString titleFor(const QString &code);

    QString key() const { return m_key; }
    qreal zoom() const;          // 1.0 = 100%
    qreal fitZoom() const;       // zoom yang memuat seluruh diagram di jendela
    bool fitsToWindow() const;   // mode Paskan: zoom ikut berubah saat jendela diubah ukurannya

    // Tombol dan pintasan keyboard; zoom berpusat di tengah jendela
    void zoomIn();
    void zoomOut();
    void fitToWindow();
    void showActualSize();

private:
    // PNG di folder sementara → penampil gambar bawaan sistem (simpan, cetak, salin)
    void openExternally();
    void updateControls();

    QString m_key;
    QImage m_image;
    DiagramCanvas *m_canvas;
    QPushButton *m_zoomOut;
    QLabel *m_zoomLevel;
    QPushButton *m_zoomIn;
    QPushButton *m_fit;
    QPushButton *m_actualSize;
};

#endif // DIAGRAMVIEWER_H

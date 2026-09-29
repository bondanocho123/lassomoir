#ifndef EDGEMERMAIDRENDERER_H
#define EDGEMERMAIDRENDERER_H

#pragma once

#include "MermaidRenderer.h"

#include <QByteArray>
#include <QList>
#include <QPair>
#include <QSize>
#include <QSizeF>
#include <QTemporaryDir>
#include <QTimer>

class QProcess;

// Menggambar diagram dengan browser headless (Edge bawaan Windows, atau Chrome): halaman lokal
// berisi mermaid.min.js (dibundel di :/vendor) di-screenshot dengan latar transparan, lalu tepinya
// dipotong. Diagram yang lebih besar dari kanvas pertama diperkecil agar muat, lalu dirender sekali
// lagi seukuran aslinya supaya tetap tajam saat di-zoom (DiagramViewer). Hasil disimpan di cache
// per key beserta kerapatan pikselnya: ukuran logis gambar = ukuran asli diagram.
class EdgeMermaidRenderer final : public MermaidRenderer {
    Q_OBJECT

public:
    // Satu proses browser: kanvas dalam piksel CSS (= --window-size) dan skala perangkat
    struct Pass {
        QSize canvas;
        qreal scale = 0.0;
    };

    // Laporan halaman render di #lassomoir-render, dibaca dari --dump-dom
    struct Report {
        bool ok = false;
        QString error;      // pesan Mermaid bila kode tidak bisa digambar
        QSizeF natural;     // ukuran asli diagram (piksel CSS)
        qreal fit = 1.0;    // skala diagram di kanvas; < 1 = diperkecil agar muat
    };

    // cacheDir kosong = <CacheLocation>/mermaid
    explicit EdgeMermaidRenderer(QString browser = findBrowser(), QString cacheDir = QString(),
                                 QObject *parent = nullptr);
    ~EdgeMermaidRenderer() override;

    // msedge.exe (Program Files x86 / Program Files), lalu chrome.exe; kosong bila tidak ada
    static QString findBrowser();

    bool isAvailable(QString *reason) const override;
    void render(const QString &code) override;

    // Potong tepi transparan hasil screenshot (sisakan margin kecil); gambar kosong bila tidak ada isi
    static QImage trimTransparent(const QImage &image, int margin = 4);

    // Halaman HTML render untuk satu kanvas; kode diagram di-escape sehingga tidak bisa menyisipkan
    // tag/script
    static QString pageHtml(const QString &code, const QSize &canvas);
    static Report readReport(const QByteArray &dom);
    // Lintasan pertama semua diagram: kanvas 1400 × 2400 pada skala 2
    static Pass firstPass();
    // Lintasan kedua untuk diagram seukuran `natural`: kanvas pas seukuran diagram, skala dibatasi
    // jumlah piksel dan sisi screenshot terbesar. scale 0 = terlalu besar untuk dirender ulang.
    static Pass detailPass(const QSizeF &natural);

private:
    struct Job {
        QString key;
        QString code;
        Pass pass;
        QImage fallback;   // hasil lintasan pertama, dipakai bila lintasan kedua gagal
    };

    void startNext();                        // satu proses browser dalam satu waktu
    void launch();                           // jalankan browser untuk m_current.pass
    void onFinished();                       // laporan + PNG -> potong -> lintasan kedua, atau selesai
    void finishCurrent(QImage image, const QString &error);   // simpan cache -> emit -> berikutnya
    void releaseProcess();
    QString cachePath(const QString &key) const;
    bool ensureRuntimeFiles();               // salin mermaid.min.js dari resource ke folder kerja

    QString m_browser;
    QString m_cacheDir;
    QTemporaryDir m_workDir;                 // halaman render + profil browser sementara
    QList<QPair<QString, QString>> m_queue;  // key, kode
    QProcess *m_process = nullptr;
    Job m_current;
    QTimer m_timeout;                        // batas waktu satu lintasan
};

#endif // EDGEMERMAIDRENDERER_H

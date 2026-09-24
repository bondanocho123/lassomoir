#ifndef EDGEMERMAIDRENDERER_H
#define EDGEMERMAIDRENDERER_H

#pragma once

#include "MermaidRenderer.h"

#include <QList>
#include <QPair>
#include <QTemporaryDir>
#include <QTimer>

class QProcess;

// Menggambar diagram dengan browser headless (Edge bawaan Windows, atau Chrome):
// halaman lokal berisi mermaid.min.js (dibundel di :/vendor) di-screenshot 2x dengan
// latar transparan, lalu tepinya dipotong. Hasil disimpan di cache per key.
class EdgeMermaidRenderer final : public MermaidRenderer {
    Q_OBJECT

public:
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

    // Halaman HTML render; kode diagram di-escape sehingga tidak bisa menyisipkan tag/script
    static QString pageHtml(const QString &code);

private:
    void startNext();                        // satu proses browser dalam satu waktu
    void onFinished();                       // baca PNG -> potong -> simpan cache -> emit
    void finishCurrent(const QImage &image, const QString &error);
    QString cachePath(const QString &key) const;
    bool ensureRuntimeFiles();               // salin mermaid.min.js dari resource ke folder kerja

    QString m_browser;
    QString m_cacheDir;
    QTemporaryDir m_workDir;                 // halaman render + profil browser sementara
    QList<QPair<QString, QString>> m_queue;  // key, kode
    QProcess *m_process = nullptr;
    QString m_currentKey;
    QTimer m_timeout;                        // batas waktu satu diagram
};

#endif // EDGEMERMAIDRENDERER_H

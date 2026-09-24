#ifndef MARKDOWNVIEW_H
#define MARKDOWNVIEW_H

#pragma once

#include <QHash>
#include <QImage>
#include <QSet>
#include <QString>
#include <QTextBrowser>
#include <QTimer>

class MermaidRenderer;

// Penampil Markdown (dialek GitHub: tabel, daftar, kode) yang menggambar blok ```mermaid
// sebagai diagram lewat MermaidRenderer. Tanpa renderer, blok Mermaid tampil sebagai kode.
class MarkdownView : public QTextBrowser {
    Q_OBJECT

public:
    explicit MarkdownView(MermaidRenderer *renderer, QWidget *parent = nullptr);

    void showMarkdown(const QString &markdown);
    QString markdown() const { return m_markdown; }

protected:
    // mermaid://<key> -> gambar hasil render, diperkecil agar muat lebar panel
    QVariant loadResource(int type, const QUrl &name) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    // Blok Mermaid -> gambar / "Merender diagram…" / kode + pesan gagal; posisi gulir dijaga
    void rebuild();
    // Lebar logis maksimum gambar di dalam panel
    int availableImageWidth() const;
    void onRendered(const QString &key, const QImage &image);
    void onFailed(const QString &key, const QString &error);
    // Diagram -> buka PNG ukuran penuh di viewer bawaan; http(s)/file -> aplikasi default
    void onAnchorClicked(const QUrl &url);

    MermaidRenderer *m_renderer;
    QString m_markdown;
    QHash<QString, QImage> m_images;   // key -> diagram
    QHash<QString, QString> m_errors;  // key -> alasan gagal
    QSet<QString> m_requested;         // key yang sudah diminta ke renderer
    QTimer m_resizeDebounce;           // gambar disesuaikan ulang setelah lebar berhenti berubah
    int m_lastWidth = 0;
};

#endif // MARKDOWNVIEW_H

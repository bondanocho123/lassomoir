#ifndef MERMAIDRENDERER_H
#define MERMAIDRENDERER_H

#pragma once

#include <QImage>
#include <QObject>
#include <QString>

// Kontrak "kode Mermaid -> gambar". MarkdownView hanya mengenal interface ini,
// jadi cara menggambarnya (Edge headless, atau palsu di test) bisa diganti.
class MermaidRenderer : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;
    ~MermaidRenderer() override = default;

    // Kunci stabil untuk satu diagram: dipakai untuk cache dan URL gambar (mermaid://<key>).
    // Berubah bila tema/versi renderer berubah, supaya cache lama tidak terpakai.
    static QString keyFor(const QString &code);

    // Renderer siap dipakai? Isi *reason bila tidak
    virtual bool isAvailable(QString *reason) const = 0;

    // Asinkron: hasil selalu lewat sinyal, tidak pernah langsung di dalam render()
    virtual void render(const QString &code) = 0;

signals:
    void rendered(const QString &key, const QImage &image);
    void failed(const QString &key, const QString &error);
};

#endif // MERMAIDRENDERER_H

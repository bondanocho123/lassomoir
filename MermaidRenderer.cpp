#include "MermaidRenderer.h"

#include <QCryptographicHash>

namespace {
// Naikkan bila tema diagram, cara render, atau versi mermaid.min.js berubah
// (3: diagram besar dirender seukuran aslinya, kerapatan piksel disimpan di PNG)
constexpr char kRendererVersion[] = "lassomoir-mermaid-3|mermaid-11.17.2|";
}

QString MermaidRenderer::keyFor(const QString &code) {
    const QByteArray data = QByteArray(kRendererVersion) + code.trimmed().toUtf8();
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha1).toHex());
}

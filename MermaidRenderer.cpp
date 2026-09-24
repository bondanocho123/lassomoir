#include "MermaidRenderer.h"

#include <QCryptographicHash>

namespace {
// Naikkan bila tema diagram atau versi mermaid.min.js berubah
constexpr char kRendererVersion[] = "lassomoir-mermaid-2|mermaid-11.17.2|";
}

QString MermaidRenderer::keyFor(const QString &code) {
    const QByteArray data = QByteArray(kRendererVersion) + code.trimmed().toUtf8();
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha1).toHex());
}

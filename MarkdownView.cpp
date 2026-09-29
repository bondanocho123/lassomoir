#include "MarkdownView.h"
#include "DiagramViewer.h"
#include "MermaidRenderer.h"

#include <QDesktopServices>
#include <QHelpEvent>
#include <QList>
#include <QPair>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QToolTip>
#include <QUrl>

namespace {
constexpr char kScheme[] = "mermaid";
}

MarkdownView::MarkdownView(MermaidRenderer *renderer, QWidget *parent)
    : QTextBrowser(parent), m_renderer(renderer) {
    setObjectName("markdownView");
    // Link ditangani sendiri: jangan biarkan QTextBrowser berpindah "halaman"
    setOpenLinks(false);
    connect(this, &QTextBrowser::anchorClicked, this, &MarkdownView::onAnchorClicked);

    if (m_renderer) {
        connect(m_renderer, &MermaidRenderer::rendered, this, &MarkdownView::onRendered);
        connect(m_renderer, &MermaidRenderer::failed, this, &MarkdownView::onFailed);
    }

    m_resizeDebounce.setSingleShot(true);
    m_resizeDebounce.setInterval(120);
    connect(&m_resizeDebounce, &QTimer::timeout, this, [this]() {
        if (!m_images.isEmpty()) {
            rebuild();
        }
    });
}

void MarkdownView::showMarkdown(const QString &markdown) {
    m_markdown = markdown;
    rebuild();
}

QVariant MarkdownView::loadResource(int type, const QUrl &name) {
    if (type != QTextDocument::ImageResource || name.scheme() != QLatin1String(kScheme)) {
        return QTextBrowser::loadResource(type, name);
    }
    const QString key = name.host();
    const QImage image = m_images.value(key);
    if (image.isNull()) {
        return QVariant();
    }

    // QTextDocument memakai ukuran logis (piksel / devicePixelRatio) untuk tata letak
    const int available = availableImageWidth();
    if (image.width() / image.devicePixelRatio() <= available) {
        return image;
    }
    // Diperkecil ke kerapatan piksel layar (tajam, tanpa diskala lagi saat digambar), sekali per
    // lebar panel: diagram yang dirender seukuran aslinya bisa belasan megapiksel
    const qreal screen = devicePixelRatio();
    const int width = qMax(1, qRound(available * screen));
    const QImage fitted = m_fitted.value(key);
    if (fitted.width() == width && fitted.devicePixelRatio() == screen) {
        return fitted;
    }
    QImage scaled = image.scaledToWidth(width, Qt::SmoothTransformation);
    scaled.setDevicePixelRatio(screen);
    m_fitted.insert(key, scaled);
    return scaled;
}

int MarkdownView::availableImageWidth() const {
    return qMax(120, viewport()->width() - int(document()->documentMargin() * 2) - 8);
}

void MarkdownView::resizeEvent(QResizeEvent *event) {
    QTextBrowser::resizeEvent(event);
    if (event->size().width() != m_lastWidth) {
        m_lastWidth = event->size().width();
        m_resizeDebounce.start();
    }
}

bool MarkdownView::viewportEvent(QEvent *event) {
    if (event->type() == QEvent::ToolTip) {
        const auto *help = static_cast<QHelpEvent *>(event);
        if (QUrl(anchorAt(help->pos())).scheme() == QLatin1String(kScheme)) {
            QToolTip::showText(help->globalPos(), QStringLiteral("Klik untuk memperbesar diagram (zoom dan geser)"),
                               viewport());
            return true;
        }
    }
    return QTextBrowser::viewportEvent(event);
}

void MarkdownView::rebuild() {
    static const QRegularExpression fence(
        QStringLiteral(R"(^[ \t]*```[ \t]*mermaid[^\n]*\n(.*?)^[ \t]*```[ \t]*$)"),
        QRegularExpression::MultilineOption | QRegularExpression::DotMatchesEverythingOption);

    QString unavailable;
    const bool canRender = m_renderer && m_renderer->isAvailable(&unavailable);
    if (!m_renderer) {
        unavailable = QStringLiteral("renderer Mermaid tidak dipasang");
    }

    m_codes.clear();
    QString processed;
    qsizetype last = 0;
    QRegularExpressionMatchIterator it = fence.globalMatch(m_markdown);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        processed += m_markdown.mid(last, match.capturedStart() - last);
        last = match.capturedEnd();

        // Buang newline sebelum pagar penutup; indentasi awal dibiarkan (mindmap bergantung padanya)
        QString code = match.captured(1);
        while (!code.isEmpty() && code.back().isSpace()) {
            code.chop(1);
        }
        const QString key = MermaidRenderer::keyFor(code);
        m_codes.insert(key, code);
        if (m_images.contains(key)) {
            // Dijadikan link oleh linkDiagrams(): klik untuk membuka penampil zoom
            processed += QStringLiteral("![Diagram Mermaid](%1://%2)").arg(QLatin1String(kScheme), key);
            const QImage &image = m_images[key];
            if (image.width() / image.devicePixelRatio() > availableImageWidth()) {
                processed += QStringLiteral("\n\n*Diagram diperkecil agar muat. Klik diagram untuk membukanya "
                                            "di penampil zoom.*");
            }
        } else if (!canRender || m_errors.contains(key)) {
            processed += match.captured(0);
            processed += QStringLiteral("\n\n> Diagram tidak bisa dirender: %1")
                             .arg(canRender ? m_errors.value(key) : unavailable);
        } else {
            processed += QStringLiteral("> Merender diagram…");
            if (!m_requested.contains(key)) {
                m_requested.insert(key);
                m_renderer->render(code);
            }
        }
    }
    processed += m_markdown.mid(last);

    // Diagram yang tidak ada lagi di dokumen dilepas: gambar seukuran aslinya bisa puluhan MB.
    // Bila dokumennya tampil lagi, diagram diminta ulang (renderer mengambilnya dari cache disk).
    for (auto image = m_images.begin(); image != m_images.end();) {
        if (m_codes.contains(image.key())) {
            ++image;
            continue;
        }
        m_requested.remove(image.key());
        m_fitted.remove(image.key());
        image = m_images.erase(image);
    }

    // setMarkdown mengosongkan dokumen (termasuk cache gambar), lalu loadResource dipanggil ulang.
    // Kursor teks dikembalikan ke awal: bila tertinggal di akhir dokumen, QTextEdit menggulir ke
    // bawah untuk menampilkannya saat view berubah ukuran (mis. tab tersembunyi baru dibuka).
    const int scroll = verticalScrollBar()->value();
    document()->setMarkdown(processed, QTextDocument::MarkdownDialectGitHub);
    linkDiagrams();
    setTextCursor(QTextCursor(document()));
    verticalScrollBar()->setValue(scroll);
}

void MarkdownView::linkDiagrams() {
    // Importer Markdown Qt membuang link di sekeliling gambar ([![..](..)](..) jadi <img> saja),
    // jadi format anchor dipasang langsung di karakter gambarnya. Dikumpulkan dulu: mengubah format
    // memecah/menggabung fragmen yang sedang diiterasi.
    QList<QPair<int, QString>> diagrams;   // posisi karakter gambar, nama (mermaid://<key>)
    const QString prefix = QLatin1String(kScheme) + QStringLiteral("://");
    for (QTextBlock block = document()->begin(); block.isValid(); block = block.next()) {
        for (auto part = block.begin(); !part.atEnd(); ++part) {
            const QTextFragment fragment = part.fragment();
            const QString name = fragment.charFormat().toImageFormat().name();
            if (fragment.charFormat().isImageFormat() && name.startsWith(prefix)) {
                for (int offset = 0; offset < fragment.length(); ++offset) {
                    diagrams.append({fragment.position() + offset, name});
                }
            }
        }
    }
    for (const auto &[position, name] : std::as_const(diagrams)) {
        QTextCursor cursor(document());
        cursor.setPosition(position);
        cursor.setPosition(position + 1, QTextCursor::KeepAnchor);
        QTextCharFormat link;
        link.setAnchor(true);
        link.setAnchorHref(name);
        cursor.mergeCharFormat(link);
    }
}

void MarkdownView::onRendered(const QString &key, const QImage &image) {
    if (!m_requested.contains(key)) {
        return;
    }
    m_images.insert(key, image);
    m_fitted.remove(key);
    m_errors.remove(key);
    rebuild();
}

void MarkdownView::onFailed(const QString &key, const QString &error) {
    if (!m_requested.contains(key)) {
        return;
    }
    m_errors.insert(key, error);
    rebuild();
}

void MarkdownView::onAnchorClicked(const QUrl &url) {
    if (url.scheme() == QLatin1String(kScheme)) {
        const QString key = url.host();
        const QImage image = m_images.value(key);
        if (!image.isNull()) {
            DiagramViewer::showDiagram(key, image, DiagramViewer::titleFor(m_codes.value(key)), this);
        }
        return;
    }

    const QString scheme = url.scheme();
    if (scheme == QLatin1String("http") || scheme == QLatin1String("https") || scheme == QLatin1String("file")) {
        QDesktopServices::openUrl(url);
    }
}

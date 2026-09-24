#include "MarkdownView.h"
#include "MermaidRenderer.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScrollBar>
#include <QStandardPaths>
#include <QTextDocument>
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
    const QImage image = m_images.value(name.host());
    if (image.isNull()) {
        return QVariant();
    }

    // QTextDocument memakai ukuran logis (piksel / devicePixelRatio) untuk tata letak
    const qreal ratio = image.devicePixelRatio();
    const int available = availableImageWidth();
    if (image.width() / ratio <= available) {
        return image;
    }
    QImage scaled = image.scaledToWidth(int(available * ratio), Qt::SmoothTransformation);
    scaled.setDevicePixelRatio(ratio);
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

void MarkdownView::rebuild() {
    static const QRegularExpression fence(
        QStringLiteral(R"(^[ \t]*```[ \t]*mermaid[^\n]*\n(.*?)^[ \t]*```[ \t]*$)"),
        QRegularExpression::MultilineOption | QRegularExpression::DotMatchesEverythingOption);

    QString unavailable;
    const bool canRender = m_renderer && m_renderer->isAvailable(&unavailable);
    if (!m_renderer) {
        unavailable = QStringLiteral("renderer Mermaid tidak dipasang");
    }

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
        if (m_images.contains(key)) {
            // Gambar yang sekaligus link: klik untuk membuka ukuran penuh
            processed += QStringLiteral("[![Diagram Mermaid](%1://%2)](%1://%2)").arg(QLatin1String(kScheme), key);
            const QImage &image = m_images[key];
            if (image.width() / image.devicePixelRatio() > availableImageWidth()) {
                processed += QStringLiteral("\n\n*Diagram diperkecil agar muat — klik untuk ukuran penuh, "
                                            "atau lebarkan panel.*");
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

    // setMarkdown mengosongkan dokumen (termasuk cache gambar), lalu loadResource dipanggil ulang
    const int scroll = verticalScrollBar()->value();
    document()->setMarkdown(processed, QTextDocument::MarkdownDialectGitHub);
    verticalScrollBar()->setValue(scroll);
}

void MarkdownView::onRendered(const QString &key, const QImage &image) {
    if (!m_requested.contains(key)) {
        return;
    }
    m_images.insert(key, image);
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
        if (image.isNull()) {
            return;
        }
        const QString path = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                                 .filePath(QStringLiteral("lassomoir-diagram-%1.png").arg(key));
        if (!QFileInfo::exists(path)) {
            image.save(path, "PNG");
        }
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
        return;
    }

    const QString scheme = url.scheme();
    if (scheme == QLatin1String("http") || scheme == QLatin1String("https") || scheme == QLatin1String("file")) {
        QDesktopServices::openUrl(url);
    }
}

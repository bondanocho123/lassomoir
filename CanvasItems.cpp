#include "CanvasItems.h"
#include "CanvasWorkflow.h"
#include "RunLogFormatter.h"
#include "Theme.h"

#include <QApplication>
#include <QCursor>
#include <QFocusEvent>
#include <QFontMetricsF>
#include <QGraphicsScene>
#include <QGraphicsSceneHoverEvent>
#include <QGraphicsSceneMouseEvent>
#include <QGraphicsView>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPathStroker>
#include <QRegularExpression>
#include <QStyleOptionGraphicsItem>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextLayout>

#include <cmath>

namespace {

constexpr qreal kRadius = 10.0;
constexpr qreal kPad = 12.0;
constexpr qreal kChipHeight = 16.0;
constexpr qreal kPortRadius = 5.0;
constexpr qreal kHandle = 14.0;
constexpr int kNoteFontPx = 13;
constexpr qreal kArrowLength = 10.0;
constexpr qreal kArrowHalfWidth = 5.0;
// Di bawah skala ini huruf kartu (12-13 px) jatuh di bawah ~8 px di layar: kartu dilukis ringkas
constexpr qreal kFullDetail = 0.6;

// Warna mode terang; dipetakan ke mode gelap lewat Theme
constexpr QRgb kSurface = 0xffffff;
constexpr QRgb kBorder = 0xe7ddcd;
constexpr QRgb kAccent = 0xa9743f;
constexpr QRgb kNavy = 0x33517a;
constexpr QRgb kBody = 0x3d3730;
constexpr QRgb kMuted = 0x6f6557;

struct Chip {
    QString text;
    QRgb background = 0;
    QRgb foreground = 0;
};

struct NoteColor {
    const char *key;
    const char *label;
    QRgb light;
};

// Semua warna ini sudah punya pasangan gelap di peta Theme::fill
const NoteColor kNoteColors[] = {
    {"amber", "Kuning", 0xfbeccf},
    {"sand", "Krem", 0xf2e3d1},
    {"sage", "Hijau", 0xe3f0e5},
    {"sky", "Biru", 0xe3ebf6},
    {"rose", "Merah muda", 0xf6e0db},
};

// Markdown jadi teks ringkas untuk kartu: tanpa pagar kode, tanda judul, dan baris kosong beruntun
QString previewOf(const QString &text, int maxChars = 900) {
    static const QRegularExpression heading(QStringLiteral(R"(^#{1,6}\s+)"));
    QStringList lines;
    bool lastBlank = true;
    int total = 0;
    const QStringList raw = text.split(QLatin1Char('\n'));
    for (const QString &line : raw) {
        QString clean = line.trimmed();
        if (clean.startsWith(QLatin1String("```"))) {
            continue;
        }
        clean.remove(heading);
        clean.remove(QStringLiteral("**"));
        if (clean.isEmpty()) {
            if (!lastBlank) {
                lines.append(QString());
                lastBlank = true;
            }
            continue;
        }
        lines.append(clean);
        lastBlank = false;
        total += int(clean.size());
        if (total > maxChars) {
            break;
        }
    }
    return lines.join(QLatin1Char('\n')).trimmed();
}

// Isi instruksi sesudah baris pertamanya (baris pertama sudah jadi judul kartu)
QString restOf(const QString &text) {
    const qsizetype newline = text.trimmed().indexOf(QLatin1Char('\n'));
    return newline < 0 ? QString() : previewOf(text.trimmed().mid(newline + 1));
}

QFont sized(const QFont &base, int pixels, bool bold = false) {
    QFont font = base;
    font.setPixelSize(pixels);
    font.setBold(bold);
    return font;
}

qreal drawChip(QPainter *painter, const QPointF &anchor, const Chip &chip, const QFont &font, bool alignRight) {
    if (chip.text.isEmpty()) {
        return 0.0;
    }
    const QFontMetricsF metrics(font);
    const qreal width = metrics.horizontalAdvance(chip.text) + 12.0;
    const QRectF rect(alignRight ? QPointF(anchor.x() - width, anchor.y()) : anchor, QSizeF(width, kChipHeight));
    painter->setPen(Qt::NoPen);
    painter->setBrush(Theme::fill(chip.background));
    painter->drawRoundedRect(rect, 4.0, 4.0);
    painter->setPen(Theme::text(chip.foreground));
    painter->setFont(font);
    painter->drawText(rect, Qt::AlignCenter, chip.text);
    return width;
}

// Teks terbungkus di dalam rect; yang tidak muat dipudarkan ke warna latar di bagian bawah
void drawWrapped(QPainter *painter, const QRectF &rect, const QString &text, const QFont &font, const QColor &color,
                 const QColor &background) {
    if (rect.height() < 4.0 || rect.width() < 8.0 || text.isEmpty()) {
        return;
    }
    const int flags = Qt::TextWordWrap | Qt::AlignLeft | Qt::AlignTop;
    painter->save();
    painter->setClipRect(rect);
    painter->setFont(font);
    painter->setPen(color);
    painter->drawText(rect, flags, text);
    const QFontMetricsF metrics(font);
    if (metrics.boundingRect(rect, flags, text).height() > rect.height()) {
        const qreal fade = qMin(rect.height(), metrics.height() * 1.6);
        const QRectF band(rect.left(), rect.bottom() - fade, rect.width(), fade);
        QLinearGradient gradient(band.topLeft(), band.bottomLeft());
        QColor clear = background;
        clear.setAlpha(0);
        gradient.setColorAt(0.0, clear);
        gradient.setColorAt(1.0, background);
        painter->fillRect(band, gradient);
    }
    painter->restore();
}

void drawElided(QPainter *painter, const QRectF &rect, const QString &text, const QFont &font, const QColor &color) {
    const QFontMetricsF metrics(font);
    painter->setFont(font);
    painter->setPen(color);
    painter->drawText(rect, Qt::AlignLeft | Qt::AlignVCenter, metrics.elidedText(text, Qt::ElideRight, rect.width()));
}

// Judul sampai maxLines baris, dipatahkan di batas kata; sisanya dipotong elipsis di baris terakhir.
// Kembalikan tinggi yang terpakai.
qreal drawTitle(QPainter *painter, const QPointF &topLeft, qreal width, const QString &text, const QFont &font,
                const QColor &color, int maxLines) {
    QTextLayout layout(text, font);
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    layout.setTextOption(option);
    QStringList lines;
    layout.beginLayout();
    for (QTextLine line = layout.createLine(); line.isValid(); line = layout.createLine()) {
        line.setLineWidth(width);
        if (lines.size() == maxLines - 1) {
            lines.append(text.mid(line.textStart()));
            break;
        }
        lines.append(text.mid(line.textStart(), line.textLength()));
    }
    layout.endLayout();

    const QFontMetricsF metrics(font);
    qreal y = topLeft.y();
    for (const QString &line : std::as_const(lines)) {
        drawElided(painter, QRectF(topLeft.x(), y, width, metrics.height()), line.trimmed(), font, color);
        y += metrics.height();
    }
    return qMax<qsizetype>(1, lines.size()) * metrics.height();
}

}

QStringList CanvasPalette::noteColors() {
    QStringList keys;
    for (const NoteColor &color : kNoteColors) {
        keys.append(QLatin1String(color.key));
    }
    return keys;
}

QString CanvasPalette::defaultNoteColor() {
    return QLatin1String(kNoteColors[0].key);
}

QString CanvasPalette::noteColorLabel(const QString &key) {
    for (const NoteColor &color : kNoteColors) {
        if (key == QLatin1String(color.key)) {
            return QString::fromLatin1(color.label);
        }
    }
    return key;
}

QColor CanvasPalette::noteFill(const QString &key) {
    for (const NoteColor &color : kNoteColors) {
        if (key == QLatin1String(color.key)) {
            return Theme::fill(color.light);
        }
    }
    return Theme::fill(kNoteColors[0].light);
}

QPainterPath canvasConnectorPath(const QPointF &from, const QPointF &to) {
    const qreal reach = qMax(60.0, qAbs(to.x() - from.x()) * 0.5);
    QPainterPath path(from);
    path.cubicTo(from + QPointF(reach, 0), to - QPointF(reach, 0), to);
    return path;
}

CanvasTextEditor::CanvasTextEditor(QGraphicsItem *parent) : QGraphicsTextItem(parent) {
    document()->setDocumentMargin(0);
    setTextInteractionFlags(Qt::TextEditorInteraction);
}

void CanvasTextEditor::paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget) {
    // Tanpa bingkai putus-putus bawaan QGraphicsTextItem saat fokus: kartunya sendiri sudah tersorot
    QStyleOptionGraphicsItem plain(*option);
    plain.state &= ~(QStyle::State_Selected | QStyle::State_HasFocus);
    QGraphicsTextItem::paint(painter, &plain, widget);
}

void CanvasTextEditor::focusOutEvent(QFocusEvent *event) {
    QGraphicsTextItem::focusOutEvent(event);
    // Menu klik kanan editor mengambil fokus sebentar; itu belum berarti selesai mengetik
    if (event->reason() == Qt::PopupFocusReason || m_done) {
        return;
    }
    m_done = true;
    if (finished) {
        finished();
    }
}

void CanvasTextEditor::keyPressEvent(QKeyEvent *event) {
    const bool commit = event->key() == Qt::Key_Escape
                        || ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
                            && (event->modifiers() & Qt::ControlModifier));
    if (commit) {
        clearFocus();
        event->accept();
        return;
    }
    QGraphicsTextItem::keyPressEvent(event);
}

CanvasNodeItem::CanvasNodeItem(const CanvasNode &node) : m_node(node), m_size(node.size) {
    setFlags(ItemIsMovable | ItemIsSelectable | ItemSendsGeometryChanges);
    setAcceptHoverEvents(true);
    setPos(node.pos);
    refreshTexts();
}

CanvasNodeItem::~CanvasNodeItem() {
    // Editor kehilangan fokus saat ikut dibongkar; kartu yang sedang dihancurkan tidak boleh dipanggil lagi
    if (m_editor) {
        m_editor->finished = nullptr;
    }
}

void CanvasNodeItem::setNode(const CanvasNode &node) {
    if (node.size != m_size && !m_resizing) {
        prepareGeometryChange();
        m_size = node.size;
    }
    m_node = node;
    if (pos() != node.pos) {
        setPos(node.pos);
    }
    if (m_editor) {
        m_editor->setTextWidth(m_size.width() - 2 * kPad);
    }
    refreshTexts();
    updateEdges();
    update();
}

void CanvasNodeItem::setRunState(RunState state) {
    if (m_runState == state) {
        return;
    }
    m_runState = state;
    if (state != RunState::Running) {
        m_pulse = 0.0;
    }
    refreshTexts();
    update();
}

void CanvasNodeItem::setPulse(qreal level) {
    m_pulse = level;
    update();
}

bool CanvasNodeItem::wantsThumbnail() const {
    return CanvasWorkflow::isImageReference(m_node);
}

void CanvasNodeItem::setThumbnail(const QImage &image) {
    m_thumbnail = image;
    update();
}

QPointF CanvasNodeItem::inPort() const {
    return mapToScene(QPointF(0, m_size.height() / 2));
}

QPointF CanvasNodeItem::outPort() const {
    return mapToScene(QPointF(m_size.width(), m_size.height() / 2));
}

bool CanvasNodeItem::hitsOutPort(const QPointF &scenePos, qreal tolerance) const {
    const QPointF delta = scenePos - outPort();
    return std::hypot(delta.x(), delta.y()) <= qMax(tolerance, kPortRadius + 2);
}

void CanvasNodeItem::addEdge(CanvasEdgeItem *edge) {
    if (!m_edges.contains(edge)) {
        m_edges.append(edge);
        update();
    }
}

void CanvasNodeItem::removeEdge(CanvasEdgeItem *edge) {
    m_edges.removeAll(edge);
    update();
}

void CanvasNodeItem::beginEdit() {
    if (m_editor || m_node.kind != CanvasNodeKind::Note) {
        return;
    }
    auto *editor = new CanvasTextEditor(this);
    QFont font = QApplication::font();
    if (scene() && !scene()->views().isEmpty()) {
        font = scene()->views().first()->viewport()->font();
    }
    editor->setFont(sized(font, kNoteFontPx));
    editor->setDefaultTextColor(Theme::text(kBody));
    editor->setPlainText(m_node.text);
    editor->setTextWidth(m_size.width() - 2 * kPad);
    editor->setPos(kPad, kPad);
    editor->finished = [this]() { finishEdit(); };
    // Kartu ikut memanjang selagi diketik, supaya teksnya tidak keluar dari kertas
    connect(editor->document(), &QTextDocument::contentsChanged, this, [this]() {
        if (!m_editor) {
            return;
        }
        const qreal needed = m_editor->document()->size().height() + 2 * kPad;
        if (needed > m_size.height()) {
            prepareGeometryChange();
            m_size.setHeight(needed);
            updateEdges();
            update();
        }
    });
    m_editor = editor;
    editor->setFocus(Qt::OtherFocusReason);
    QTextCursor cursor = editor->textCursor();
    cursor.movePosition(QTextCursor::End);
    editor->setTextCursor(cursor);
    update();
}

void CanvasNodeItem::finishEdit() {
    if (!m_editor) {
        return;
    }
    const QString text = m_editor->toPlainText();
    const QSizeF needed(m_size.width(), m_editor->document()->size().height() + 2 * kPad);
    m_editor->deleteLater();
    m_editor = nullptr;
    update();
    emit edited(m_node.id, text, needed);
}

qreal CanvasNodeItem::viewScale() const {
    if (scene() && !scene()->views().isEmpty()) {
        return scene()->views().first()->transform().m11();
    }
    return 1.0;
}

bool CanvasNodeItem::hitsResizeHandle(const QPointF &localPos) const {
    const qreal side = qMax(kHandle, kHandle / viewScale());
    return QRectF(m_size.width() - side, m_size.height() - side, side, side).contains(localPos);
}

void CanvasNodeItem::refreshTexts() {
    m_heading.clear();
    m_meta.clear();
    m_preview.clear();
    switch (m_node.kind) {
    case CanvasNodeKind::Note:
        m_preview = m_node.text.left(2000);
        break;
    case CanvasNodeKind::Artifact:
    case CanvasNodeKind::Task:
        m_heading = m_node.title;
        m_meta = m_node.detail.isEmpty() ? m_node.source.projectId
                                         : QStringLiteral("%1 · %2").arg(m_node.detail, m_node.source.projectId);
        if (!CanvasWorkflow::isImageReference(m_node)) {
            m_preview = previewOf(m_node.text);
        }
        break;
    case CanvasNodeKind::Step: {
        m_heading = CanvasWorkflow::firstLine(m_node.text);
        if (m_heading.isEmpty()) {
            m_heading = QStringLiteral("Langkah AI tanpa instruksi");
        }
        const AgentDefinition agent = CanvasWorkflow::brainstormAgent(m_node.model, m_node.effort);
        QStringList meta = {agent.model, agent.effort};
        if (m_node.hasResult() && m_node.result.durationMs > 0) {
            meta.append(RunLogFormatter::formatDuration(m_node.result.durationMs));
        }
        if (m_node.hasResult() && m_node.result.totalTokens > 0) {
            meta.append(QStringLiteral("%1 tok").arg(RunLogFormatter::formatTokens(m_node.result.totalTokens)));
        }
        m_meta = meta.join(QStringLiteral(" · "));
        if (m_runState == RunState::Running) {
            m_preview = QStringLiteral("Agent sedang bekerja… keluarannya tampil di panel Detail.");
        } else if (m_runState == RunState::Queued) {
            m_preview = QStringLiteral("Menunggu giliran atau menunggu langkah hulunya selesai.");
        } else if (m_node.result.success) {
            m_preview = previewOf(m_node.result.message);
        } else if (m_node.hasResult()) {
            m_preview = QStringLiteral("Gagal (%1): %2").arg(m_node.result.outcome, m_node.result.message.left(400));
        } else {
            const QString rest = restOf(m_node.text);
            m_preview = !rest.isEmpty() ? rest
                                        : QStringLiteral("Sambungkan kartu bahan ke titik kiri kartu ini, lalu jalankan ▶.");
        }
        break;
    }
    }
}

QRectF CanvasNodeItem::boundingRect() const {
    return cardRect().adjusted(-6, -6, kPortRadius + 6, 8);
}

QPainterPath CanvasNodeItem::shape() const {
    QPainterPath path;
    path.addRoundedRect(cardRect(), kRadius, kRadius);
    path.addEllipse(QPointF(m_size.width(), m_size.height() / 2), kPortRadius + 4, kPortRadius + 4);
    return path;
}

void CanvasNodeItem::paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget) {
    const qreal detail = option->levelOfDetailFromTransform(painter->worldTransform());
    const QRectF card = cardRect();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setRenderHint(QPainter::TextAntialiasing);

    painter->setPen(Qt::NoPen);
    painter->setBrush(QColor(0, 0, 0, Theme::isDark() ? 70 : 20));
    painter->drawRoundedRect(card.translated(0, 2), kRadius, kRadius);

    const bool note = m_node.kind == CanvasNodeKind::Note;
    const QColor surface = note ? CanvasPalette::noteFill(m_node.color) : Theme::fill(kSurface);
    QColor border = Theme::fill(kBorder);
    Qt::PenStyle style = Qt::SolidLine;
    if (m_runState == RunState::Running) {
        border = Theme::fill(kNavy);
    } else if (m_runState == RunState::Queued) {
        border = Theme::fill(kNavy);
        style = Qt::DashLine;
    } else if (m_node.kind == CanvasNodeKind::Step && m_node.hasResult()) {
        border = Theme::fill(m_node.result.success ? 0x9cc9a0 : 0xb4442f);
    } else if (m_node.isReference() && !m_node.available) {
        border = Theme::fill(0xd9cdbb);
        style = Qt::DashLine;
    }
    qreal width = 1.0;
    if (isSelected()) {
        border = Theme::fill(kAccent);
        width = 2.0;
        style = Qt::SolidLine;
    } else if (m_hovered) {
        border = Theme::fill(kAccent);
    }
    QPen pen(border, width, style);
    pen.setCosmetic(true);
    painter->setPen(pen);
    painter->setBrush(surface);
    painter->drawRoundedRect(card.adjusted(0.5, 0.5, -0.5, -0.5), kRadius, kRadius);

    if (m_runState == RunState::Running) {
        QColor glow = Theme::text(kNavy);
        glow.setAlphaF(0.12 * m_pulse);
        QColor line = Theme::text(kNavy);
        line.setAlphaF(0.25 + 0.75 * m_pulse);
        QPen pulsePen(line, 2.0);
        pulsePen.setCosmetic(true);
        painter->setPen(pulsePen);
        painter->setBrush(glow);
        painter->drawRoundedRect(card.adjusted(1, 1, -1, -1), kRadius - 1, kRadius - 1);
    }

    const QFont base = widget ? widget->font() : QApplication::font();
    if (detail >= kFullDetail) {
        if (note) {
            paintNote(painter, base);
        } else {
            paintCard(painter, base);
        }
    } else {
        paintOverview(painter, base, detail);
    }

    if (isSelected() || m_hovered) {
        QPen portPen(Theme::fill(kAccent), 1.5);
        portPen.setCosmetic(true);
        painter->setPen(portPen);
        painter->setBrush(Theme::fill(kSurface));
        painter->drawEllipse(QPointF(m_size.width(), m_size.height() / 2), kPortRadius, kPortRadius);

        QPen grip(Theme::text(kMuted), 1.0);
        grip.setCosmetic(true);
        painter->setPen(grip);
        const QPointF corner(m_size.width() - 4, m_size.height() - 4);
        for (int i = 1; i <= 3; ++i) {
            const qreal d = 3.0 * i;
            painter->drawLine(corner - QPointF(d, 0), corner - QPointF(0, d));
        }
    }
}

void CanvasNodeItem::paintNote(QPainter *painter, const QFont &base) {
    if (m_editor) {
        return;
    }
    const QRectF area = cardRect().adjusted(kPad, kPad, -kPad, -kPad);
    if (m_preview.trimmed().isEmpty()) {
        QFont hint = sized(base, kNoteFontPx);
        hint.setItalic(true);
        drawWrapped(painter, area, QStringLiteral("Klik dua kali untuk menulis…"), hint, Theme::text(0x8a7f70),
                    CanvasPalette::noteFill(m_node.color));
        return;
    }
    drawWrapped(painter, area, m_preview, sized(base, kNoteFontPx), Theme::text(kBody), CanvasPalette::noteFill(m_node.color));
}

void CanvasNodeItem::paintCard(QPainter *painter, const QFont &base) {
    const qreal inner = m_size.width() - 2 * kPad;
    const QFont chipFont = sized(base, 10, true);
    qreal y = kPad;

    Chip kind;
    Chip status;
    switch (m_node.kind) {
    case CanvasNodeKind::Artifact:
        kind = {m_node.source.stage.isEmpty() ? (wantsThumbnail() ? QStringLiteral("FOTO") : QStringLiteral("LAMPIRAN"))
                                              : m_node.source.stage,
                0xf2e3d1, 0x8a5a2f};
        break;
    case CanvasNodeKind::Task:
        kind = {QStringLiteral("TASK"), 0xe3ebf6, 0x2f4b73};
        if (m_node.detail.contains(QStringLiteral("menunggu review"))) {
            status = {QStringLiteral("REVIEW"), 0xfbeccf, 0x7a4a12};
        } else if (m_node.detail.contains(QStringLiteral("gagal"))) {
            status = {QStringLiteral("GAGAL"), 0xf6e0db, 0x9b2c1f};
        } else if (m_node.detail.startsWith(QStringLiteral("DONE"))) {
            status = {QStringLiteral("SELESAI"), 0xe3f0e5, 0x2f7d32};
        }
        break;
    case CanvasNodeKind::Step:
        kind = {m_node.output == CanvasStepOutput::Tasks ? QStringLiteral("LANGKAH AI → TASK") : QStringLiteral("LANGKAH AI"),
                kNavy, 0xffffff};
        if (m_runState == RunState::Running) {
            status = {QStringLiteral("BERJALAN"), kNavy, 0xffffff};
        } else if (m_runState == RunState::Queued) {
            status = {QStringLiteral("ANTRE"), 0xe3ebf6, 0x2f4b73};
        } else if (m_node.hasResult()) {
            status = m_node.result.success ? Chip{QStringLiteral("SELESAI"), 0xe3f0e5, 0x2f7d32}
                                           : Chip{QStringLiteral("GAGAL"), 0xf6e0db, 0x9b2c1f};
        }
        break;
    case CanvasNodeKind::Note:
        break;
    }
    if (m_node.isReference() && !m_node.available) {
        status = {QStringLiteral("TIDAK TERSEDIA"), 0xefe9e1, 0x8a7f70};
    }
    drawChip(painter, QPointF(kPad, y), kind, chipFont, false);
    drawChip(painter, QPointF(m_size.width() - kPad, y), status, chipFont, true);
    y += kChipHeight + 8;

    // Judul dua baris selama kartunya cukup tinggi untuk tetap menyisakan ruang pratinjau
    const int titleLines = m_size.height() >= 120 ? 2 : 1;
    y += drawTitle(painter, QPointF(kPad, y), inner, m_heading, sized(base, 13, true), Theme::text(kNavy), titleLines) + 2;

    QString meta = m_meta;
    if (m_node.kind == CanvasNodeKind::Step) {
        int inputs = 0;
        for (const CanvasEdgeItem *edge : m_edges) {
            inputs += edge->to() == this ? 1 : 0;
        }
        meta = QStringLiteral("%1 bahan · %2").arg(inputs).arg(m_meta);
    }
    const QFont metaFont = sized(base, 11);
    const qreal metaHeight = QFontMetricsF(metaFont).height();
    drawElided(painter, QRectF(kPad, y, inner, metaHeight), meta, metaFont, Theme::text(kMuted));
    y += metaHeight + 8;

    const QRectF body(kPad, y, inner, m_size.height() - y - kPad);
    if (wantsThumbnail()) {
        if (!m_thumbnail.isNull() && body.height() > 8) {
            const QSizeF fitted = QSizeF(m_thumbnail.size()).scaled(body.size(), Qt::KeepAspectRatio);
            const QRectF target(body.left() + (body.width() - fitted.width()) / 2, body.top(), fitted.width(), fitted.height());
            QPainterPath clip;
            clip.addRoundedRect(target, 6, 6);
            painter->save();
            painter->setClipPath(clip, Qt::IntersectClip);
            painter->setRenderHint(QPainter::SmoothPixmapTransform);
            painter->drawImage(target, m_thumbnail);
            painter->restore();
        }
        return;
    }
    const bool failed = m_node.kind == CanvasNodeKind::Step && m_node.hasResult() && !m_node.result.success
                        && m_runState == RunState::Idle;
    drawWrapped(painter, body, m_preview, sized(base, 12), Theme::text(failed ? 0x9b2c1f : kBody), Theme::fill(kSurface));
}

// Diperkecil jauh: rincian kartu tidak terbaca lagi. Tinggal pita warna jenisnya dan judul berhuruf
// ~11 px di layar, supaya papan besar tetap bisa dibaca sebagai peta ide.
void CanvasNodeItem::paintOverview(QPainter *painter, const QFont &base, qreal detail) {
    const bool note = m_node.kind == CanvasNodeKind::Note;
    qreal top = 0.0;
    if (!note) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(Theme::fill(m_node.kind == CanvasNodeKind::Step ? kNavy
                                      : m_node.kind == CanvasNodeKind::Task ? 0xe3ebf6 : 0xf2e3d1));
        painter->drawRoundedRect(QRectF(0, 0, m_size.width(), 28).adjusted(1, 1, -1, 0), kRadius - 1, kRadius - 1);
        top = 28.0;
    }
    const QRectF area = QRectF(0, top, m_size.width(), m_size.height() - top).adjusted(kPad, kPad / 2, -kPad, -kPad / 2);
    const qreal scale = qMax(detail, 0.01);
    const int pixels = int(qMin(11.0 / scale, area.height() * 0.45));
    if (pixels * scale < 5.0) {
        return;
    }
    drawWrapped(painter, area, note ? CanvasWorkflow::firstLine(m_preview) : m_heading, sized(base, pixels, true),
                Theme::text(note ? kBody : kNavy), note ? CanvasPalette::noteFill(m_node.color) : Theme::fill(kSurface));
}

QVariant CanvasNodeItem::itemChange(GraphicsItemChange change, const QVariant &value) {
    if (change == ItemPositionHasChanged) {
        updateEdges();
    } else if (change == ItemSelectedHasChanged) {
        update();
    }
    return QGraphicsObject::itemChange(change, value);
}

void CanvasNodeItem::hoverEnterEvent(QGraphicsSceneHoverEvent *event) {
    m_hovered = true;
    update();
    QGraphicsObject::hoverEnterEvent(event);
}

void CanvasNodeItem::hoverMoveEvent(QGraphicsSceneHoverEvent *event) {
    if (hitsResizeHandle(event->pos())) {
        setCursor(Qt::SizeFDiagCursor);
    } else if (hitsOutPort(event->scenePos(), 10.0 / viewScale())) {
        setCursor(Qt::CrossCursor);
    } else {
        unsetCursor();
    }
    QGraphicsObject::hoverMoveEvent(event);
}

void CanvasNodeItem::hoverLeaveEvent(QGraphicsSceneHoverEvent *event) {
    m_hovered = false;
    unsetCursor();
    update();
    QGraphicsObject::hoverLeaveEvent(event);
}

void CanvasNodeItem::mousePressEvent(QGraphicsSceneMouseEvent *event) {
    if (event->button() == Qt::LeftButton && hitsResizeHandle(event->pos())) {
        m_resizing = true;
        m_resizeOrigin = event->scenePos();
        m_resizeStart = m_size;
        event->accept();
        return;
    }
    QGraphicsObject::mousePressEvent(event);
}

void CanvasNodeItem::mouseMoveEvent(QGraphicsSceneMouseEvent *event) {
    if (m_resizing) {
        const QPointF delta = event->scenePos() - m_resizeOrigin;
        const QSizeF size = QSizeF(m_resizeStart.width() + delta.x(), m_resizeStart.height() + delta.y())
                                .expandedTo(CanvasNode::minimumSize(m_node.kind));
        if (size != m_size) {
            prepareGeometryChange();
            m_size = size;
            if (m_editor) {
                m_editor->setTextWidth(m_size.width() - 2 * kPad);
            }
            updateEdges();
            update();
        }
        event->accept();
        return;
    }
    QGraphicsObject::mouseMoveEvent(event);
}

void CanvasNodeItem::mouseReleaseEvent(QGraphicsSceneMouseEvent *event) {
    if (m_resizing) {
        m_resizing = false;
        event->accept();
        if (m_size != m_node.size) {
            emit resized(m_node.id, m_size);
        }
        return;
    }
    QGraphicsObject::mouseReleaseEvent(event);
}

void CanvasNodeItem::updateEdges() {
    for (CanvasEdgeItem *edge : std::as_const(m_edges)) {
        edge->updatePath();
    }
}

CanvasEdgeItem::CanvasEdgeItem(const CanvasEdge &edge, CanvasNodeItem *from, CanvasNodeItem *to)
    : m_edge(edge), m_from(from), m_to(to) {
    setFlags(ItemIsSelectable);
    setAcceptHoverEvents(true);
    setZValue(-1);
    m_from->addEdge(this);
    m_to->addEdge(this);
    updatePath();
}

void CanvasEdgeItem::updatePath() {
    prepareGeometryChange();
    const QPointF start = m_from->outPort();
    const QPointF tip = m_to->inPort();
    // Garis berhenti di pangkal panah supaya ujungnya tidak menembus mata panah
    m_path = canvasConnectorPath(start, tip - QPointF(kArrowLength - 1, 0));
    m_arrow = QPolygonF({tip, tip + QPointF(-kArrowLength, -kArrowHalfWidth), tip + QPointF(-kArrowLength, kArrowHalfWidth)});
}

QRectF CanvasEdgeItem::boundingRect() const {
    return m_path.controlPointRect().united(m_arrow.boundingRect()).adjusted(-8, -8, 8, 8);
}

QPainterPath CanvasEdgeItem::shape() const {
    QPainterPathStroker stroker;
    stroker.setWidth(12);
    QPainterPath path = stroker.createStroke(m_path);
    path.addPolygon(m_arrow);
    return path;
}

void CanvasEdgeItem::paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget) {
    Q_UNUSED(option);
    Q_UNUSED(widget);
    // Garis yang masuk ke langkah AI = bahan untuk agent: navy; garis lain sekadar keterkaitan ide
    const bool feedsStep = m_to->node().kind == CanvasNodeKind::Step;
    QColor color = feedsStep ? Theme::fill(kNavy) : Theme::fill(0xb3a898);
    if (isSelected() || m_hovered) {
        color = Theme::fill(kAccent);
    }
    painter->setRenderHint(QPainter::Antialiasing);
    QPen pen(color, isSelected() ? 2.5 : 1.6);
    pen.setCosmetic(true);
    painter->setPen(pen);
    painter->setBrush(Qt::NoBrush);
    painter->drawPath(m_path);
    painter->setPen(Qt::NoPen);
    painter->setBrush(color);
    painter->drawPolygon(m_arrow);
}

void CanvasEdgeItem::hoverEnterEvent(QGraphicsSceneHoverEvent *event) {
    m_hovered = true;
    update();
    QGraphicsItem::hoverEnterEvent(event);
}

void CanvasEdgeItem::hoverLeaveEvent(QGraphicsSceneHoverEvent *event) {
    m_hovered = false;
    update();
    QGraphicsItem::hoverLeaveEvent(event);
}

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
#include <QLineF>
#include <QPainter>
#include <QPainterPathStroker>
#include <QRegularExpression>
#include <QStyleOptionGraphicsItem>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextLayout>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

constexpr qreal kRadius = 10.0;
constexpr qreal kPad = 12.0;
constexpr qreal kPortRadius = 5.0;
constexpr qreal kHandle = 14.0;
constexpr int kNoteFontPx = 13;
constexpr qreal kArrowLength = 10.0;
// Titik kendali lengkung garis paling jauh sebegini dari titik tempelnya selama kartunya berdekatan
constexpr qreal kReach = 60.0;
// Lorong garis diperiksa sedikit lebih lebar daripada garisnya sendiri
constexpr qreal kLaneMargin = 12.0;
// Di bawah skala ini huruf catatan (13 px) tidak nyaman lagi dibaca: catatan dilukis sebagai ringkasan
constexpr qreal kNoteSummaryScale = 0.72;
// Baris pertama catatan menjadi judul ringkasannya selama masih sependek judul
constexpr int kHeadingMaxChars = 90;

// Ukuran huruf di kanvas (px pada zoom 100%) dan batas bawahnya di layar. Saat kanvas diperkecil huruf
// berhenti mengecil di batas itu: kartu memuat lebih sedikit teks, tetapi teksnya tetap terbaca.
struct TypeSize {
    int base;
    qreal floor;
};
constexpr TypeSize kTitleType{13, 10.5};
constexpr TypeSize kBodyType{12, 9.5};
constexpr TypeSize kMetaType{11, 9.0};
constexpr TypeSize kLabelType{10, 8.0};
constexpr TypeSize kNoteType{13, 10.0};
// Judul yang tidak muat boleh mengecil sampai sebesar ini di layar sebelum dipotong elipsis
constexpr qreal kTitleShrinkFloor = 8.0;

// Warna mode terang; dipetakan ke mode gelap lewat Theme
constexpr QRgb kSurface = 0xffffff;
constexpr QRgb kBorder = 0xe7ddcd;
constexpr QRgb kAccent = 0xa9743f;
constexpr QRgb kNavy = 0x33517a;
constexpr QRgb kBody = 0x3d3730;
constexpr QRgb kMuted = 0x6f6557;

constexpr CanvasSide kSides[] = {CanvasSide::Left, CanvasSide::Top, CanvasSide::Right, CanvasSide::Bottom};

struct Chip {
    QString text;
    QRgb background = 0;
    QRgb foreground = 0;
};

// Pita di puncak kartu referensi dan langkah AI: warna jenisnya, nama jenis, dan status
struct CardKind {
    QString label;
    QString brief;   // nama jenis yang lebih pendek untuk pita sempit; kosong = tidak ada
    QRgb band = 0;
    QRgb ink = 0;
    Chip status;
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

// Isi sesudah baris yang dipilih CanvasWorkflow::firstLine sebagai judulnya
QString afterHeading(const QString &text) {
    qsizetype start = 0;
    while (start <= text.size()) {
        qsizetype end = text.indexOf(QLatin1Char('\n'), start);
        if (end < 0) {
            end = text.size();
        }
        if (!CanvasWorkflow::firstLine(text.mid(start, end - start)).isEmpty()) {
            return text.mid(end + 1);
        }
        start = end + 1;
    }
    return QString();
}

QFont sized(const QFont &base, int pixels, bool bold = false) {
    QFont font = base;
    font.setPixelSize(pixels);
    font.setBold(bold);
    return font;
}

// Satuan tata letak: berapa px kanvas untuk 1 px layar. Sampai zoom ±83% semua ukuran masih memakai
// nilai dasarnya, jadi tata letak tidak berubah dan kartu sekadar diperbesar/diperkecil. Di bawah itu
// satuannya dibulatkan ke langkah 4%, supaya teks tidak ditata ulang di setiap bingkai selama zoom.
qreal layoutUnit(qreal scale) {
    const qreal unit = 1.0 / qMax(scale, 0.01);
    if (unit <= 1.2) {
        return 1.0;
    }
    static const qreal step = std::log(1.04);
    return std::exp(std::round(std::log(unit) / step) * step);
}

int typePx(const TypeSize &type, qreal unit) {
    return qMax(type.base, qRound(type.floor * unit));
}

// Jarak `base` px di kanvas yang tidak pernah lebih kecil dari `screen` px di layar
qreal atLeast(qreal base, qreal screen, qreal unit) {
    return qMax(base, screen * unit);
}

qreal lineHeightOf(const QFont &font) {
    const QFontMetricsF metrics(font);
    return std::ceil(metrics.height() + qMax(0.0, metrics.leading()));
}

// Teks terbungkus yang sudah ditata baris demi baris. Yang tidak muat tidak pernah terpotong di
// tengah baris: baris terakhir yang muat diakhiri elipsis.
struct TextFlow {
    struct Line {
        QString text;
        qreal top = 0.0;
    };
    // Satu paragraf yang hurufnya sudah dibentuk dan barisnya sudah diberi tempat: melukis ulang
    // (hover, denyut, kanvas digeser) tidak membentuknya lagi
    struct Run {
        std::shared_ptr<QTextLayout> layout;
        int lines = 0;   // sekian baris pertamanya yang tampil
    };
    QList<Line> lines;
    QList<Run> runs;
    qreal lineHeight = 0.0;
    qreal height = 0.0;        // puncak baris pertama sampai dasar baris terakhir
    bool truncated = false;    // ada teks yang tidak tampil
    bool brokenWord = false;   // ada kata yang lebih lebar dari barisnya, jadi terpatah di tengah

    bool isEmpty() const { return lines.isEmpty(); }
};

// Huruf atau angka yang menyatu dengan tetangganya dalam satu kata. Aksara tanpa spasi (Han, kana)
// memang boleh patah di antara hurufnya, jadi tidak termasuk.
bool gluedLetter(QChar c) {
    switch (c.category()) {
    case QChar::Letter_Uppercase:
    case QChar::Letter_Lowercase:
    case QChar::Letter_Titlecase:
    case QChar::Letter_Modifier:
    case QChar::Number_DecimalDigit:
        return true;
    default:
        return false;
    }
}

// maxHeight < 0 dan maxLines <= 0 berarti tanpa batas. blankLine: tinggi baris kosong antar paragraf
// sebagai bagian dari tinggi baris (pratinjau memakai setengah baris supaya lebih rapat).
TextFlow flowText(const QString &text, const QFont &font, qreal width, qreal maxHeight = -1.0, int maxLines = 0,
                  qreal blankLine = 1.0) {
    TextFlow flow;
    const QFontMetricsF metrics(font);
    flow.lineHeight = lineHeightOf(font);
    if (text.isEmpty() || width < 1.0) {
        return flow;
    }
    int lineLimit = maxLines > 0 ? maxLines : std::numeric_limits<int>::max();
    if (maxHeight >= 0.0) {
        lineLimit = qMin(lineLimit, int((maxHeight + 0.5) / flow.lineHeight));
    }
    if (lineLimit <= 0) {
        flow.truncated = true;
        return flow;
    }
    // Yang jelas tidak akan muat tidak ikut ditata: paragraf panjang dipotong kira-kira seisi kotaknya
    const qsizetype lineChars = qsizetype(width / qMax(1.0, metrics.averageCharWidth() * 0.4)) + 16;
    const qsizetype charLimit = lineLimit > 4096 ? -1 : lineChars * (lineLimit + 1);

    QTextOption wrap;
    wrap.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    qreal y = 0.0;
    QString tailSource;   // paragraf baris terakhir yang muat, dan awal baris itu di dalamnya
    int tailStart = 0;
    bool more = false;
    // Paragraf diambil satu per satu: teks panjang tidak dipecah seluruhnya hanya untuk beberapa baris awalnya
    for (qsizetype position = 0; position <= text.size() && !more;) {
        qsizetype end = text.indexOf(QLatin1Char('\n'), position);
        if (end < 0) {
            end = text.size();
        }
        QString paragraph = text.mid(position, end - position);
        position = end + 1;
        paragraph.replace(QLatin1Char('\t'), QLatin1String("    "));
        if (paragraph.endsWith(QLatin1Char('\r'))) {
            paragraph.chop(1);
        }
        if (paragraph.trimmed().isEmpty()) {
            y += flow.lineHeight * blankLine;
            continue;
        }
        const bool cut = charLimit >= 0 && paragraph.size() > charLimit;
        if (cut) {
            paragraph.truncate(charLimit);
        }
        auto layout = std::make_shared<QTextLayout>(paragraph, font);
        layout->setCacheEnabled(true);
        layout->setTextOption(wrap);
        layout->beginLayout();
        int shown = 0;
        for (QTextLine line = layout->createLine(); line.isValid(); line = layout->createLine()) {
            line.setLineWidth(width);
            if (flow.lines.size() >= lineLimit || (maxHeight >= 0.0 && y + flow.lineHeight > maxHeight + 0.5)) {
                more = true;
                break;
            }
            line.setPosition(QPointF(0.0, y));
            QString piece = paragraph.mid(line.textStart(), line.textLength());
            while (piece.endsWith(QLatin1Char(' '))) {
                piece.chop(1);
            }
            flow.lines.append({piece, y});
            ++shown;
            const int next = line.textStart() + line.textLength();
            if (next > 0 && next < paragraph.size() && gluedLetter(paragraph.at(next - 1))
                && gluedLetter(paragraph.at(next))) {
                flow.brokenWord = true;
            }
            tailSource = paragraph;
            tailStart = line.textStart();
            y += flow.lineHeight;
        }
        layout->endLayout();
        if (shown > 0) {
            flow.runs.append({layout, shown});
        }
        more = more || cut;
    }
    if (more) {
        flow.truncated = true;
        if (!flow.lines.isEmpty()) {
            // Baris terakhir yang muat diganti sisa paragrafnya sejauh lebarnya, diakhiri elipsis
            const QString tail = tailSource.mid(tailStart, lineChars).trimmed();
            QString elided = metrics.elidedText(tail, Qt::ElideRight, width);
            if (elided == tail) {
                // Sisa paragrafnya muat: elipsisnya menandai paragraf-paragraf sesudahnya, menggantikan
                // tanda baca penutupnya ("selesai.…" jadi "selesai…")
                QString ended = tail;
                while (!ended.isEmpty() && QStringLiteral(".,:;").contains(ended.back())) {
                    ended.chop(1);
                }
                elided = metrics.elidedText(ended + QChar(0x2026), Qt::ElideRight, width);
            }
            flow.lines.last().text = elided;
            if (--flow.runs.last().lines == 0) {
                flow.runs.removeLast();
            }
            QTextOption single;
            single.setWrapMode(QTextOption::NoWrap);
            auto last = std::make_shared<QTextLayout>(flow.lines.constLast().text, font);
            last->setCacheEnabled(true);
            last->setTextOption(single);
            last->beginLayout();
            QTextLine line = last->createLine();
            if (line.isValid()) {
                line.setLineWidth(width);
                line.setPosition(QPointF(0.0, flow.lines.constLast().top));
            }
            last->endLayout();
            flow.runs.append({last, line.isValid() ? 1 : 0});
        }
    }
    if (!flow.lines.isEmpty()) {
        flow.height = flow.lines.constLast().top + flow.lineHeight;
    }
    return flow;
}

// Judul yang tidak muat dengan hurufnya sendiri, atau katanya terpatah di tengah, ditata dengan huruf
// kecil; yang tetap tidak muat dipotong elipsis. Hanya dua ukuran, supaya judul kartu-kartu yang
// berjajar tidak belang-belang.
TextFlow fitTitle(const QString &text, const QFont &font, int smallPx, qreal width, qreal height) {
    // Makin kecil makin baik: 0 utuh, 1 ada kata terpatah, 2 terpotong
    auto flaw = [](const TextFlow &flow) { return flow.truncated ? 2 : flow.brokenWord ? 1 : 0; };
    const TextFlow normal = flowText(text, font, width, height);
    if (flaw(normal) == 0 || smallPx >= font.pixelSize()) {
        return normal;
    }
    const TextFlow small = flowText(text, sized(font, smallPx, font.bold()), width, height);
    // Sama-sama terpotong: huruf kecil menampilkan lebih banyak. Selain itu huruf kecil hanya dipakai
    // bila memang memperbaiki sesuatu.
    return flaw(small) < flaw(normal) || small.truncated ? small : normal;
}

void drawFlow(QPainter *painter, const QPointF &topLeft, const TextFlow &flow, const QColor &color) {
    painter->setPen(color);
    for (const TextFlow::Run &run : flow.runs) {
        for (int i = 0; i < run.lines; ++i) {
            run.layout->lineAt(i).draw(painter, topLeft);
        }
    }
}

CardKind kindOf(const CanvasNode &node, RunState state) {
    CardKind kind;
    switch (node.kind) {
    case CanvasNodeKind::Artifact:
        kind.label = !node.source.stage.isEmpty() ? node.source.stage
                     : CanvasWorkflow::isImageReference(node) ? QStringLiteral("FOTO") : QStringLiteral("LAMPIRAN");
        kind.band = 0xf2e3d1;
        kind.ink = 0x8a5a2f;
        break;
    case CanvasNodeKind::Task:
        kind.label = QStringLiteral("TASK");
        kind.band = 0xe3ebf6;
        kind.ink = 0x2f4b73;
        if (node.detail.contains(QStringLiteral("menunggu review"))) {
            kind.status = {QStringLiteral("REVIEW"), 0xfbeccf, 0x7a4a12};
        } else if (node.detail.contains(QStringLiteral("gagal"))) {
            kind.status = {QStringLiteral("GAGAL"), 0xf6e0db, 0x9b2c1f};
        } else if (node.detail.startsWith(QStringLiteral("DONE"))) {
            kind.status = {QStringLiteral("SELESAI"), 0xe3f0e5, 0x2f7d32};
        }
        break;
    case CanvasNodeKind::Step:
        kind.label = node.output == CanvasStepOutput::Tasks ? QStringLiteral("LANGKAH AI → TASK")
                                                            : QStringLiteral("LANGKAH AI");
        kind.brief = node.output == CanvasStepOutput::Tasks ? QStringLiteral("AI → TASK") : QStringLiteral("AI");
        kind.band = kNavy;
        kind.ink = 0xffffff;
        // Pitanya sendiri navy: status berjalan memakai warna permukaan supaya tetap terlihat
        if (state == RunState::Running) {
            kind.status = {QStringLiteral("BERJALAN"), kSurface, kNavy};
        } else if (state == RunState::Queued) {
            kind.status = {QStringLiteral("ANTRE"), 0xe3ebf6, 0x2f4b73};
        } else if (node.hasResult()) {
            kind.status = node.result.success ? Chip{QStringLiteral("SELESAI"), 0xe3f0e5, 0x2f7d32}
                                              : Chip{QStringLiteral("GAGAL"), 0xf6e0db, 0x9b2c1f};
        }
        break;
    case CanvasNodeKind::Note:
        break;
    }
    if (node.isReference() && !node.available) {
        kind.status = {QStringLiteral("TIDAK TERSEDIA"), 0xefe9e1, 0x8a7f70};
    }
    return kind;
}

// Arah tegak lurus sisi itu, menjauhi kartunya
QPointF sideNormal(CanvasSide side) {
    switch (side) {
    case CanvasSide::Left: return QPointF(-1, 0);
    case CanvasSide::Top: return QPointF(0, -1);
    case CanvasSide::Right: return QPointF(1, 0);
    case CanvasSide::Bottom: break;
    }
    return QPointF(0, 1);
}

// Titik sambung: 5 px di kanvas, tetapi di layar tidak lebih kecil dari 4,5 px dan tidak lebih besar dari 6,5 px
qreal portRadius(qreal scale) {
    return std::clamp(kPortRadius * scale, 4.5, 6.5) / scale;
}

// Mata panah: 10 px di kanvas. Saat diperkecil ia tetap ±6,5 px di layar supaya arah garisnya
// terbaca, tetapi tidak pernah lebih dari sepertiga jarak kedua titik tempelnya.
qreal arrowLength(qreal scale, qreal span) {
    return qMax(1.0, qMin(qMax(kArrowLength, 6.5 / scale), span * 0.35));
}

// Daerah klik garis: 12 px di kanvas, paling sedikit 10 px di layar
qreal edgeHitWidth(qreal scale) {
    return qMax(12.0, 10.0 / scale);
}

// Skala untuk menghitung daerah lukis item. Bagian yang berukuran px layar (titik sambung, mata
// panah, garis tepi) melebar di kanvas saat diperkecil, tetapi daerah lukis yang berubah di setiap
// langkah zoom berarti indeks scene diperbarui untuk semua item di setiap langkah. Skalanya
// dibulatkan ke bawah ke kelipatan 1,5×: daerahnya sedikit lebih luas dari perlu dan hanya berubah
// beberapa kali di sepanjang rentang zoom.
qreal boundsScale(qreal scale) {
    if (scale >= 1.0) {
        return 1.0;
    }
    static const qreal step = std::log(1.5);
    return std::exp(std::floor(std::log(scale) / step) * step);
}

// Bagian atas kartu setinggi `height` (paling sedikit sebesar radius): dua pojok atasnya ikut membulat
QPainterPath bandPath(const QRectF &card, qreal radius, qreal height) {
    const qreal diameter = 2 * radius;
    QPainterPath path;
    path.moveTo(card.left(), card.top() + height);
    path.lineTo(card.left(), card.top() + radius);
    path.arcTo(QRectF(card.left(), card.top(), diameter, diameter), 180, -90);
    path.lineTo(card.right() - radius, card.top());
    path.arcTo(QRectF(card.right() - diameter, card.top(), diameter, diameter), 90, -90);
    path.lineTo(card.right(), card.top() + height);
    path.closeSubpath();
    return path;
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

QPointF canvasPort(const QRectF &rect, CanvasSide side) {
    switch (side) {
    case CanvasSide::Left: return QPointF(rect.left(), rect.center().y());
    case CanvasSide::Top: return QPointF(rect.center().x(), rect.top());
    case CanvasSide::Right: return QPointF(rect.right(), rect.center().y());
    case CanvasSide::Bottom: break;
    }
    return QPointF(rect.center().x(), rect.bottom());
}

CanvasRoute canvasRoute(const QRectF &from, const QRectF &to, const std::function<bool(const QRectF &lane)> &occupied) {
    const QPointF a = from.center();
    const QPointF b = to.center();
    const bool right = b.x() >= a.x();
    const bool below = b.y() >= a.y();
    const CanvasRoute across{right ? CanvasSide::Right : CanvasSide::Left, right ? CanvasSide::Left : CanvasSide::Right};
    const CanvasRoute upright{below ? CanvasSide::Bottom : CanvasSide::Top, below ? CanvasSide::Top : CanvasSide::Bottom};
    // Celah antara kedua kartu di tiap arah; nol atau negatif = keduanya bertindihan di arah itu
    const qreal gapX = right ? to.left() - from.right() : from.left() - to.right();
    const qreal gapY = below ? to.top() - from.bottom() : from.top() - to.bottom();
    if (gapX > 0.0 && gapY <= 0.0) {
        return across;
    }
    if (gapY > 0.0 && gapX <= 0.0) {
        return upright;
    }
    const qreal shiftX = qAbs(b.x() - a.x());
    const qreal shiftY = qAbs(b.y() - a.y());
    if (gapX <= 0.0) {
        // Kartunya saling menimpa: ikut arah pergeseran yang lebih besar dibanding ukuran kartunya
        return shiftX * (from.height() + to.height()) >= shiftY * (from.width() + to.width()) ? across : upright;
    }
    // Serong: lorong mendatar ada di antara sisi kiri/kanan kedua kartu, lorong tegak di antara sisi
    // atas/bawahnya; masing-masing selebar rentang kedua titik tempelnya
    const QRectF laneAcross(QPointF(right ? from.right() : to.right(), qMin(a.y(), b.y()) - kLaneMargin),
                            QPointF(right ? to.left() : from.left(), qMax(a.y(), b.y()) + kLaneMargin));
    const QRectF laneUpright(QPointF(qMin(a.x(), b.x()) - kLaneMargin, below ? from.bottom() : to.bottom()),
                             QPointF(qMax(a.x(), b.x()) + kLaneMargin, below ? to.top() : from.top()));
    const bool acrossTaken = occupied && occupied(laneAcross);
    const bool uprightTaken = occupied && occupied(laneUpright);
    if (acrossTaken != uprightTaken) {
        return acrossTaken ? upright : across;
    }
    // Lebih lurus = bergeser ke samping lebih sedikit dibanding jarak majunya
    return shiftY * gapY <= shiftX * gapX ? across : upright;
}

QPainterPath canvasConnectorPath(const QPointF &from, CanvasSide fromSide, const QPointF &to, CanvasSide toSide) {
    const QPointF out = sideNormal(fromSide);
    const QPointF in = sideNormal(toSide);
    const QPointF delta = to - from;
    // ahead: sejauh apa ujung lainnya di depan sisi ini; across: sejauh apa ia bergeser ke samping
    auto reach = [](qreal ahead, qreal across) {
        if (ahead >= 0.0) {
            // Paling jauh sampai sejajar ujung lainnya, supaya lengkungnya tidak keluar dari lorongnya
            return qMax(ahead * 0.5, qMin(ahead, kReach));
        }
        // Ujung lainnya di belakang sisi ini (kartu bertindihan): memutar secukupnya
        return qMin(2 * kReach, kReach * 0.5 + (qAbs(ahead) + qAbs(across)) * 0.25);
    };
    const qreal aheadFrom = QPointF::dotProduct(delta, out);
    const qreal aheadTo = -QPointF::dotProduct(delta, in);
    const qreal acrossFrom = QPointF::dotProduct(delta, QPointF(-out.y(), out.x()));
    const qreal acrossTo = QPointF::dotProduct(delta, QPointF(-in.y(), in.x()));
    QPainterPath path(from);
    path.cubicTo(from + out * reach(aheadFrom, acrossFrom), to + in * reach(aheadTo, acrossTo), to);
    return path;
}

bool canvasLaneOccupied(const QGraphicsScene *scene, const QRectF &lane, const CanvasNodeItem *from,
                        const CanvasNodeItem *to) {
    const QList<QGraphicsItem *> items = scene->items(lane, Qt::IntersectsItemBoundingRect);
    for (const QGraphicsItem *item : items) {
        const auto *node = qgraphicsitem_cast<const CanvasNodeItem *>(item);
        if (node && node != from && node != to && node->sceneCardRect().intersects(lane)) {
            return true;
        }
    }
    return false;
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

// Hasil menata isi kartu untuk satu ukuran kartu dan satu skala tampilan
struct CanvasNodeItem::Layout {
    // Kunci: tata letak dihitung ulang hanya bila salah satunya berubah
    int revision = -1;
    QSizeF size;
    qreal unit = 0.0;
    bool summary = false;
    QFont base;

    qreal pad = kPad;
    qreal radius = kRadius;
    qreal band = 0.0;     // tinggi pita jenis kartu; catatan tidak punya
    bool label = false;   // pitanya setinggi penuh: memuat nama jenis dan status
    QFont labelFont;
    TextFlow title;
    QPointF titlePos;
    QString meta;         // kosong = tidak tampil
    QFont metaFont;
    QRectF metaRect;
    TextFlow body;
    QPointF bodyPos;
    QRectF image;         // tempat gambar pratinjau kartu foto
};

CanvasNodeItem::CanvasNodeItem(const CanvasNode &node)
    : m_node(node), m_size(node.size), m_layout(std::make_unique<Layout>()) {
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

void CanvasNodeItem::setViewScale(qreal scale) {
    m_viewScale = qMax(scale, 0.01);
    const qreal bounds = boundsScale(m_viewScale);
    if (bounds != m_boundsScale) {
        prepareGeometryChange();
        m_boundsScale = bounds;
    }
}

bool CanvasNodeItem::wantsThumbnail() const {
    return CanvasWorkflow::isImageReference(m_node);
}

void CanvasNodeItem::setThumbnail(const QImage &image) {
    m_thumbnail = image;
    update();
}

QPointF CanvasNodeItem::port(CanvasSide side) const {
    return canvasPort(sceneCardRect(), side);
}

bool CanvasNodeItem::hitsPort(const QPointF &scenePos, qreal tolerance) const {
    // Pojok kanan bawah milik pegangan ubah ukuran, juga bila daerah titik sambung sampai ke sana
    if (hitsResizeHandle(mapFromScene(scenePos))) {
        return false;
    }
    // Di kartu yang kecil di layar daerah titiknya ikut mengecil, supaya kartunya masih bisa digeser
    const qreal reach = qMin(qMax(tolerance, kPortRadius + 2), 0.22 * qMin(m_size.width(), m_size.height()));
    return std::any_of(std::begin(kSides), std::end(kSides), [&](CanvasSide side) {
        const QPointF delta = scenePos - port(side);
        return std::hypot(delta.x(), delta.y()) <= reach;
    });
}

void CanvasNodeItem::addEdge(CanvasEdgeItem *edge) {
    if (!m_edges.contains(edge)) {
        m_edges.append(edge);
        ++m_revision;   // langkah AI menampilkan jumlah bahannya
        update();
    }
}

void CanvasNodeItem::removeEdge(CanvasEdgeItem *edge) {
    m_edges.removeAll(edge);
    ++m_revision;
    update();
}

QFont CanvasNodeItem::baseFont() const {
    if (scene() && !scene()->views().isEmpty()) {
        return scene()->views().first()->viewport()->font();
    }
    return QApplication::font();
}

void CanvasNodeItem::beginEdit() {
    if (m_editor || m_node.kind != CanvasNodeKind::Note) {
        return;
    }
    auto *editor = new CanvasTextEditor(this);
    editor->setFont(sized(baseFont(), kNoteFontPx));
    editor->setDefaultTextColor(Theme::text(kBody));
    editor->setPlainText(m_node.text);
    editor->setTextWidth(m_size.width() - 2 * kPad);
    editor->setPos(kPad, kPad);
    editor->finished = [this]() { finishEdit(); };
    // Kartu ikut memanjang selagi diketik, supaya teksnya tidak keluar dari kertas
    auto growToFit = [this]() {
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
    };
    connect(editor->document(), &QTextDocument::contentsChanged, this, growToFit);
    m_editor = editor;
    // Catatan yang isinya sudah lebih panjang dari kertasnya dibuka seutuhnya; setelah selesai diedit
    // kartunya memang disimpan sebesar isinya
    growToFit();
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

qreal CanvasNodeItem::resizeHandleSide() const {
    // 14 px di layar, tetapi tidak lebih dari sepertiga sisi kartu: kartu yang kecil di layar tetap
    // punya badan untuk digeser
    return qMin(qMax(kHandle, kHandle / m_viewScale), 0.3 * qMin(m_size.width(), m_size.height()));
}

bool CanvasNodeItem::hitsResizeHandle(const QPointF &localPos) const {
    const qreal side = resizeHandleSide();
    return QRectF(m_size.width() - side, m_size.height() - side, side, side).contains(localPos);
}

void CanvasNodeItem::refreshTexts() {
    ++m_revision;
    m_heading.clear();
    m_meta.clear();
    m_preview.clear();
    m_summary.clear();
    // Pratinjau cukup sepanjang yang bisa dimuat kartu sebesar ini
    const int budget = std::clamp(int(m_size.width() * m_size.height() / 40.0), 900, 24000);
    switch (m_node.kind) {
    case CanvasNodeKind::Note: {
        m_preview = m_node.text;
        const QString first = CanvasWorkflow::firstLine(m_node.text);
        if (!first.isEmpty() && first.size() <= kHeadingMaxChars) {
            m_heading = first;
            m_summary = previewOf(afterHeading(m_node.text), budget);
        } else {
            // Paragraf pembuka yang panjang bukan judul: seluruhnya menjadi isi ringkasan
            m_summary = previewOf(m_node.text, budget);
        }
        break;
    }
    case CanvasNodeKind::Artifact:
    case CanvasNodeKind::Task:
        m_heading = m_node.title;
        m_meta = m_node.detail.isEmpty() ? m_node.source.projectId
                                         : QStringLiteral("%1 · %2").arg(m_node.detail, m_node.source.projectId);
        if (!CanvasWorkflow::isImageReference(m_node)) {
            m_preview = previewOf(m_node.text, budget);
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
            m_preview = previewOf(m_node.result.message, budget);
        } else if (m_node.hasResult()) {
            m_preview = QStringLiteral("Gagal (%1): %2").arg(m_node.result.outcome, m_node.result.message.left(400));
        } else {
            const QString rest = restOf(m_node.text);
            m_preview = !rest.isEmpty() ? rest
                                        : QStringLiteral("Sambungkan kartu bahan ke kartu ini, lalu jalankan ▶.");
        }
        break;
    }
    }
}

const CanvasNodeItem::Layout &CanvasNodeItem::layoutFor(qreal scale, const QFont &base) const {
    const qreal unit = layoutUnit(scale);
    const bool summary = m_node.kind == CanvasNodeKind::Note && scale < kNoteSummaryScale;
    Layout &layout = *m_layout;
    if (layout.revision == m_revision && layout.size == m_size && layout.unit == unit && layout.summary == summary
        && layout.base == base) {
        return layout;
    }
    layout = Layout();
    layout.revision = m_revision;
    layout.size = m_size;
    layout.unit = unit;
    layout.summary = summary;
    layout.base = base;
    layout.pad = atLeast(kPad, 5.0, unit);
    layout.radius = qMin(atLeast(kRadius, 3.0, unit), qMin(m_size.width(), m_size.height()) / 2);
    if (m_node.kind == CanvasNodeKind::Note) {
        layoutNote(layout);
    } else {
        layoutCard(layout);
    }
    return layout;
}

void CanvasNodeItem::layoutNote(Layout &layout) const {
    const qreal unit = layout.unit;
    const bool empty = m_node.text.trimmed().isEmpty();
    const QString hint = QStringLiteral("Klik dua kali untuk menulis…");
    if (!layout.summary) {
        // Teks apa adanya dengan huruf dan jarak tepi editornya, supaya tidak bergeser saat mulai diedit
        QFont font = sized(layout.base, kNoteFontPx);
        font.setItalic(empty);
        layout.body = flowText(empty ? hint : m_preview, font, m_size.width() - 2 * kPad, m_size.height() - 2 * kPad);
        layout.bodyPos = QPointF(kPad, kPad);
        return;
    }
    // Diperkecil: baris pertama menjadi judul tebal dan sisanya pratinjau, keduanya berhuruf terbaca
    const qreal inner = m_size.width() - 2 * layout.pad;
    qreal top = layout.pad;
    qreal bottom = m_size.height() - layout.pad;
    QFont bodyFont = sized(layout.base, typePx(kNoteType, unit));
    if (empty) {
        bodyFont.setItalic(true);
        layout.body = flowText(hint, bodyFont, inner, bottom - top);
        layout.bodyPos = QPointF(layout.pad, top);
        return;
    }
    if (!m_heading.isEmpty()) {
        const QFont titleFont = sized(layout.base, typePx(kTitleType, unit), true);
        layout.title = flowText(m_heading, titleFont, inner, bottom - top, 3);
        if (layout.title.truncated || layout.title.brokenWord) {
            // Judulnya tidak muat: jarak tepi atas-bawah dirapatkan dan hurufnya mengecil
            top = atLeast(5.0, 2.0, unit);
            bottom = m_size.height() - top;
            layout.title = fitTitle(m_heading, titleFont, qMax(9, qRound(kTitleShrinkFloor * unit)), inner, bottom - top);
        }
        layout.titlePos = QPointF(layout.pad, top);
        if (!layout.title.isEmpty()) {
            top += layout.title.height + atLeast(6.0, 3.0, unit);
        }
    }
    if (bottom - top >= lineHeightOf(bodyFont)) {
        layout.body = flowText(m_summary, bodyFont, inner, bottom - top, 0, 0.5);
        layout.bodyPos = QPointF(layout.pad, top);
    }
}

void CanvasNodeItem::layoutCard(Layout &layout) const {
    const qreal unit = layout.unit;
    const qreal height = m_size.height();
    const qreal inner = m_size.width() - 2 * layout.pad;

    layout.labelFont = sized(layout.base, typePx(kLabelType, unit), true);
    layout.labelFont.setLetterSpacing(QFont::PercentageSpacing, 104);
    const QFont titleFont = sized(layout.base, typePx(kTitleType, unit), true);
    const qreal fullBand = lineHeightOf(layout.labelFont) + 2 * atLeast(7.0, 4.0, unit);
    const qreal strip = qMin(fullBand, qMax(layout.radius, 3.5 * unit));
    const qreal gap = atLeast(9.0, 4.0, unit);
    const qreal foot = atLeast(11.0, 4.5, unit);

    TextFlow title = flowText(m_heading, titleFont, inner, -1.0, 3);
    const qreal titleLine = title.lineHeight;
    const int wanted = qMax(1, int(title.lines.size()));
    // Pita mengalah lebih dulu: dua baris judul lebih penting daripada nama jenis kartunya. Pita yang
    // tidak memuat nama jenis tinggal garis warna, bukan pita kosong setengah tinggi.
    layout.label = height - (fullBand + gap + foot) >= qMin(wanted, 2) * titleLine;
    layout.band = layout.label ? fullBand : strip;
    qreal y = layout.band + gap;
    qreal room = height - foot - y;
    if (!layout.label && (title.truncated || title.brokenWord || wanted * titleLine > room + 0.5)) {
        // Kartu kecil di layar: pitanya tinggal garis warna, jarak tepinya dirapatkan, dan judul mengecil
        layout.band = strip;
        const qreal top = strip + atLeast(4.0, 2.0, unit);
        const qreal space = height - atLeast(5.0, 2.5, unit) - top;
        const int smallPx = qMax(9, qRound(kTitleShrinkFloor * unit));
        if (!title.truncated && !title.brokenWord && wanted * titleLine <= space + 0.5) {
            // Dengan jarak tepi yang rapat judulnya muat tanpa mengecil
            layout.title = title;
        } else if (title.truncated && space < 4 * titleLine) {
            // Butuh lebih dari tiga baris huruf biasa dan ruangnya tidak sebanyak itu: langsung huruf kecil
            layout.title = flowText(m_heading, sized(titleFont, smallPx, true), inner, space);
        } else {
            layout.title = fitTitle(m_heading, titleFont, smallPx, inner, space);
        }
        layout.titlePos = QPointF(layout.pad, top);
        return;
    }
    if (wanted * titleLine > room + 0.5) {
        title = flowText(m_heading, titleFont, inner, room, 3);
    }
    layout.title = title;
    layout.titlePos = QPointF(layout.pad, y);
    y += title.height;
    room = height - foot - y;

    QString meta = m_meta;
    if (m_node.kind == CanvasNodeKind::Step) {
        const qsizetype inputs = std::count_if(m_edges.cbegin(), m_edges.cend(),
                                               [this](const CanvasEdgeItem *edge) { return edge->to() == this; });
        meta = QStringLiteral("%1 bahan · %2").arg(inputs).arg(m_meta);
    }
    layout.metaFont = sized(layout.base, typePx(kMetaType, unit));
    const qreal metaGap = atLeast(2.0, 1.0, unit);
    const qreal metaLine = lineHeightOf(layout.metaFont);
    if (!meta.isEmpty() && room >= metaGap + metaLine) {
        layout.meta = QFontMetricsF(layout.metaFont).elidedText(meta, Qt::ElideRight, inner);
        layout.metaRect = QRectF(layout.pad, y + metaGap, inner, metaLine);
        y += metaGap + metaLine;
        room = height - foot - y;
    }

    const qreal bodyGap = atLeast(7.0, 3.0, unit);
    const QFont bodyFont = sized(layout.base, typePx(kBodyType, unit));
    if (wantsThumbnail()) {
        if (room - bodyGap >= atLeast(24.0, 20.0, unit)) {
            layout.image = QRectF(layout.pad, y + bodyGap, inner, room - bodyGap);
        }
    } else if (room - bodyGap >= lineHeightOf(bodyFont)) {
        layout.body = flowText(m_preview, bodyFont, inner, room - bodyGap, 0, 0.5);
        layout.bodyPos = QPointF(layout.pad, y + bodyGap);
    }
}

QStringList CanvasNodeItem::visibleText(qreal scale) const {
    const Layout &layout = layoutFor(scale, baseFont());
    QStringList lines;
    for (const TextFlow::Line &line : layout.title.lines) {
        lines.append(line.text);
    }
    if (!layout.meta.isEmpty()) {
        lines.append(layout.meta);
    }
    for (const TextFlow::Line &line : layout.body.lines) {
        lines.append(line.text);
    }
    return lines;
}

QRectF CanvasNodeItem::boundingRect() const {
    // Titik sambung dan garis tepi kosmetik keluar sedikit dari kartu; bayangannya jatuh ke bawah
    const qreal handles = portRadius(m_boundsScale) + 3.0 / m_boundsScale;
    const qreal side = qMax(4.0, handles);
    return cardRect().adjusted(-side, -side, side, qMax(9.0, handles));
}

QPainterPath CanvasNodeItem::shape() const {
    QPainterPath path;
    path.setFillRule(Qt::WindingFill);
    path.addRoundedRect(cardRect(), kRadius, kRadius);
    // Titik sambung menonjol sedikit dari tepi: lebih kecil dari jarak antar kartu yang berjajar,
    // jadi tonjolannya tidak menutupi kartu tetangga
    for (CanvasSide side : kSides) {
        path.addEllipse(canvasPort(cardRect(), side), kPortRadius + 4, kPortRadius + 4);
    }
    return path;
}

void CanvasNodeItem::paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget) {
    const qreal scale = qMax(0.01, option->levelOfDetailFromTransform(painter->worldTransform()));
    const Layout &layout = layoutFor(scale, widget ? widget->font() : baseFont());
    const QRectF card = cardRect().adjusted(0.5, 0.5, -0.5, -0.5);
    const qreal radius = layout.radius;
    const bool note = m_node.kind == CanvasNodeKind::Note;
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setRenderHint(QPainter::TextAntialiasing);

    // Bayangan lembut berlapis. Diperkecil jauh ia tinggal sepersekian piksel, jadi dilewati.
    if (scale >= 0.3) {
        static const struct {
            qreal grow;
            qreal drop;
            int alpha;
        } layers[] = {{3.0, 5.0, 5}, {1.5, 2.5, 8}, {0.0, 1.0, 12}};
        const int strength = Theme::isDark() ? 3 : 1;
        painter->setPen(Qt::NoPen);
        for (const auto &layer : layers) {
            painter->setBrush(QColor(0x24, 0x20, 0x1b, layer.alpha * strength));
            painter->drawRoundedRect(card.adjusted(-layer.grow, layer.drop - layer.grow, layer.grow, layer.drop + layer.grow),
                                     radius + layer.grow, radius + layer.grow);
        }
    }

    const QColor surface = note ? CanvasPalette::noteFill(m_node.color) : Theme::fill(kSurface);
    painter->setPen(Qt::NoPen);
    painter->setBrush(surface);
    painter->drawRoundedRect(card, radius, radius);
    if (!note) {
        painter->setBrush(Theme::fill(kindOf(m_node, m_runState).band));
        painter->drawPath(bandPath(card, radius, layout.band));
    }

    // Garis tepi catatan senada dengan kertasnya; kartu lain memakai garis krem yang sama
    QColor border = note ? (Theme::isDark() ? surface.lighter(130) : surface.darker(109)) : Theme::fill(kBorder);
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
    painter->setBrush(Qt::NoBrush);
    if (isSelected()) {
        border = Theme::fill(kAccent);
        width = 2.0;
        style = Qt::SolidLine;
        // Pendar tipis di luar garis pilihan
        QColor glow = border;
        glow.setAlpha(60);
        QPen halo(glow, 5.0);
        halo.setCosmetic(true);
        painter->setPen(halo);
        painter->drawRoundedRect(card, radius, radius);
    } else if (m_hovered) {
        border = Theme::fill(kAccent);
    }
    QPen pen(border, width, style);
    pen.setCosmetic(true);
    painter->setPen(pen);
    painter->drawRoundedRect(card, radius, radius);

    if (m_runState == RunState::Running) {
        QColor glow = Theme::text(kNavy);
        glow.setAlphaF(0.12 * m_pulse);
        QColor line = Theme::text(kNavy);
        line.setAlphaF(0.25 + 0.75 * m_pulse);
        QPen pulsePen(line, 2.0);
        pulsePen.setCosmetic(true);
        painter->setPen(pulsePen);
        painter->setBrush(glow);
        painter->drawRoundedRect(card.adjusted(0.5, 0.5, -0.5, -0.5), radius - 0.5, radius - 0.5);
    }

    if (note) {
        paintNote(painter, layout);
    } else {
        paintCard(painter, layout);
    }
    if (isSelected() || m_hovered) {
        paintHandles(painter, scale);
    }
}

void CanvasNodeItem::paintNote(QPainter *painter, const Layout &layout) {
    if (m_editor) {
        return;
    }
    const bool empty = m_node.text.trimmed().isEmpty();
    drawFlow(painter, layout.titlePos, layout.title, Theme::text(kBody));
    drawFlow(painter, layout.bodyPos, layout.body, Theme::text(empty ? 0x8a7f70 : kBody));
}

void CanvasNodeItem::paintCard(QPainter *painter, const Layout &layout) {
    if (layout.label) {
        const CardKind kind = kindOf(m_node, m_runState);
        const QFontMetricsF metrics(layout.labelFont);
        qreal right = m_size.width() - layout.pad;
        if (!kind.status.text.isEmpty()) {
            const qreal chipHeight = metrics.height() + 2 * atLeast(1.5, 1.0, layout.unit);
            const qreal chipWidth = metrics.horizontalAdvance(kind.status.text) + chipHeight * 0.9;
            const QRectF chip(right - chipWidth, (layout.band - chipHeight) / 2, chipWidth, chipHeight);
            painter->setPen(Qt::NoPen);
            painter->setBrush(Theme::fill(kind.status.background));
            painter->drawRoundedRect(chip, chipHeight * 0.3, chipHeight * 0.3);
            painter->setPen(Theme::text(kind.status.foreground));
            painter->setFont(layout.labelFont);
            painter->drawText(chip, Qt::AlignCenter, kind.status.text);
            right = chip.left() - chipHeight * 0.5;
        }
        // Nama jenis yang tidak muat di samping statusnya disingkat; kalau tetap tidak muat dilewati
        // (warna pitanya sudah menunjukkan jenis kartu), tidak dipotong menjadi "LANG…"
        const qreal space = right - layout.pad;
        const QString label = metrics.horizontalAdvance(kind.label) <= space ? kind.label : kind.brief;
        if (!label.isEmpty() && metrics.horizontalAdvance(label) <= space) {
            painter->setFont(layout.labelFont);
            painter->setPen(Theme::text(kind.ink));
            painter->drawText(QRectF(layout.pad, 0, space, layout.band), Qt::AlignLeft | Qt::AlignVCenter, label);
        }
    }
    drawFlow(painter, layout.titlePos, layout.title, Theme::text(kNavy));
    if (!layout.meta.isEmpty()) {
        painter->setFont(layout.metaFont);
        painter->setPen(Theme::text(kMuted));
        painter->drawText(layout.metaRect, Qt::AlignLeft | Qt::AlignVCenter, layout.meta);
    }
    if (!layout.image.isEmpty() && !m_thumbnail.isNull()) {
        const QSizeF fitted = QSizeF(m_thumbnail.size()).scaled(layout.image.size(), Qt::KeepAspectRatio);
        const QRectF target(layout.image.left() + (layout.image.width() - fitted.width()) / 2, layout.image.top(),
                            fitted.width(), fitted.height());
        QPainterPath clip;
        clip.addRoundedRect(target, 6, 6);
        painter->save();
        painter->setClipPath(clip, Qt::IntersectClip);
        painter->setRenderHint(QPainter::SmoothPixmapTransform);
        painter->drawImage(target, m_thumbnail);
        painter->restore();
    }
    const bool failed = m_node.kind == CanvasNodeKind::Step && m_node.hasResult() && !m_node.result.success
                        && m_runState == RunState::Idle;
    drawFlow(painter, layout.bodyPos, layout.body, Theme::text(failed ? 0x9b2c1f : kBody));
}

void CanvasNodeItem::paintHandles(QPainter *painter, qreal scale) {
    // Empat titik sambung, satu di tengah tiap sisi. Ukurannya tetap di layar saat diperkecil, kecuali
    // di kartu yang sudah sangat kecil: di sana ikut mengecil seperti daerah kliknya (hitsPort).
    QPen portPen(Theme::fill(kAccent), 1.5);
    portPen.setCosmetic(true);
    painter->setPen(portPen);
    painter->setBrush(Theme::fill(kSurface));
    const qreal radius = qMin(portRadius(scale), 0.18 * qMin(m_size.width(), m_size.height()));
    for (CanvasSide side : kSides) {
        painter->drawEllipse(canvasPort(cardRect(), side), radius, radius);
    }

    // Pegangan ubah ukuran di pojok kanan bawah, sebesar daerah kliknya
    QPen grip(Theme::text(kMuted), 1.0);
    grip.setCosmetic(true);
    painter->setPen(grip);
    const qreal grain = resizeHandleSide() / kHandle;
    const QPointF corner(m_size.width() - 4 * grain, m_size.height() - 4 * grain);
    for (int i = 1; i <= 3; ++i) {
        const qreal d = 3.0 * grain * i;
        painter->drawLine(corner - QPointF(d, 0), corner - QPointF(0, d));
    }
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
    } else if (hitsPort(event->scenePos(), 9.0 / m_viewScale)) {
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
    emit geometryChanged();
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
    const QRectF from = m_from->sceneCardRect();
    const QRectF to = m_to->sceneCardRect();
    const QGraphicsScene *canvas = scene();
    const CanvasRoute route = canvasRoute(from, to, [this, canvas](const QRectF &lane) {
        return canvas && canvasLaneOccupied(canvas, lane, m_from, m_to);
    });
    const QPointF start = canvasPort(from, route.from);
    const QPointF tip = canvasPort(to, route.to);
    if (!m_path.isEmpty() && route == m_route && start == m_start && tip == m_tip) {
        return;
    }
    prepareGeometryChange();
    m_route = route;
    m_start = start;
    m_tip = tip;
    rebuild();
}

void CanvasEdgeItem::setViewScale(qreal scale) {
    scale = qMax(scale, 0.01);
    if (qFuzzyCompare(scale, m_viewScale)) {
        return;
    }
    const qreal bounds = boundsScale(scale);
    if (bounds != m_boundsScale) {
        prepareGeometryChange();
        m_boundsScale = bounds;
    }
    m_viewScale = scale;
    rebuild();
    update();
}

void CanvasEdgeItem::rebuild() {
    const qreal span = QLineF(m_start, m_tip).length();
    const QPointF away = sideNormal(m_route.to);
    const QPointF across(-away.y(), away.x());
    auto arrowAt = [&](qreal length) {
        const QPointF base = m_tip + away * length;
        return QPolygonF({m_tip, base + across * (length * 0.42), base - across * (length * 0.42)});
    };
    // Garis berhenti sedikit di dalam pangkal panah supaya ujungnya tidak menembus mata panah
    auto lineTo = [&](qreal length) {
        return canvasConnectorPath(m_start, m_route.from, m_tip + away * (length * 0.9), m_route.to);
    };
    const qreal length = arrowLength(m_viewScale, span);
    m_arrow = arrowAt(length);
    m_path = lineTo(length);
    m_shape = QPainterPath();

    // Daerah lukis berlaku untuk seluruh rentang zoom yang dibulatkan ke m_boundsScale: mata panahnya
    // paling panjang di sana, dan garisnya ada di antara garis tanpa panah dan garis berpanah terpanjang itu
    const qreal longest = arrowLength(m_boundsScale, span);
    const qreal margin = edgeHitWidth(m_boundsScale) / 2 + 2.0 / m_boundsScale;
    m_bounds = lineTo(0.0).controlPointRect().united(lineTo(longest).controlPointRect())
                   .united(arrowAt(longest).boundingRect()).adjusted(-margin, -margin, margin, margin);
}

QRectF CanvasEdgeItem::boundingRect() const {
    return m_bounds;
}

QPainterPath CanvasEdgeItem::shape() const {
    // Dihitung saat pertama kali ditanyakan sesudah garisnya berubah: selama kartu diseret atau kanvas
    // di-zoom semua garis berubah, tetapi hanya yang di bawah kursor yang ditanyai bentuknya
    if (m_shape.isEmpty()) {
        QPainterPathStroker stroker;
        stroker.setWidth(edgeHitWidth(m_viewScale));
        stroker.setCapStyle(Qt::FlatCap);
        QPainterPath arrow;
        arrow.addPolygon(m_arrow);
        arrow.closeSubpath();
        // Disatukan sungguhan: dua bentuk yang sekadar ditumpuk saling meniadakan di tempat bertemunya,
        // dan pangkal mata panah jadi tidak bisa diklik
        m_shape = stroker.createStroke(m_path).united(arrow);
    }
    return m_shape;
}

void CanvasEdgeItem::paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget) {
    Q_UNUSED(widget);
    const qreal scale = option->levelOfDetailFromTransform(painter->worldTransform());
    // Garis yang masuk ke langkah AI = bahan untuk agent: navy; garis lain sekadar keterkaitan ide
    const bool feedsStep = m_to->node().kind == CanvasNodeKind::Step;
    QColor color = feedsStep ? Theme::fill(kNavy) : Theme::fill(0xb3a898);
    if (isSelected() || m_hovered) {
        color = Theme::fill(kAccent);
    }
    painter->setRenderHint(QPainter::Antialiasing);
    // Tebalnya dalam px layar; menipis saat diperkecil supaya garis yang berjajar rapat tidak menggumpal
    const qreal width = 1.1 + 0.5 * std::clamp((scale - 0.25) / 0.45, 0.0, 1.0) + (isSelected() ? 0.9 : 0.0);
    QPen pen(color, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    pen.setCosmetic(true);
    painter->setPen(pen);
    painter->setBrush(Qt::NoBrush);
    painter->drawPath(m_path);
    // Mata panah bersudut tumpul: isiannya digaris tipis dengan sambungan bulat
    pen.setWidthF(1.0);
    painter->setPen(pen);
    painter->setBrush(color);
    painter->drawPolygon(m_arrow);
}

QVariant CanvasEdgeItem::itemChange(GraphicsItemChange change, const QVariant &value) {
    // Baru setelah berada di scene garis ini tahu kartu lain di sekitar kedua kartunya
    if (change == ItemSceneHasChanged && scene()) {
        updatePath();
    }
    return QGraphicsItem::itemChange(change, value);
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

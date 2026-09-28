#include "DocumentText.h"

#include <QDate>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QList>
#include <QSet>
#include <QStringDecoder>
#include <QTime>
#include <QXmlStreamReader>

// .xlsx dan .docx adalah arsip ZIP. Qt tidak punya pembaca ZIP publik; QZipReader (QtCore
// privat, diekspor) sudah dipakai Qt sendiri untuk dokumen Office dan stabil sejak Qt 4.
#include <QtCore/private/qzipreader_p.h>

#include <cmath>
#include <optional>

namespace {

// Batas keluaran satu dokumen supaya lembar kerja raksasa tidak menghabiskan memori.
// Prompt sendiri memakai batas yang jauh lebih kecil (lihat TaskAttachments).
constexpr qsizetype kMaxTextChars = 2 * 1024 * 1024;
// Satu bagian arsip yang lebih besar dari ini tidak dibuka (mis. ZIP yang sengaja dirusak)
constexpr qint64 kMaxPartBytes = 256LL * 1024 * 1024;
// Kolom terakhir Excel (XFD)
constexpr int kMaxColumns = 16384;

const QStringList kWordNamespaces = {
    QStringLiteral("http://schemas.openxmlformats.org/wordprocessingml/2006/main"),
    QStringLiteral("http://purl.oclc.org/ooxml/wordprocessingml/main"),   // Strict Open XML
};
const QString kMarkupCompatibility = QStringLiteral("http://schemas.openxmlformats.org/markup-compatibility/2006");

bool fail(QString *error, const QString &why) {
    if (error) {
        *error = why;
    }
    return false;
}

QString suffixOf(const QString &fileName) {
    return QFileInfo(fileName).suffix().toLower();
}

QString truncatedNote() {
    return QStringLiteral("[… sisa dokumen tidak dibaca: lebih dari %1 karakter …]").arg(kMaxTextChars);
}

// Baris kosong beruntun jadi satu; baris kosong di awal dan akhir dibuang
QString joinLines(const QStringList &lines) {
    QStringList result;
    bool previousBlank = true;
    for (const QString &line : lines) {
        const bool blank = line.trimmed().isEmpty();
        if (blank && previousBlank) {
            continue;
        }
        result.append(blank ? QString() : line);
        previousBlank = blank;
    }
    while (!result.isEmpty() && result.last().isEmpty()) {
        result.removeLast();
    }
    return result.join(QLatin1Char('\n'));
}

// ---------- CSV / TSV ----------

QString decodeText(const QByteArray &data) {
    // BOM (UTF-8/16/32) menentukan encoding bila ada; BOM-nya sendiri tidak ikut ke teks
    if (const std::optional<QStringConverter::Encoding> encoding = QStringConverter::encodingForData(data)) {
        QStringDecoder decoder(*encoding);
        return decoder(data);
    }
    QStringDecoder utf8(QStringConverter::Utf8);
    const QString text = utf8(data);
    if (!utf8.hasError()) {
        return text;
    }
    // "CSV" dari Excel (bukan "CSV UTF-8") ditulis dengan code page Windows
    QStringDecoder system(QStringConverter::System);
    return system(data);
}

QString plainText(const QString &path, QString *error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        fail(error, QStringLiteral("File tidak bisa dibuka: %1").arg(file.errorString()));
        return QString();
    }
    const QByteArray data = file.read(kMaxTextChars * 4);   // UTF-8 paling banyak 4 byte per karakter
    bool cut = !file.atEnd();

    QString text = decodeText(data);
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    if (text.size() > kMaxTextChars) {
        const qsizetype lineEnd = text.lastIndexOf(QLatin1Char('\n'), kMaxTextChars);
        text.truncate(lineEnd > 0 ? lineEnd + 1 : kMaxTextChars);
        cut = true;
    }
    if (cut) {
        if (!text.endsWith(QLatin1Char('\n'))) {
            text += QLatin1Char('\n');
        }
        text += truncatedNote();
    }
    return text;
}

// ---------- arsip Office ----------

class OfficeArchive {
public:
    explicit OfficeArchive(const QString &path) : m_zip(path) {
        const QList<QZipReader::FileInfo> entries = m_zip.fileInfoList();
        for (const QZipReader::FileInfo &entry : entries) {
            m_sizes.insert(entry.filePath, entry.size);
        }
    }

    bool has(const QString &part) const { return m_sizes.contains(part); }

    // Kosong bila bagian tidak ada atau terlalu besar
    QByteArray part(const QString &name) const {
        if (m_sizes.value(name, -1) < 0 || m_sizes.value(name) > kMaxPartBytes) {
            return QByteArray();
        }
        return m_zip.fileData(name);
    }

private:
    QZipReader m_zip;
    QHash<QString, qint64> m_sizes;
};

QString mainPart(const QString &suffix) {
    return suffix == QLatin1String("docx") ? QStringLiteral("word/document.xml") : QStringLiteral("xl/workbook.xml");
}

// Alasan arsip Office tidak bisa dibaca; kosong bila baik-baik saja
QString officeProblem(const QString &path, const QString &suffix) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QStringLiteral("File tidak bisa dibuka: %1").arg(file.errorString());
    }
    const QByteArray magic = file.read(4);
    file.close();

    const QString kind = suffix == QLatin1String("docx") ? QStringLiteral("Word") : QStringLiteral("Excel");
    if (magic == QByteArray::fromHex("d0cf11e0")) {
        // Dokumen Office berpassword (dan format lama) disimpan sebagai compound file, bukan ZIP
        return QStringLiteral("File %1 ini berpassword atau berformat lama; simpan ulang tanpa password sebagai .%2")
            .arg(kind, suffix);
    }
    if (magic != QByteArray("PK\x03\x04", 4)) {
        return QStringLiteral("Bukan file %1 yang valid").arg(kind);
    }
    if (!OfficeArchive(path).has(mainPart(suffix))) {
        return QStringLiteral("Bukan file %1 yang valid (bagian %2 tidak ada)").arg(kind, mainPart(suffix));
    }
    return QString();
}

// Atribut berdasarkan nama lokalnya; prefiks namespace (w:, r:) diabaikan
QString attribute(const QXmlStreamReader &xml, QStringView localName) {
    const QXmlStreamAttributes attributes = xml.attributes();
    for (const QXmlStreamAttribute &attribute : attributes) {
        if (attribute.name() == localName) {
            return attribute.value().toString();
        }
    }
    return QString();
}

// ---------- Word ----------

bool isWord(const QXmlStreamReader &xml) {
    return kWordNamespaces.contains(xml.namespaceUri());
}

// styleId -> level judul (1..6). Nama gaya bawaan di styles.xml selalu berbahasa Inggris
// ("heading 1", "Title") walau Word-nya berbahasa lain; gaya buatan sendiri dikenali dari outlineLvl.
QHash<QString, int> headingStyles(const QByteArray &stylesXml) {
    QHash<QString, int> levels;
    QXmlStreamReader xml(stylesXml);
    QString styleId;
    int level = 0;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement() && isWord(xml)) {
            if (xml.name() == u"style") {
                styleId = attribute(xml, u"styleId");
                level = 0;
            } else if (xml.name() == u"name" && !styleId.isEmpty()) {
                const QString name = attribute(xml, u"val").toLower();
                if (name == QLatin1String("title")) {
                    level = 1;
                } else if (name.startsWith(QLatin1String("heading "))) {
                    level = name.mid(8).toInt();
                }
            } else if (xml.name() == u"outlineLvl" && !styleId.isEmpty() && level == 0) {
                bool ok = false;
                const int outline = attribute(xml, u"val").toInt(&ok);
                if (ok && outline >= 0 && outline < 9) {
                    level = outline + 1;
                }
            }
        } else if (xml.isEndElement() && isWord(xml) && xml.name() == u"style") {
            if (level > 0) {
                levels.insert(styleId, qMin(level, 6));
            }
            styleId.clear();
        }
    }
    return levels;
}

struct WordParagraph {
    QString text;
    int heading = 0;       // 1..6; 0 = paragraf biasa
    int listLevel = -1;    // >= 0: butir daftar bertingkat
};

QString renderParagraph(const WordParagraph &paragraph) {
    QStringList lines = paragraph.text.split(QLatin1Char('\n'));
    for (QString &line : lines) {
        while (!line.isEmpty() && line.back().isSpace()) {
            line.chop(1);
        }
    }
    const QString text = lines.join(QLatin1Char('\n'));
    if (text.trimmed().isEmpty()) {
        return QString();
    }
    if (paragraph.heading > 0) {
        return QString(paragraph.heading, QLatin1Char('#')) + QLatin1Char(' ') + text.simplified();
    }
    if (paragraph.listLevel >= 0) {
        return QString(paragraph.listLevel * 2, QLatin1Char(' ')) + QStringLiteral("- ") + text.trimmed();
    }
    return text;
}

// Tabel Markdown; kolom yang kurang di satu baris diisi sel kosong
QStringList renderTable(const QList<QStringList> &rows) {
    qsizetype columns = 0;
    for (const QStringList &row : rows) {
        columns = qMax(columns, row.size());
    }
    QStringList lines;
    if (columns == 0) {
        return lines;
    }
    for (qsizetype r = 0; r < rows.size(); ++r) {
        QStringList cells = rows.at(r);
        cells.resize(columns);
        for (QString &cell : cells) {
            cell = cell.simplified().replace(QLatin1Char('|'), QStringLiteral("\\|"));
        }
        lines.append(QStringLiteral("| %1 |").arg(cells.join(QStringLiteral(" | "))));
        if (r == 0 && rows.size() > 1) {
            lines.append(QStringLiteral("|%1|").arg(QStringList(columns, QStringLiteral(" --- ")).join(QLatin1Char('|'))));
        }
    }
    return lines;
}

QString wordText(const OfficeArchive &archive, QString *error) {
    const QHash<QString, int> headings = headingStyles(archive.part(QStringLiteral("word/styles.xml")));
    QXmlStreamReader xml(archive.part(QStringLiteral("word/document.xml")));

    QStringList lines;
    qsizetype size = 0;
    bool cut = false;
    // Bertumpuk: kotak teks berisi paragraf bisa berada di dalam paragraf, tabel di dalam sel tabel
    QList<WordParagraph> paragraphs;
    QList<QList<QStringList>> tables;   // tabel -> baris -> sel

    // Paragraf/tabel yang selesai dibaca: ke sel tabel yang sedang terbuka, atau ke dokumen
    auto emitBlock = [&](const QStringList &block) {
        if (!tables.isEmpty()) {
            QList<QStringList> &rows = tables.last();
            if (!rows.isEmpty() && !rows.last().isEmpty()) {
                const QString text = block.join(QLatin1Char(' ')).trimmed();
                QString &cell = rows.last().last();
                if (!text.isEmpty()) {
                    cell += (cell.isEmpty() ? QString() : QStringLiteral(" ")) + text;
                }
            }
            return;
        }
        for (const QString &line : block) {
            lines.append(line);
            size += line.size() + 1;
        }
    };

    while (!xml.atEnd() && !cut) {
        xml.readNext();
        if (xml.isStartElement()) {
            // Isi alternatif untuk aplikasi lama: sama dengan mc:Choice, jangan dibaca dua kali
            if (xml.namespaceUri() == kMarkupCompatibility && xml.name() == u"Fallback") {
                xml.skipCurrentElement();
                continue;
            }
            if (!isWord(xml)) {
                continue;
            }
            const QStringView name = xml.name();
            if (name == u"del" || name == u"moveFrom") {
                xml.skipCurrentElement();   // teks yang dihapus / dipindah lewat track changes
            } else if (name == u"p") {
                paragraphs.append(WordParagraph());
            } else if (name == u"tbl") {
                tables.append(QList<QStringList>());
            } else if (name == u"tr") {
                if (!tables.isEmpty()) {
                    tables.last().append(QStringList());
                }
            } else if (name == u"tc") {
                if (!tables.isEmpty() && !tables.last().isEmpty()) {
                    tables.last().last().append(QString());
                }
            } else if (!paragraphs.isEmpty()) {
                WordParagraph &paragraph = paragraphs.last();
                if (name == u"t") {
                    paragraph.text += xml.readElementText();
                } else if (name == u"tab") {
                    paragraph.text += QLatin1Char('\t');
                } else if (name == u"br" || name == u"cr") {
                    paragraph.text += QLatin1Char('\n');
                } else if (name == u"noBreakHyphen") {
                    paragraph.text += QLatin1Char('-');
                } else if (name == u"pStyle") {
                    paragraph.heading = headings.value(attribute(xml, u"val"), paragraph.heading);
                } else if (name == u"outlineLvl") {
                    bool ok = false;
                    const int outline = attribute(xml, u"val").toInt(&ok);
                    if (ok && outline >= 0 && outline < 6) {
                        paragraph.heading = outline + 1;
                    }
                } else if (name == u"ilvl") {
                    paragraph.listLevel = qBound(0, attribute(xml, u"val").toInt(), 8);
                } else if (name == u"numId") {
                    // numId 0 = penomoran dari gaya dimatikan
                    if (attribute(xml, u"val") == QLatin1String("0")) {
                        paragraph.listLevel = -1;
                    } else if (paragraph.listLevel < 0) {
                        paragraph.listLevel = 0;
                    }
                }
            }
        } else if (xml.isEndElement() && isWord(xml)) {
            if (xml.name() == u"p" && !paragraphs.isEmpty()) {
                emitBlock({renderParagraph(paragraphs.takeLast())});
            } else if (xml.name() == u"tbl" && !tables.isEmpty()) {
                QStringList block = renderTable(tables.takeLast());
                if (tables.isEmpty()) {
                    block.prepend(QString());   // tabel di badan dokumen dipisah baris kosong
                    block.append(QString());
                }
                emitBlock(block);
            }
        }
        cut = size > kMaxTextChars;
    }
    if (xml.hasError() && !cut) {
        fail(error, QStringLiteral("Isi dokumen Word rusak: %1").arg(xml.errorString()));
        return QString();
    }

    QString text = joinLines(lines);
    if (cut) {
        text += QLatin1Char('\n') + truncatedNote();
    }
    return text;
}

// ---------- Excel ----------

struct SheetRef {
    QString name;
    QString relationId;
    bool hidden = false;
};

QString resolvePart(const QString &target) {
    // Target relasi relatif terhadap folder xl/, atau absolut dari akar arsip
    const QString path = target.startsWith(QLatin1Char('/')) ? target.mid(1) : QStringLiteral("xl/") + target;
    QStringList parts;
    const QStringList segments = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString &segment : segments) {
        if (segment == QLatin1String("..")) {
            if (!parts.isEmpty()) {
                parts.removeLast();
            }
        } else if (segment != QLatin1String(".")) {
            parts.append(segment);
        }
    }
    return parts.join(QLatin1Char('/'));
}

QStringList sharedStrings(const QByteArray &data) {
    QStringList strings;
    QXmlStreamReader xml(data);
    QString current;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            if (xml.name() == u"si") {
                current.clear();
            } else if (xml.name() == u"rPh") {
                xml.skipCurrentElement();   // teks fonetik (furigana), bukan isi sel
            } else if (xml.name() == u"t") {
                current += xml.readElementText();
            }
        } else if (xml.isEndElement() && xml.name() == u"si") {
            strings.append(current);
        }
    }
    return strings;
}

// Format angka tanggal/waktu: id bawaan Excel, atau kode format yang memuat token d/m/y/h/s
// di luar teks berkutip, [warna/kondisi], dan karakter yang di-escape
bool isDateFormat(int id, const QString &code) {
    if ((id >= 14 && id <= 22) || (id >= 27 && id <= 36) || (id >= 45 && id <= 47) || (id >= 50 && id <= 58)) {
        return true;
    }
    bool quoted = false;
    for (qsizetype i = 0; i < code.size(); ++i) {
        const QChar c = code.at(i);
        if (quoted) {
            quoted = c != QLatin1Char('"');
        } else if (c == QLatin1Char('"')) {
            quoted = true;
        } else if (c == QLatin1Char('\\') || c == QLatin1Char('_') || c == QLatin1Char('*')) {
            ++i;
        } else if (c == QLatin1Char('[')) {
            const qsizetype end = code.indexOf(QLatin1Char(']'), i);
            if (end < 0) {
                return false;
            }
            // [h], [mm], [ss] = durasi
            const QString inner = code.mid(i + 1, end - i - 1).toLower();
            if (!inner.isEmpty() && (inner.count(inner.at(0)) == inner.size())
                && QStringLiteral("hms").contains(inner.at(0))) {
                return true;
            }
            i = end;
        } else if (QStringLiteral("dmyhsDMYHS").contains(c)) {
            return true;
        }
    }
    return false;
}

// Indeks gaya sel (cellXfs) yang format angkanya tanggal/waktu
QSet<int> dateStyles(const QByteArray &data) {
    QHash<int, QString> customFormats;
    QList<int> cellFormats;
    bool inCellXfs = false;
    QXmlStreamReader xml(data);
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            if (xml.name() == u"numFmt") {
                customFormats.insert(attribute(xml, u"numFmtId").toInt(), attribute(xml, u"formatCode"));
            } else if (xml.name() == u"cellXfs") {
                inCellXfs = true;
            } else if (xml.name() == u"xf" && inCellXfs) {
                cellFormats.append(attribute(xml, u"numFmtId").toInt());
            }
        } else if (xml.isEndElement() && xml.name() == u"cellXfs") {
            inCellXfs = false;
        }
    }

    QSet<int> styles;
    for (int index = 0; index < cellFormats.size(); ++index) {
        const int id = cellFormats.at(index);
        if (isDateFormat(id, customFormats.value(id))) {
            styles.insert(index);
        }
    }
    return styles;
}

// Nomor seri Excel -> "2026-09-28", "2026-09-28 13:45", atau "13:45" (waktu saja)
QString excelDate(double serial, bool date1904) {
    if (!std::isfinite(serial) || serial < 0 || serial > 2958465) {   // di luar tahun 1900..9999
        return QString::number(serial, 'g', 15);
    }
    qint64 days = qint64(std::floor(serial));
    qint64 seconds = qRound64((serial - double(days)) * 86400.0);
    if (seconds >= 86400) {
        ++days;
        seconds -= 86400;
    }
    const QString time = QTime(0, 0).addSecs(int(seconds))
                             .toString(seconds % 60 ? QStringLiteral("HH:mm:ss") : QStringLiteral("HH:mm"));
    if (days == 0 && seconds > 0) {
        return time;
    }
    // Excel menganggap 1900 tahun kabisat, jadi seri sebelum 1 Maret 1900 bergeser sehari
    if (!date1904 && days < 61) {
        ++days;
    }
    const QDate date = (date1904 ? QDate(1904, 1, 1) : QDate(1899, 12, 30)).addDays(days);
    return seconds == 0 ? date.toString(Qt::ISODate) : date.toString(Qt::ISODate) + QLatin1Char(' ') + time;
}

// "AB12" -> 27 (indeks kolom mulai 0); -1 bila tidak ada huruf kolom
int columnIndex(const QString &reference) {
    int index = 0;
    for (const QChar c : reference) {
        const char16_t letter = c.toUpper().unicode();
        if (letter < u'A' || letter > u'Z') {
            break;
        }
        index = index * 26 + (letter - u'A' + 1);
        if (index > kMaxColumns) {
            return -1;
        }
    }
    return index - 1;
}

QString csvField(const QString &text) {
    if (!text.contains(QLatin1Char(',')) && !text.contains(QLatin1Char('"')) && !text.contains(QLatin1Char('\n'))
        && !text.contains(QLatin1Char('\r'))) {
        return text;
    }
    QString quoted = text;
    quoted.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    return QLatin1Char('"') + quoted + QLatin1Char('"');
}

struct WorkbookContext {
    QStringList sharedStrings;
    QSet<int> dateStyles;
    bool date1904 = false;
};

QString cellText(const QString &type, const QString &value, int style, const WorkbookContext &context) {
    if (type == QLatin1String("s")) {
        bool ok = false;
        const int index = value.toInt(&ok);
        return ok && index >= 0 && index < context.sharedStrings.size() ? context.sharedStrings.at(index) : QString();
    }
    if (type == QLatin1String("b")) {
        return value == QLatin1String("1") ? QStringLiteral("TRUE") : QStringLiteral("FALSE");
    }
    if (type.isEmpty() || type == QLatin1String("n")) {
        bool ok = false;
        const double number = value.toDouble(&ok);
        if (ok && context.dateStyles.contains(style)) {
            return excelDate(number, context.date1904);
        }
    }
    return value;   // angka apa adanya, teks rumus (str), teks sebaris (inlineStr), error (#DIV/0!)
}

// <is><t>..</t></is> atau <is><r><t>..</t></r>..</is>; reader berada di awal elemen <is>
QString inlineString(QXmlStreamReader &xml) {
    QString text;
    while (xml.readNextStartElement()) {
        if (xml.name() == u"t") {
            text += xml.readElementText();
        } else if (xml.name() == u"r") {
            while (xml.readNextStartElement()) {
                if (xml.name() == u"t") {
                    text += xml.readElementText();
                } else {
                    xml.skipCurrentElement();
                }
            }
        } else {
            xml.skipCurrentElement();   // rPh, phoneticPr
        }
    }
    return text;
}

// Baris CSV satu sheet. *size bertambah sepanjang teks yang ditulis; berhenti di kMaxTextChars.
QStringList sheetRows(const QByteArray &data, const WorkbookContext &context, qsizetype *size, bool *cut,
                      QString *error) {
    QStringList rows;
    QStringList row;
    int nextColumn = 0;
    int column = -1;
    QString type;
    int style = -1;
    QString value;
    bool hasValue = false;

    QXmlStreamReader xml(data);
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            const QStringView name = xml.name();
            if (name == u"row") {
                row.clear();
                nextColumn = 0;
            } else if (name == u"c") {
                const QString reference = attribute(xml, u"r");
                column = reference.isEmpty() ? nextColumn : columnIndex(reference);
                type = attribute(xml, u"t");
                const QString styleIndex = attribute(xml, u"s");
                style = styleIndex.isEmpty() ? -1 : styleIndex.toInt();
                value.clear();
                hasValue = false;
            } else if (name == u"v") {
                value = xml.readElementText();
                hasValue = true;
            } else if (name == u"is") {
                value = inlineString(xml);
                hasValue = true;
            } else if (name == u"f") {
                xml.skipCurrentElement();   // rumus: yang dipakai hasil terakhirnya di <v>
            }
        } else if (xml.isEndElement()) {
            if (xml.name() == u"c") {
                if (hasValue && column >= 0 && column < kMaxColumns) {
                    if (row.size() <= column) {
                        row.resize(column + 1);
                    }
                    row[column] = cellText(type, value, style, context);
                }
                nextColumn = column + 1;
            } else if (xml.name() == u"row") {
                while (!row.isEmpty() && row.last().isEmpty()) {
                    row.removeLast();
                }
                if (row.isEmpty()) {
                    continue;
                }
                QStringList fields;
                for (const QString &cell : std::as_const(row)) {
                    fields.append(csvField(cell));
                }
                const QString line = fields.join(QLatin1Char(','));
                rows.append(line);
                *size += line.size() + 1;
                if (*size > kMaxTextChars) {
                    *cut = true;
                    return rows;
                }
            }
        }
    }
    if (xml.hasError()) {
        fail(error, QStringLiteral("Isi sheet rusak: %1").arg(xml.errorString()));
    }
    return rows;
}

QString workbookText(const OfficeArchive &archive, QString *error) {
    WorkbookContext context;
    QList<SheetRef> sheets;
    {
        QXmlStreamReader xml(archive.part(QStringLiteral("xl/workbook.xml")));
        while (!xml.atEnd()) {
            xml.readNext();
            if (!xml.isStartElement()) {
                continue;
            }
            if (xml.name() == u"workbookPr") {
                const QString flag = attribute(xml, u"date1904");
                context.date1904 = flag == QLatin1String("1") || flag == QLatin1String("true");
            } else if (xml.name() == u"sheet") {
                SheetRef sheet;
                sheet.name = attribute(xml, u"name");
                sheet.relationId = attribute(xml, u"id");
                const QString state = attribute(xml, u"state");
                sheet.hidden = state == QLatin1String("hidden") || state == QLatin1String("veryHidden");
                sheets.append(sheet);
            }
        }
    }

    // Relasi id -> part; chartsheet dan relasi lain bukan lembar data
    QHash<QString, QString> worksheetParts;
    {
        QXmlStreamReader xml(archive.part(QStringLiteral("xl/_rels/workbook.xml.rels")));
        while (!xml.atEnd()) {
            xml.readNext();
            if (xml.isStartElement() && xml.name() == u"Relationship"
                && attribute(xml, u"Type").endsWith(QLatin1String("/worksheet"))) {
                worksheetParts.insert(attribute(xml, u"Id"), resolvePart(attribute(xml, u"Target")));
            }
        }
    }

    context.sharedStrings = sharedStrings(archive.part(QStringLiteral("xl/sharedStrings.xml")));
    context.dateStyles = dateStyles(archive.part(QStringLiteral("xl/styles.xml")));

    QStringList blocks;
    qsizetype size = 0;
    bool cut = false;
    for (const SheetRef &sheet : std::as_const(sheets)) {
        const QString part = worksheetParts.value(sheet.relationId);
        if (part.isEmpty() || !archive.has(part)) {
            continue;
        }
        QString sheetError;
        const QStringList rows = sheetRows(archive.part(part), context, &size, &cut, &sheetError);
        if (!sheetError.isEmpty()) {
            fail(error, QStringLiteral("Sheet \"%1\": %2").arg(sheet.name, sheetError));
            return QString();
        }
        const QString header = QStringLiteral("[Sheet: %1%2]")
                                   .arg(sheet.name, sheet.hidden ? QStringLiteral(" (tersembunyi)") : QString());
        blocks.append(header + QLatin1Char('\n') + (rows.isEmpty() ? QStringLiteral("(kosong)") : rows.join(QLatin1Char('\n'))));
        if (cut) {
            break;
        }
    }
    if (blocks.isEmpty()) {
        fail(error, QStringLiteral("Workbook tidak punya sheet berisi data yang bisa dibaca"));
        return QString();
    }

    QString text = blocks.join(QStringLiteral("\n\n"));
    if (cut) {
        text += QLatin1Char('\n') + truncatedNote();
    }
    return text;
}

}

const QStringList &DocumentText::supportedSuffixes() {
    static const QStringList suffixes = {
        QStringLiteral("xlsx"), QStringLiteral("xlsm"), QStringLiteral("docx"),
        QStringLiteral("csv"), QStringLiteral("tsv"),
    };
    return suffixes;
}

bool DocumentText::isSupported(const QString &fileName) {
    return supportedSuffixes().contains(suffixOf(fileName));
}

bool DocumentText::check(const QString &path, QString *error) {
    const QString suffix = suffixOf(path);
    if (suffix == QLatin1String("xls") || suffix == QLatin1String("doc")) {
        return fail(error, QStringLiteral("Format lama .%1 tidak didukung; simpan ulang sebagai .%1x").arg(suffix));
    }
    if (!isSupported(path)) {
        return fail(error, QStringLiteral("Hanya file Excel (.xlsx), Word (.docx), dan CSV yang bisa dilampirkan"));
    }
    const QFileInfo info(path);
    if (!info.isFile()) {
        return fail(error, QStringLiteral("File tidak ditemukan"));
    }
    if (suffix == QLatin1String("csv") || suffix == QLatin1String("tsv")) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            return fail(error, QStringLiteral("File tidak bisa dibuka: %1").arg(file.errorString()));
        }
        return true;
    }
    const QString problem = officeProblem(path, suffix);
    return problem.isEmpty() || fail(error, problem);
}

QString DocumentText::extract(const QString &path, QString *error) {
    if (!check(path, error)) {
        return QString();
    }
    const QString suffix = suffixOf(path);
    if (suffix == QLatin1String("csv") || suffix == QLatin1String("tsv")) {
        return plainText(path, error);
    }
    const OfficeArchive archive(path);
    return suffix == QLatin1String("docx") ? wordText(archive, error) : workbookText(archive, error);
}

#include "NotionExport.h"

#include <QJsonArray>
#include <QJsonValue>
#include <QRegularExpression>
#include <QStringList>

namespace {

constexpr int kPageSize = 30;
constexpr int kTitleLimit = 120;
constexpr int kTabWidth = 4;

// Lebar indentasi di depan baris; tab melompat ke kelipatan kTabWidth berikutnya
int indentWidth(const QString &line, qsizetype *contentStart) {
    int width = 0;
    qsizetype i = 0;
    for (; i < line.size(); ++i) {
        if (line.at(i) == QLatin1Char(' ')) {
            ++width;
        } else if (line.at(i) == QLatin1Char('\t')) {
            width += kTabWidth - width % kTabWidth;
        } else {
            break;
        }
    }
    *contentStart = i;
    return width;
}

// Pagar blok kode (``` atau ~~~); length 0 = bukan pagar
struct Fence {
    QChar mark;
    int length = 0;
    bool bare = false;   // tanpa nama bahasa: bisa menjadi penutup
};

Fence fenceOf(const QString &content) {
    if (content.isEmpty() || (content.at(0) != QLatin1Char('`') && content.at(0) != QLatin1Char('~'))) {
        return {};
    }
    const QChar mark = content.at(0);
    int length = 0;
    while (length < content.size() && content.at(length) == mark) {
        ++length;
    }
    if (length < 3) {
        return {};
    }
    return {mark, length, content.mid(length).trimmed().isEmpty()};
}

// Garis pemisah ("---", "* * *"), bukan butir daftar
bool isThematicBreak(const QString &content) {
    static const QRegularExpression rule(QStringLiteral(R"(^([-*_])[ \t]*(\1[ \t]*){2,}$)"));
    return rule.match(content).hasMatch();
}

bool isListItem(const QString &content) {
    static const QRegularExpression item(QStringLiteral(R"(^([-*+]|\d{1,9}[.)])([ \t]|$))"));
    return item.match(content).hasMatch() && !isThematicBreak(content);
}

// Sel satu baris tabel pipa; "\|" bukan pemisah
QStringList tableCells(const QString &line) {
    QString row = line.trimmed();
    if (row.startsWith(QLatin1Char('|'))) {
        row.remove(0, 1);
    }
    if (row.endsWith(QLatin1Char('|')) && !row.endsWith(QLatin1String("\\|"))) {
        row.chop(1);
    }
    QStringList cells;
    QString cell;
    for (qsizetype i = 0; i < row.size(); ++i) {
        const QChar c = row.at(i);
        if (c == QLatin1Char('\\') && i + 1 < row.size() && row.at(i + 1) == QLatin1Char('|')) {
            cell += QStringLiteral("\\|");
            ++i;
        } else if (c == QLatin1Char('|')) {
            cells.append(cell.trimmed());
            cell.clear();
        } else {
            cell += c;
        }
    }
    cells.append(cell.trimmed());
    return cells;
}

// Baris kedua tabel pipa ("|---|:--:|"); kolomnya harus sebanyak kepala tabel
bool isDelimiterRow(const QString &line, qsizetype columns) {
    if (!line.contains(QLatin1Char('|'))) {
        return false;
    }
    static const QRegularExpression dashes(QStringLiteral("^:?-+:?$"));
    const QStringList cells = tableCells(line);
    if (cells.size() != columns) {
        return false;
    }
    for (const QString &cell : cells) {
        if (!dashes.match(cell).hasMatch()) {
            return false;
        }
    }
    return true;
}

QString plainText(const QJsonArray &richText) {
    QString text;
    for (const QJsonValue &part : richText) {
        text += part.toObject().value(QLatin1String("plain_text")).toString();
    }
    return text;
}

}

NotionExport::Request NotionExport::whoAmIRequest() {
    return {QByteArrayLiteral("GET"), QStringLiteral("/users/me"), {}};
}

NotionExport::Request NotionExport::searchRequest(const QString &query) {
    QJsonObject body{
        {QStringLiteral("filter"),
         QJsonObject{{QStringLiteral("property"), QStringLiteral("object")}, {QStringLiteral("value"), QStringLiteral("page")}}},
        {QStringLiteral("sort"), QJsonObject{{QStringLiteral("timestamp"), QStringLiteral("last_edited_time")},
                                             {QStringLiteral("direction"), QStringLiteral("descending")}}},
        {QStringLiteral("page_size"), kPageSize},
    };
    const QString trimmed = query.trimmed();
    if (!trimmed.isEmpty()) {
        body.insert(QStringLiteral("query"), trimmed);
    }
    return {QByteArrayLiteral("POST"), QStringLiteral("/search"), body};
}

NotionExport::Request NotionExport::createPageRequest(const QString &parentId, const QString &title,
                                                      const QString &markdown) {
    const QJsonArray titleText{QJsonObject{{QStringLiteral("text"), QJsonObject{{QStringLiteral("content"), title}}}}};
    return {QByteArrayLiteral("POST"), QStringLiteral("/pages"),
            QJsonObject{
                {QStringLiteral("parent"), QJsonObject{{QStringLiteral("page_id"), parentId}}},
                {QStringLiteral("properties"),
                 QJsonObject{{QStringLiteral("title"), QJsonObject{{QStringLiteral("title"), titleText}}}}},
                {QStringLiteral("markdown"), markdown},
            }};
}

NotionExport::Account NotionExport::parseAccount(const QJsonObject &body) {
    return {body.value(QLatin1String("name")).toString(),
            body.value(QLatin1String("bot")).toObject().value(QLatin1String("workspace_name")).toString()};
}

NotionExport::Page NotionExport::parsePage(const QJsonObject &body) {
    Page page;
    page.id = body.value(QLatin1String("id")).toString();
    page.url = body.value(QLatin1String("url")).toString();
    // Di bawah halaman properti judulnya bernama "title"; di data source namanya bebas, tipenya tetap
    const QJsonObject properties = body.value(QLatin1String("properties")).toObject();
    for (const QJsonValue &value : properties) {
        const QJsonObject property = value.toObject();
        if (property.value(QLatin1String("type")).toString() == QLatin1String("title")) {
            page.title = plainText(property.value(QLatin1String("title")).toArray()).simplified();
            break;
        }
    }
    const QJsonObject icon = body.value(QLatin1String("icon")).toObject();
    if (icon.value(QLatin1String("type")).toString() == QLatin1String("emoji")) {
        page.icon = icon.value(QLatin1String("emoji")).toString();
    }
    return page;
}

QList<NotionExport::Page> NotionExport::parsePages(const QJsonObject &body) {
    QList<Page> pages;
    const QJsonArray results = body.value(QLatin1String("results")).toArray();
    for (const QJsonValue &value : results) {
        const QJsonObject object = value.toObject();
        if (object.value(QLatin1String("object")).toString() != QLatin1String("page")
            || !object.contains(QLatin1String("properties"))
            || object.value(QLatin1String("in_trash")).toBool()
            || object.value(QLatin1String("archived")).toBool()) {
            continue;
        }
        Page page = parsePage(object);
        if (page.id.isEmpty()) {
            continue;
        }
        if (page.title.isEmpty()) {
            page.title = QStringLiteral("Tanpa judul");
        }
        pages.append(page);
    }
    return pages;
}

QString NotionExport::errorMessage(int status, const QJsonObject &body, const QString &networkError) {
    const QString detail = body.value(QLatin1String("message")).toString().simplified();
    switch (status) {
    case 0:
        return QStringLiteral("Notion tidak bisa dihubungi: %1")
            .arg(networkError.isEmpty() ? QStringLiteral("tidak ada jawaban") : networkError);
    case 401:
        return QStringLiteral("Token Notion ditolak. Periksa tokennya di File > Integrations.");
    case 403:
        return QStringLiteral("Integrasi Notion ini tidak diizinkan melakukannya. Aktifkan Read content dan "
                              "Insert content di pengaturan integrasinya.");
    case 404:
        return QStringLiteral("Halaman itu tidak ditemukan atau belum dibagikan ke integrasi. Di Notion: buka "
                              "halamannya > ••• > Connections > pilih integrasinya.");
    case 429:
        return QStringLiteral("Notion sedang membatasi permintaan. Coba lagi sebentar lagi.");
    default:
        break;
    }
    if (status >= 500) {
        return QStringLiteral("Notion sedang bermasalah (HTTP %1). Coba lagi nanti.").arg(status);
    }
    if (body.value(QLatin1String("code")).toString() == QLatin1String("validation_error")) {
        return QStringLiteral("Notion menolak isinya: %1").arg(detail);
    }
    return detail.isEmpty() ? QStringLiteral("Notion menjawab HTTP %1.").arg(status)
                            : QStringLiteral("Notion menjawab HTTP %1: %2").arg(status).arg(detail);
}

QString NotionExport::toNotionMarkdown(const QString &markdown) {
    QString text = markdown;
    text.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    const QStringList lines = text.split(QLatin1Char('\n'));

    QStringList out;
    QList<int> listIndents;     // indentasi butir daftar yang masih terbuka, dari luar ke dalam
    Fence open;                 // blok kode yang sedang terbuka
    int openIndent = 0;         // indentasi pagar pembukanya di sumber
    QString openPrefix;         // tab di depan setiap barisnya di hasil
    bool quoteOpen = false;     // baris hasil terakhir adalah kutipan yang masih bisa disambung

    for (qsizetype i = 0; i < lines.size(); ++i) {
        const QString &line = lines.at(i);
        qsizetype start = 0;
        const int indent = indentWidth(line, &start);
        const QString content = line.mid(start);

        if (open.length > 0) {
            const Fence fence = fenceOf(content);
            if (fence.bare && fence.mark == open.mark && fence.length >= open.length) {
                open = {};
                out.append(openPrefix + content);
                continue;
            }
            // Isi kode: hanya indentasi milik pagarnya yang dibuang
            qsizetype strip = 0;
            int width = 0;
            while (strip < start && width < openIndent) {
                width += line.at(strip) == QLatin1Char('\t') ? kTabWidth - width % kTabWidth : 1;
                ++strip;
            }
            out.append(openPrefix + line.mid(strip));
            continue;
        }

        if (content.isEmpty()) {
            // Baris kosong tidak menutup daftar: butir berikutnya bisa menyusul
            out.append(QString());
            quoteOpen = false;
            continue;
        }

        if (isListItem(content)) {
            while (!listIndents.isEmpty() && indent < listIndents.last()) {
                listIndents.removeLast();
            }
            if (listIndents.isEmpty() || indent > listIndents.last()) {
                listIndents.append(indent);
            }
            out.append(QString(listIndents.size() - 1, QLatin1Char('\t')) + content);
            quoteOpen = false;
            continue;
        }

        // Bukan butir: anak dari butir terdalam yang indentasinya lebih kecil, atau daftarnya selesai
        int depth = 0;
        while (depth < listIndents.size() && indent > listIndents.at(depth)) {
            ++depth;
        }
        listIndents.resize(depth);
        const QString prefix(depth, QLatin1Char('\t'));

        const Fence fence = fenceOf(content);
        if (fence.length > 0) {
            open = fence;
            openIndent = indent;
            openPrefix = prefix;
            out.append(prefix + content);
            quoteOpen = false;
            continue;
        }

        if (content.contains(QLatin1Char('|')) && i + 1 < lines.size()) {
            const QStringList head = tableCells(content);
            if (isDelimiterRow(lines.at(i + 1), head.size())) {
                QList<QStringList> rows{head};
                qsizetype next = i + 2;
                for (; next < lines.size(); ++next) {
                    const QString row = lines.at(next).trimmed();
                    if (row.isEmpty() || !row.contains(QLatin1Char('|')) || fenceOf(row).length > 0) {
                        break;
                    }
                    rows.append(tableCells(row));
                }
                out.append(prefix + QStringLiteral("<table header-row=\"true\">"));
                for (const QStringList &row : std::as_const(rows)) {
                    out.append(prefix + QStringLiteral("\t<tr>"));
                    // Sel yang kurang dikosongkan, yang berlebih dibuang: lebar tabel mengikuti kepalanya
                    for (qsizetype column = 0; column < head.size(); ++column) {
                        out.append(prefix + QStringLiteral("\t\t<td>%1</td>").arg(row.value(column)));
                    }
                    out.append(prefix + QStringLiteral("\t</tr>"));
                }
                out.append(prefix + QStringLiteral("</table>"));
                i = next - 1;
                quoteOpen = false;
                continue;
            }
        }

        if (content.startsWith(QLatin1Char('>'))) {
            // Notion menjadikan setiap baris ">" satu blok kutipan: baris yang berurutan disatukan
            const QString quote = content.mid(1).trimmed();
            if (quoteOpen) {
                out.last() += QStringLiteral("<br>") + quote;
            } else {
                out.append(prefix + QStringLiteral("> ") + quote);
                quoteOpen = true;
            }
            continue;
        }

        out.append(prefix + content);
        quoteOpen = false;
    }
    return out.join(QLatin1Char('\n'));
}

QString NotionExport::pageTitle(const QString &text) {
    QString title = text.simplified();
    if (title.size() > kTitleLimit) {
        title = title.left(kTitleLimit - 1).trimmed() + QChar(0x2026);
    }
    return title;
}

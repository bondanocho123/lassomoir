#include "SourceTokens.h"

#include <QFileInfo>
#include <QSet>
#include <QStringList>

#include <cmath>

namespace {

class Tokenizer {
public:
    Tokenizer(const QString &source, const SourceRules &rules) : m_src(source), m_rules(rules) {}

    QList<SourceToken> run();

private:
    QChar at(qsizetype offset) const {
        const qsizetype index = m_pos + offset;
        return index < m_src.size() ? m_src.at(index) : QChar();
    }
    void advance(qsizetype to);   // pindah posisi sambil menghitung baris yang dilewati
    void skipLineComment();
    void skipBlockComment();
    void readDirective();
    bool readString(bool lineStart);
    bool readCSharpString();
    qsizetype csharpStringEnd(qsizetype quote, bool verbatim, bool interpolated) const;
    qsizetype interpolationEnd(qsizetype pos) const;
    void readNumber();
    void readWord();
    void readSymbol();
    void add(SourceToken::Kind kind, qsizetype start, int startLine, const QString &text = QString());

    const QString &m_src;
    const SourceRules &m_rules;
    qsizetype m_pos = 0;
    int m_line = 0;
    QList<SourceToken> m_tokens;
};

QList<SourceToken> Tokenizer::run() {
    bool inCode = !m_rules.phpTags;
    bool lineStart = true;
    while (m_pos < m_src.size()) {
        if (!inCode) {
            const qsizetype open = m_src.indexOf(QLatin1String("<?"), m_pos);
            if (open < 0) {
                break;
            }
            advance(open + 2);
            if (QStringView(m_src).mid(m_pos, 3).compare(QLatin1String("php"), Qt::CaseInsensitive) == 0) {
                m_pos += 3;
            } else if (at(0) == QLatin1Char('=')) {
                ++m_pos;
            }
            inCode = true;
            lineStart = false;
            continue;
        }

        const QChar c = at(0);
        if (c == QLatin1Char('\n')) {
            advance(m_pos + 1);
            lineStart = true;
            continue;
        }
        if (c.isSpace()) {
            ++m_pos;
            continue;
        }
        const bool firstOnLine = lineStart;
        lineStart = false;

        if (m_rules.phpTags && c == QLatin1Char('?') && at(1) == QLatin1Char('>')) {
            m_pos += 2;
            inCode = false;
            continue;
        }
        if (m_rules.syntax == SourceSyntax::CFamily && c == QLatin1Char('/') && at(1) == QLatin1Char('/')) {
            skipLineComment();
            continue;
        }
        if (m_rules.syntax == SourceSyntax::CFamily && c == QLatin1Char('/') && at(1) == QLatin1Char('*')) {
            skipBlockComment();
            continue;
        }
        if (c == QLatin1Char('#')) {
            // PHP 8: "#[" adalah atribut, bukan komentar
            if (m_rules.hashComments && !(m_rules.phpTags && at(1) == QLatin1Char('['))) {
                skipLineComment();
                continue;
            }
            if (m_rules.preprocessor && firstOnLine) {
                readDirective();
                continue;
            }
        }
        if ((m_rules.csharp && readCSharpString()) || readString(firstOnLine)) {
            continue;
        }
        const bool verbatimIdentifier = m_rules.csharp && c == QLatin1Char('@')
                                        && (at(1).isLetter() || at(1) == QLatin1Char('_'));
        if (c.isDigit() || (c == QLatin1Char('.') && at(1).isDigit())) {
            readNumber();
        } else if (c.isLetter() || c == QLatin1Char('_') || c == QLatin1Char('$') || verbatimIdentifier) {
            readWord();
        } else {
            readSymbol();
        }
    }
    return m_tokens;
}

void Tokenizer::advance(qsizetype to) {
    to = qMin(to, m_src.size());
    for (qsizetype i = m_pos; i < to; ++i) {
        if (m_src.at(i) == QLatin1Char('\n')) {
            ++m_line;
        }
    }
    m_pos = to;
}

void Tokenizer::skipLineComment() {
    qsizetype end = m_src.indexOf(QLatin1Char('\n'), m_pos);
    if (end < 0) {
        end = m_src.size();
    }
    if (m_rules.phpTags) {
        // Di PHP, "?>" juga mengakhiri komentar satu baris
        const qsizetype close = m_src.indexOf(QLatin1String("?>"), m_pos);
        if (close >= 0 && close < end) {
            end = close;
        }
    }
    m_pos = end;
}

void Tokenizer::skipBlockComment() {
    const qsizetype end = m_src.indexOf(QLatin1String("*/"), m_pos + 2);
    advance(end < 0 ? m_src.size() : end + 2);
}

void Tokenizer::readDirective() {
    const qsizetype start = m_pos;
    const int startLine = m_line;
    qsizetype end = m_pos;
    while (end < m_src.size()) {
        if (m_src.at(end) == QLatin1Char('\n')) {
            // Direktif berlanjut ke baris berikutnya bila baris diakhiri '\'
            qsizetype last = end - 1;
            if (last >= 0 && m_src.at(last) == QLatin1Char('\r')) {
                --last;
            }
            if (last < 0 || m_src.at(last) != QLatin1Char('\\')) {
                break;
            }
        }
        ++end;
    }
    advance(end);

    // Satu token per jenis direktif ("#include"), bukan per isi baris
    qsizetype nameStart = start + 1;
    while (nameStart < end && m_src.at(nameStart).isSpace()) {
        ++nameStart;
    }
    qsizetype nameEnd = nameStart;
    while (nameEnd < end && m_src.at(nameEnd).isLetter()) {
        ++nameEnd;
    }
    add(SourceToken::Kind::Directive, start, startLine, QLatin1Char('#') + m_src.mid(nameStart, nameEnd - nameStart));
}

bool Tokenizer::readString(bool lineStart) {
    const bool python = m_rules.syntax == SourceSyntax::Python;
    qsizetype quote = m_pos;
    if (python) {
        // Awalan r, b, f, u (paling banyak dua huruf) sebelum tanda kutip
        static const QString prefixes = QStringLiteral("rRbBuUfF");
        while (quote < m_pos + 2 && quote < m_src.size() && prefixes.contains(m_src.at(quote))) {
            ++quote;
        }
    }
    if (quote >= m_src.size()) {
        return false;
    }
    const QChar q = m_src.at(quote);
    if (q != QLatin1Char('"') && q != QLatin1Char('\'') && (python || q != QLatin1Char('`'))) {
        return false;
    }

    const qsizetype start = m_pos;
    const int startLine = m_line;
    if (m_rules.lifetimes && q == QLatin1Char('\'')) {
        const QChar next = at(1);
        if ((next.isLetter() || next == QLatin1Char('_')) && at(2) != QLatin1Char('\'')) {
            // Lifetime Rust ('a, 'static) adalah nama, bukan literal karakter
            ++m_pos;
            while (m_pos < m_src.size() && (m_src.at(m_pos).isLetterOrNumber() || m_src.at(m_pos) == QLatin1Char('_'))) {
                ++m_pos;
            }
            add(SourceToken::Kind::Word, start, startLine);
            return true;
        }
    }

    const QString triple(3, q);
    const bool isTriple = (python || q == QLatin1Char('"')) && QStringView(m_src).mid(quote, 3) == triple;
    qsizetype end;
    if (isTriple) {
        const qsizetype close = m_src.indexOf(triple, quote + 3);
        end = close < 0 ? m_src.size() : close + 3;
    } else {
        const bool multiline = q == QLatin1Char('`');
        end = quote + 1;
        while (end < m_src.size()) {
            const QChar ch = m_src.at(end);
            if (ch == QLatin1Char('\\')) {
                end += 2;
                continue;
            }
            if (ch == q) {
                ++end;
                break;
            }
            if (ch == QLatin1Char('\n') && !multiline) {
                break;   // string tak tertutup berhenti di akhir baris
            }
            ++end;
        }
    }
    advance(end);

    // Docstring Python (string tiga kutip di awal baris) diperlakukan sebagai komentar
    if (python && isTriple && lineStart) {
        return true;
    }
    add(SourceToken::Kind::Text, start, startLine);
    return true;
}

// String C#: biasa "..", verbatim @"..", interpolasi $".." / $@"..", dan raw """.."""
bool Tokenizer::readCSharpString() {
    qsizetype quote = m_pos;
    bool verbatim = false;
    bool interpolated = false;
    while (quote < m_src.size() && (m_src.at(quote) == QLatin1Char('@') || m_src.at(quote) == QLatin1Char('$'))) {
        (m_src.at(quote) == QLatin1Char('@') ? verbatim : interpolated) = true;
        ++quote;
    }
    if (quote >= m_src.size() || m_src.at(quote) != QLatin1Char('"')) {
        return false;
    }
    const qsizetype start = m_pos;
    const int startLine = m_line;
    advance(csharpStringEnd(quote, verbatim, interpolated));
    add(SourceToken::Kind::Text, start, startLine);
    return true;
}

qsizetype Tokenizer::csharpStringEnd(qsizetype quote, bool verbatim, bool interpolated) const {
    const qsizetype size = m_src.size();
    qsizetype fence = 0;
    while (quote + fence < size && m_src.at(quote + fence) == QLatin1Char('"')) {
        ++fence;
    }
    if (fence >= 3) {
        // Raw string: ditutup oleh deretan kutip sepanjang pembukanya
        const qsizetype close = m_src.indexOf(QString(fence, QLatin1Char('"')), quote + fence);
        return close < 0 ? size : close + fence;
    }

    qsizetype i = quote + 1;
    while (i < size) {
        const QChar ch = m_src.at(i);
        if (ch == QLatin1Char('"')) {
            if (verbatim && i + 1 < size && m_src.at(i + 1) == QLatin1Char('"')) {
                i += 2;   // "" = tanda kutip di dalam string verbatim
                continue;
            }
            return i + 1;
        }
        if (ch == QLatin1Char('\\') && !verbatim) {
            i += 2;
            continue;
        }
        if (ch == QLatin1Char('\n') && !verbatim) {
            return i;   // string biasa tak tertutup berhenti di akhir baris
        }
        if (interpolated && ch == QLatin1Char('{')) {
            if (i + 1 < size && m_src.at(i + 1) == QLatin1Char('{')) {
                i += 2;   // "{{" = kurung kurawal literal
                continue;
            }
            i = interpolationEnd(i + 1);
            continue;
        }
        ++i;
    }
    return size;
}

// Isi lubang interpolasi "{...}" boleh memuat string (dengan kutip) dan kurung bersarang
qsizetype Tokenizer::interpolationEnd(qsizetype pos) const {
    const qsizetype size = m_src.size();
    int depth = 1;
    qsizetype i = pos;
    while (i < size) {
        const QChar ch = m_src.at(i);
        if (ch == QLatin1Char('"')) {
            bool verbatim = false;
            bool interpolated = false;
            for (qsizetype p = i - 1; p >= pos && (m_src.at(p) == QLatin1Char('@') || m_src.at(p) == QLatin1Char('$')); --p) {
                (m_src.at(p) == QLatin1Char('@') ? verbatim : interpolated) = true;
            }
            i = csharpStringEnd(i, verbatim, interpolated);
            continue;
        }
        if (ch == QLatin1Char('\'')) {
            i += (i + 1 < size && m_src.at(i + 1) == QLatin1Char('\\')) ? 4 : 3;   // 'x' atau '\x'
            continue;
        }
        if (ch == QLatin1Char('{')) {
            ++depth;
        } else if (ch == QLatin1Char('}') && --depth == 0) {
            return i + 1;
        }
        ++i;
    }
    return size;
}

void Tokenizer::readNumber() {
    const qsizetype start = m_pos;
    const int startLine = m_line;
    const bool hex = QStringView(m_src).mid(m_pos, 2).compare(QLatin1String("0x"), Qt::CaseInsensitive) == 0;
    ++m_pos;
    while (m_pos < m_src.size()) {
        const QChar ch = m_src.at(m_pos);
        const QChar previous = m_src.at(m_pos - 1);
        const bool exponentSign = !hex && (ch == QLatin1Char('+') || ch == QLatin1Char('-'))
                                  && (previous == QLatin1Char('e') || previous == QLatin1Char('E'));
        // "0..10" (range) berhenti sebelum titik ganda
        const bool decimalPoint = ch == QLatin1Char('.') && at(1) != QLatin1Char('.');
        if (!ch.isLetterOrNumber() && ch != QLatin1Char('_') && !decimalPoint && !exponentSign) {
            break;
        }
        ++m_pos;
    }
    add(SourceToken::Kind::Number, start, startLine);
}

void Tokenizer::readWord() {
    const qsizetype start = m_pos;
    const int startLine = m_line;
    ++m_pos;
    while (m_pos < m_src.size()) {
        const QChar ch = m_src.at(m_pos);
        if (!ch.isLetterOrNumber() && ch != QLatin1Char('_') && ch != QLatin1Char('$')) {
            break;
        }
        ++m_pos;
    }
    add(SourceToken::Kind::Word, start, startLine);
}

void Tokenizer::readSymbol() {
    // Terpanjang dulu, supaya ">>=" tidak terbaca sebagai ">" ">" "="
    static const QStringList symbols = {
        QStringLiteral(">>>="), QStringLiteral("<<="), QStringLiteral(">>="), QStringLiteral(">>>"),
        QStringLiteral("..."), QStringLiteral("==="), QStringLiteral("!=="), QStringLiteral("**="),
        QStringLiteral("?\?="), QStringLiteral("<=>"), QStringLiteral("->*"), QStringLiteral("//="),
        QStringLiteral("::"), QStringLiteral("->"), QStringLiteral("=>"), QStringLiteral("=="),
        QStringLiteral("!="), QStringLiteral("<="), QStringLiteral(">="), QStringLiteral("&&"),
        QStringLiteral("||"), QStringLiteral("++"), QStringLiteral("--"), QStringLiteral("+="),
        QStringLiteral("-="), QStringLiteral("*="), QStringLiteral("/="), QStringLiteral("%="),
        QStringLiteral("&="), QStringLiteral("|="), QStringLiteral("^="), QStringLiteral("<<"),
        QStringLiteral(">>"), QStringLiteral("**"), QStringLiteral("??"), QStringLiteral("?."),
        QStringLiteral("?:"), QStringLiteral(":="), QStringLiteral(".."), QStringLiteral("//"),
    };
    const qsizetype start = m_pos;
    const QStringView rest = QStringView(m_src).mid(m_pos);
    qsizetype length = 1;
    for (const QString &symbol : symbols) {
        if (rest.startsWith(symbol)) {
            length = symbol.size();
            break;
        }
    }
    // C#: ">>" di akhir generic bersarang ("List<List<int>>") dibaca sebagai dua penutup
    if (m_rules.csharp && length == 2 && rest.startsWith(u">>")) {
        length = 1;
    }
    m_pos += length;
    add(SourceToken::Kind::Symbol, start, m_line);
}

void Tokenizer::add(SourceToken::Kind kind, qsizetype start, int startLine, const QString &text) {
    SourceToken token;
    token.kind = kind;
    token.text = text.isEmpty() ? m_src.mid(start, m_pos - start) : text;
    token.line = startLine;
    token.endLine = m_line;
    m_tokens.append(token);
}

}

std::optional<SourceRules> SourceTokens::rulesFor(const QString &path) {
    static const QSet<QString> cFamily = {"c", "h", "cc", "cpp", "cxx", "c++", "hh", "hpp", "hxx",
                                          "h++", "ino", "m", "mm"};
    static const QSet<QString> withTernary = {"java", "js", "jsx", "mjs", "cjs"};
    // Bahasa dengan tipe nullable "T?" atau tanpa operator ternary
    static const QSet<QString> withoutTernary = {"ts", "tsx", "mts", "cts", "go", "kt", "kts", "dart", "scala"};

    const QString extension = QFileInfo(path).suffix().toLower();
    SourceRules rules;
    if (extension == QLatin1String("py") || extension == QLatin1String("pyw")) {
        rules.syntax = SourceSyntax::Python;
        rules.hashComments = true;
        rules.ternary = false;
    } else if (cFamily.contains(extension)) {
        rules.preprocessor = true;
    } else if (extension == QLatin1String("cs")) {
        // Ternary C# dibedakan dari tipe nullable oleh CSharpMetrics
        rules.preprocessor = true;
        rules.ternary = false;
        rules.csharp = true;
    } else if (extension == QLatin1String("swift")) {
        rules.preprocessor = true;
        rules.ternary = false;
    } else if (extension == QLatin1String("php") || extension == QLatin1String("phtml")) {
        rules.hashComments = true;
        rules.phpTags = true;
    } else if (extension == QLatin1String("rs")) {
        rules.lifetimes = true;
        rules.ternary = false;
    } else if (withoutTernary.contains(extension)) {
        rules.ternary = false;
    } else if (!withTernary.contains(extension)) {
        return std::nullopt;
    }
    return rules;
}

QList<SourceToken> SourceTokens::tokenize(const QString &source, const SourceRules &rules) {
    return Tokenizer(source, rules).run();
}

int SourceTokens::codeLines(const QList<SourceToken> &tokens, const QList<TokenRange> &ranges) {
    int count = 0;
    int counted = -1;   // baris terakhir yang sudah dihitung
    for (const TokenRange &range : ranges) {
        for (qsizetype i = range.first; i < range.second && i < tokens.size(); ++i) {
            const SourceToken &token = tokens.at(i);
            const int from = qMax(token.line, counted + 1);
            if (token.endLine >= from) {
                count += token.endLine - from + 1;
                counted = token.endLine;
            }
        }
    }
    return count;
}

double SourceTokens::halsteadVolume(const QList<SourceToken> &tokens, const QList<TokenRange> &ranges) {
    QSet<QString> vocabulary;
    qint64 length = 0;
    for (const TokenRange &range : ranges) {
        for (qsizetype i = range.first; i < range.second && i < tokens.size(); ++i) {
            const SourceToken &token = tokens.at(i);
            if (token.is(u")") || token.is(u"]") || token.is(u"}")) {
                continue;
            }
            ++length;
            vocabulary.insert(token.text);
        }
    }
    return vocabulary.size() > 1 ? double(length) * std::log2(double(vocabulary.size())) : 0.0;
}

#include "CodeMetrics.h"
#include "SourceTokens.h"

#include <QSet>

#include <algorithm>
#include <cmath>

namespace {

using Kind = SourceToken::Kind;

const QSet<QString> &decisionWords() {
    static const QSet<QString> words = {"if", "elif", "elseif", "for", "foreach", "while", "case",
                                        "catch", "except", "and", "or", "guard"};
    return words;
}

// Kata yang membuka pernyataan kontrol: "if (x) {" bukan fungsi
const QSet<QString> &controlWords() {
    static const QSet<QString> words = {
        "if", "elif", "elseif", "else", "for", "foreach", "while", "do", "switch", "case", "catch", "except",
        "try", "finally", "using", "lock", "fixed", "synchronized", "with", "return", "throw", "new", "delete",
        "sizeof", "typeof", "alignof", "decltype", "await", "yield", "when", "match", "guard", "range", "select",
        "not", "and", "or", "in", "is", "as", "assert", "static_assert", "go", "defer",
    };
    return words;
}

const QSet<QString> &functionWords() {
    static const QSet<QString> words = {"function", "func", "fn", "fun", "def"};
    return words;
}

// "class Point(...) {" (Kotlin/Scala) adalah deklarasi tipe, bukan fungsi
const QSet<QString> &typeWords() {
    static const QSet<QString> words = {"class", "struct", "interface", "enum", "record", "object", "union",
                                        "trait", "impl", "extension", "protocol", "namespace", "module"};
    return words;
}

bool isDecision(const SourceToken &token, const SourceRules &rules, const SourceToken *previous) {
    if (token.kind == Kind::Word) {
        return decisionWords().contains(token.text);
    }
    if (token.kind != Kind::Symbol) {
        return false;
    }
    if (token.is(u"&&") || token.is(u"||")) {
        return true;
    }
    if (!rules.ternary || (!token.is(u"?") && !token.is(u"?:"))) {
        return false;
    }
    // "(?string $x" / ": ?int" (PHP) adalah tipe nullable, bukan ternary
    return !previous || !(previous->is(u"(") || previous->is(u",") || previous->is(u":"));
}

// Token yang boleh ada di antara ")" dan "{" pada tanda tangan fungsi: tipe kembalian
// ("-> Int", ": string", "error"), qualifier ("const", "noexcept", "throws X"), generic
bool isSignatureTail(const SourceToken &token) {
    static const QSet<QString> symbols = {"*", "&", "&&", ".", "::", "->", ":", "<", ">", ">>",
                                          ",", "[", "]", "?", "!", "|"};
    if (token.kind == Kind::Symbol) {
        return symbols.contains(token.text);
    }
    if (token.kind == Kind::Word) {
        return token.text == u"impl"   // Rust "-> impl Trait"
               || (!controlWords().contains(token.text) && !typeWords().contains(token.text));
    }
    return false;
}

qsizetype matchingOpen(const QList<SourceToken> &tokens, qsizetype close) {
    int depth = 0;
    for (qsizetype i = close; i >= 0; --i) {
        if (tokens.at(i).is(u")")) {
            ++depth;
        } else if (tokens.at(i).is(u"(") && --depth == 0) {
            return i;
        }
    }
    return -1;
}

// Apakah pernyataan yang memuat token ini diawali kata kontrol ("if strings.HasPrefix(x) {")
bool insideControlStatement(const QList<SourceToken> &tokens, qsizetype index) {
    for (qsizetype i = index - 1; i >= 0; --i) {
        const SourceToken &token = tokens.at(i);
        if (token.kind == Kind::Directive || token.is(u";") || token.is(u"{") || token.is(u"}")) {
            return false;
        }
        if (token.kind == Kind::Word && controlWords().contains(token.text)) {
            return true;
        }
    }
    return false;
}

// Badan fungsi = "{" setelah daftar parameter "(...)" (plus tipe kembalian/qualifier), atau
// setelah "=>". Heuristik lintas bahasa: cukup untuk rata-rata per fungsi, bukan parser.
int countBraceFunctions(const QList<SourceToken> &tokens) {
    int count = 0;
    for (qsizetype i = 0; i < tokens.size(); ++i) {
        if (!tokens.at(i).is(u"{")) {
            continue;
        }
        if (i > 0 && tokens.at(i - 1).is(u"=>")) {
            ++count;   // arrow function / lambda
            continue;
        }
        qsizetype close = i - 1;
        while (close >= 0 && isSignatureTail(tokens.at(close))) {
            --close;
        }
        if (close < 0 || !tokens.at(close).is(u")")) {
            continue;
        }
        const qsizetype open = matchingOpen(tokens, close);
        if (open <= 0) {
            continue;
        }

        const SourceToken &before = tokens.at(open - 1);
        // "[..](x) {" lambda C++, "func (r T) Name(x) (int, error) {" Go, "(() {" / ", () {" fungsi anonim,
        // "function (x) {"
        if (before.is(u"]") || before.is(u")") || before.is(u"(") || before.is(u",")
            || (before.kind == Kind::Word && functionWords().contains(before.text))) {
            ++count;
            continue;
        }
        if (before.kind != Kind::Word) {
            continue;
        }
        // "fn new(" / "function foo(": kata kunci fungsi tepat sebelum nama
        const SourceToken *keyword = open >= 2 ? &tokens.at(open - 2) : nullptr;
        if (keyword && keyword->kind == Kind::Word && functionWords().contains(keyword->text)) {
            ++count;
            continue;
        }
        if (controlWords().contains(before.text)
            || (keyword && keyword->kind == Kind::Word && typeWords().contains(keyword->text))
            || insideControlStatement(tokens, open - 1)) {
            continue;
        }
        ++count;
    }
    return count;
}

}

bool CodeMetrics::supports(const QString &path) {
    return SourceTokens::rulesFor(path).has_value();
}

CodeMetrics CodeMetrics::measure(const QString &path, const QString &source) {
    CodeMetrics metrics;
    const std::optional<SourceRules> rules = SourceTokens::rulesFor(path);
    if (!rules) {
        return metrics;
    }
    const QList<SourceToken> tokens = SourceTokens::tokenize(source, *rules);
    const QList<TokenRange> whole = {{0, tokens.size()}};
    metrics.sloc = SourceTokens::codeLines(tokens, whole);
    metrics.volume = SourceTokens::halsteadVolume(tokens, whole);

    int decisions = 0;
    for (qsizetype i = 0; i < tokens.size(); ++i) {
        if (isDecision(tokens.at(i), *rules, i > 0 ? &tokens.at(i - 1) : nullptr)) {
            ++decisions;
        }
    }
    if (rules->syntax == SourceSyntax::Python) {
        metrics.functions = int(std::count_if(tokens.cbegin(), tokens.cend(), [](const SourceToken &token) {
            return token.isWord(u"def");
        }));
    } else {
        metrics.functions = countBraceFunctions(tokens);
    }
    metrics.complexity = qMax(1, metrics.functions) + decisions;

    if (rules->csharp) {
        // Member dan kompleksitas dari pengurai C# lebih tepat: property berbadan, ??, ?.,
        // ternary yang dibedakan dari tipe nullable, arm switch expression
        metrics.types = CSharpMetrics::analyze(tokens);
        int members = 0;
        int complexity = 0;
        for (const TypeMetrics &type : std::as_const(metrics.types)) {
            members += int(type.members.size());
            complexity += type.complexity;
        }
        if (members > 0) {
            metrics.functions = members;
            metrics.complexity = complexity;
        }
    }
    metrics.maintainability = maintainabilityIndex(metrics.volume, metrics.complexity, metrics.sloc, metrics.functions);
    return metrics;
}

int CodeMetrics::maintainabilityIndex(double volume, int complexity, int sloc, int functions) {
    const double units = qMax(1, functions);
    const double raw = 171.0 - 5.2 * std::max(0.0, std::log(volume / units)) - 0.23 * (complexity / units)
                       - 16.2 * std::max(0.0, std::log(sloc / units));
    return int(std::lround(std::clamp(raw * 100.0 / 171.0, 0.0, 100.0)));
}

MaintainabilityRating maintainabilityRating(int index) {
    if (index >= 20) {
        return MaintainabilityRating::Good;
    }
    return index >= 10 ? MaintainabilityRating::Moderate : MaintainabilityRating::Low;
}

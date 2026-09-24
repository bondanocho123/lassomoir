#include "CSharpMetrics.h"
#include "CodeMetrics.h"

#include <QRegularExpression>
#include <QSet>

namespace {

using Kind = SourceToken::Kind;

// Kata kunci tetap C#: tidak pernah jadi nama tipe atau identifier
const QSet<QString> &reservedWords() {
    static const QSet<QString> words = {
        "abstract", "as", "base", "bool", "break", "byte", "case", "catch", "char", "checked", "class", "const",
        "continue", "decimal", "default", "delegate", "do", "double", "else", "enum", "event", "explicit", "extern",
        "false", "finally", "fixed", "float", "for", "foreach", "goto", "if", "implicit", "in", "int", "interface",
        "internal", "is", "lock", "long", "namespace", "new", "null", "object", "operator", "out", "override",
        "params", "private", "protected", "public", "readonly", "ref", "return", "sbyte", "sealed", "short",
        "sizeof", "stackalloc", "static", "string", "struct", "switch", "this", "throw", "true", "try", "typeof",
        "uint", "ulong", "unchecked", "unsafe", "ushort", "using", "virtual", "void", "volatile", "while",
    };
    return words;
}

// Kata kontekstual yang bisa mengikuti nama tipe tanpa membentuk deklarasi ("x is Foo and ...")
const QSet<QString> &clauseWords() {
    static const QSet<QString> words = {"and", "or", "not", "when", "with", "switch", "where", "select", "orderby",
                                        "group", "into", "join", "on", "equals", "by", "let", "ascending",
                                        "descending", "from"};
    return words;
}

const QSet<QString> &typeWords() {
    static const QSet<QString> words = {"class", "struct", "interface", "enum", "record"};
    return words;
}

const QSet<QString> &decisionWords() {
    static const QSet<QString> words = {"if", "while", "for", "foreach", "case", "catch", "when", "and", "or"};
    return words;
}

// Tipe bawaan (dan Task) tidak dihitung sebagai kopling, seperti Code Metrics Visual Studio
const QSet<QString> &ignoredTypes() {
    static const QSet<QString> names = {"Object", "String", "Boolean", "Byte", "SByte", "Char", "Decimal", "Double",
                                        "Single", "Int16", "Int32", "Int64", "UInt16", "UInt32", "UInt64", "IntPtr",
                                        "UIntPtr", "Void", "ValueType", "Task", "ValueTask"};
    return names;
}

// DIT kelas framework yang sering dijadikan kelas dasar (jumlah leluhurnya sampai System.Object)
const QHash<QString, int> &frameworkDepths() {
    static const QHash<QString, int> depths = {
        {"Exception", 1}, {"SystemException", 2}, {"ApplicationException", 2}, {"InvalidOperationException", 3},
        {"ArgumentException", 3}, {"ArgumentNullException", 4}, {"ArgumentOutOfRangeException", 4},
        {"NotImplementedException", 3}, {"NotSupportedException", 3}, {"IOException", 3}, {"Attribute", 1},
        {"EventArgs", 1}, {"Stream", 2}, {"MonoBehaviour", 4}, {"ScriptableObject", 2}, {"Editor", 3},
        {"EditorWindow", 3}, {"Form", 6}, {"Controller", 2}, {"ControllerBase", 1}, {"PageModel", 1},
        {"ComponentBase", 1}, {"DbContext", 1}, {"BackgroundService", 1}, {"Hub", 1},
    };
    return depths;
}

bool looksLikeInterface(const QString &name) {
    return name.size() > 1 && name.at(0) == QLatin1Char('I') && name.at(1).isUpper();
}

bool isTypeName(const QString &text) {
    return !text.isEmpty() && text.at(0).isUpper() && !ignoredTypes().contains(text);
}

struct TypeContext {
    int index = -1;                    // posisi slot di daftar hasil
    QString namespaceName;
    QString name;                      // di dalam namespace, termasuk tipe induk
    QString simpleName;
    QSet<QString> excluded;            // bukan kopling: tipe ini, induk, bersarang, parameter generic
    QSet<QString> memberNames;         // "Items.Count" dengan Items milik tipe ini bukan akses tipe
    QSet<QString> coupled;             // atribut dan base list
    QList<TokenRange> nestedSpans;     // tipe bersarang punya metriknya sendiri
};

struct PendingMember {
    MemberMetrics metrics;
    TokenRange range;
    QSet<QString> generics;
    QSet<QString> attributes;
};

class Parser {
public:
    explicit Parser(const QList<SourceToken> &tokens) : m_t(tokens) {}

    QList<TypeMetrics> run() {
        parseDeclarations(0, m_t.size(), QString());
        return m_types;
    }

private:
    const SourceToken &at(qsizetype i) const {
        static const SourceToken none;
        return i >= 0 && i < m_t.size() ? m_t.at(i) : none;
    }
    qsizetype matching(qsizetype open, qsizetype end) const;
    qsizetype genericEnd(qsizetype open, qsizetype end) const;
    qsizetype statementEnd(qsizetype i, qsizetype end) const;
    qsizetype expressionEnd(qsizetype i, qsizetype end) const;
    bool isTypeDeclaration(qsizetype keyword) const;

    void parseDeclarations(qsizetype begin, qsizetype end, QString ns);
    qsizetype parseType(qsizetype start, qsizetype keyword, qsizetype end, const QString &ns, TypeContext *outer);
    QList<PendingMember> parseMembers(qsizetype begin, qsizetype end, TypeContext &type);

    QSet<QString> attributeNames(qsizetype open, qsizetype close) const;
    QString parameterTypes(qsizetype open, qsizetype close) const;
    int complexity(TokenRange range) const;
    bool isTernary(qsizetype question, qsizetype end) const;
    int switchArms(qsizetype open, qsizetype end) const;
    void collectTypes(TokenRange range, const TypeContext &type, const QSet<QString> &excluded,
                      QSet<QString> *out) const;

    const QList<SourceToken> &m_t;
    QSet<QString> m_namespaceRoots = {"System", "Microsoft", "global"};
    QList<TypeMetrics> m_types;
};

qsizetype Parser::matching(qsizetype open, qsizetype end) const {
    const QString &opener = m_t.at(open).text;
    const QStringView closer = opener == u"(" ? u")" : opener == u"[" ? u"]" : u"}";
    int depth = 0;
    for (qsizetype i = open; i < end; ++i) {
        if (m_t.at(i).is(opener)) {
            ++depth;
        } else if (m_t.at(i).is(closer) && --depth == 0) {
            return i;
        }
    }
    return end - 1;   // tidak seimbang: anggap berakhir di ujung rentang
}

// "<...>" setelah nama = daftar argumen/parameter generic; indeks setelah '>' penutup, atau -1
// bila isinya bukan tipe (mis. perbandingan "a < b")
qsizetype Parser::genericEnd(qsizetype open, qsizetype end) const {
    int depth = 0;
    for (qsizetype i = open; i < end; ++i) {
        const SourceToken &t = m_t.at(i);
        if (t.is(u"<")) {
            ++depth;
        } else if (t.is(u">")) {
            if (--depth == 0) {
                return i + 1;
            }
        } else if (t.kind != Kind::Word && !t.is(u",") && !t.is(u".") && !t.is(u"?") && !t.is(u"[")
                   && !t.is(u"]") && !t.is(u"(") && !t.is(u")") && !t.is(u"::") && !t.is(u"*")) {
            return -1;
        }
    }
    return -1;
}

// Pernyataan berakhir di ';' kedalaman 0 atau saat blok "{...}" miliknya ditutup
qsizetype Parser::statementEnd(qsizetype i, qsizetype end) const {
    int depth = 0;
    for (; i < end; ++i) {
        const SourceToken &t = m_t.at(i);
        if (t.is(u"(") || t.is(u"[") || t.is(u"{")) {
            ++depth;
        } else if (t.is(u")") || t.is(u"]") || t.is(u"}")) {
            if (--depth <= 0 && t.is(u"}")) {
                return i + 1;
            }
        } else if (depth == 0 && t.is(u";")) {
            return i + 1;
        }
    }
    return end;
}

// Ekspresi (badan "=>", inisialisasi) berakhir di ';' kedalaman 0; blok di dalamnya ikut dilewati
qsizetype Parser::expressionEnd(qsizetype i, qsizetype end) const {
    int depth = 0;
    for (; i < end; ++i) {
        const SourceToken &t = m_t.at(i);
        if (t.is(u"(") || t.is(u"[") || t.is(u"{")) {
            ++depth;
        } else if (t.is(u")") || t.is(u"]") || t.is(u"}")) {
            if (--depth < 0) {
                return i - 1;
            }
        } else if (depth == 0 && t.is(u";")) {
            return i;
        }
    }
    return end - 1;
}

// "class Foo", "record struct Foo" — bukan "where T : class"
bool Parser::isTypeDeclaration(qsizetype keyword) const {
    const SourceToken &previous = at(keyword - 1);
    if (previous.is(u":") || previous.is(u",") || previous.is(u".") || previous.is(u"<") || previous.is(u"(")) {
        return false;
    }
    qsizetype name = keyword + 1;
    if (m_t.at(keyword).isWord(u"record") && (at(name).isWord(u"class") || at(name).isWord(u"struct"))) {
        ++name;
    }
    return at(name).kind == Kind::Word && !reservedWords().contains(at(name).text);
}

void Parser::parseDeclarations(qsizetype begin, qsizetype end, QString ns) {
    qsizetype declarationStart = -1;
    qsizetype i = begin;
    while (i < end) {
        const SourceToken &t = m_t.at(i);
        if (t.kind == Kind::Directive) {
            ++i;
            continue;
        }
        if (declarationStart < 0) {
            declarationStart = i;
        }
        if (t.is(u"[")) {
            i = matching(i, end) + 1;   // atribut tipe, atau [assembly: ...]
            continue;
        }
        if (t.isWord(u"using")) {
            // "using System.Text;" → akar namespace, supaya "System.Text.Encoding" tidak dianggap tipe
            qsizetype name = i + 1;
            if (at(name).isWord(u"static")) {
                ++name;
            }
            if (at(name).kind == Kind::Word && !at(name + 1).is(u"=")) {
                m_namespaceRoots.insert(at(name).text);
            }
            i = statementEnd(i, end);
            declarationStart = -1;
            continue;
        }
        if (t.isWord(u"namespace")) {
            QString name;
            qsizetype k = i + 1;
            while (k < end && (at(k).kind == Kind::Word || at(k).is(u"."))) {
                name += at(k).text;
                ++k;
            }
            m_namespaceRoots.insert(name.section(QLatin1Char('.'), 0, 0));
            const QString inner = ns.isEmpty() ? name : ns + QLatin1Char('.') + name;
            if (at(k).is(u"{")) {
                const qsizetype close = matching(k, end);
                parseDeclarations(k + 1, close, inner);
                i = close + 1;
            } else {
                ns = inner;   // namespace file-scoped: berlaku sampai akhir file
                i = k + 1;
            }
            declarationStart = -1;
            continue;
        }
        if (t.kind == Kind::Word && typeWords().contains(t.text) && isTypeDeclaration(i)) {
            i = parseType(declarationStart, i, end, ns, nullptr);
            declarationStart = -1;
            continue;
        }
        static const QSet<QString> modifiers = {"public", "private", "protected", "internal", "static", "sealed",
                                                "abstract", "partial", "file", "unsafe", "new", "readonly", "ref"};
        if (t.kind == Kind::Word && modifiers.contains(t.text)) {
            ++i;
            continue;
        }
        // delegate, pernyataan top-level, dll.
        i = statementEnd(i, end);
        declarationStart = -1;
    }
}

qsizetype Parser::parseType(qsizetype start, qsizetype keyword, qsizetype end, const QString &ns, TypeContext *outer) {
    TypeMetrics metrics;
    metrics.kind = m_t.at(keyword).text;
    qsizetype k = keyword + 1;
    if (metrics.kind == u"record" && (at(k).isWord(u"class") || at(k).isWord(u"struct"))) {
        if (at(k).isWord(u"struct")) {
            metrics.kind = QStringLiteral("record struct");
        }
        ++k;
    }

    TypeContext type;
    type.simpleName = at(k).text;
    type.namespaceName = outer ? outer->namespaceName : ns;
    type.name = outer ? outer->name + QLatin1Char('.') + type.simpleName : type.simpleName;
    if (outer) {
        type.excluded = outer->excluded;
        outer->excluded.insert(type.simpleName);
    }
    type.excluded.insert(type.simpleName);
    ++k;

    if (at(k).is(u"<")) {
        const qsizetype close = genericEnd(k, end);
        for (qsizetype g = k + 1; g < (close > 0 ? close - 1 : k); ++g) {
            if (at(g).kind == Kind::Word && !at(g).isWord(u"in") && !at(g).isWord(u"out")) {
                type.excluded.insert(at(g).text);
            }
        }
        k = close > 0 ? close : k + 1;
    }
    if (at(k).is(u"(")) {
        k = matching(k, end) + 1;   // primary constructor / record posisional
    }
    for (qsizetype a = start; a < keyword; ++a) {
        if (at(a).is(u"[")) {
            const qsizetype close = matching(a, keyword);
            type.coupled += attributeNames(a, close);
            a = close;
        }
    }

    if (at(k).is(u":")) {
        ++k;
        bool first = true;
        while (k < end && !at(k).is(u"{") && !at(k).is(u";") && !at(k).isWord(u"where")) {
            QString last;
            while (k < end && (at(k).kind == Kind::Word || at(k).is(u".") || at(k).is(u"::"))) {
                if (at(k).kind == Kind::Word) {
                    last = at(k).text;
                }
                ++k;
            }
            if (at(k).is(u"<")) {
                const qsizetype close = genericEnd(k, end);
                k = close > 0 ? close : k + 1;
            }
            if (at(k).is(u"(")) {
                k = matching(k, end) + 1;   // "class Foo(int x) : Bar(x)"
            }
            if (!last.isEmpty()) {
                type.coupled.insert(last);
                const bool classKind = metrics.kind == u"class" || metrics.kind == u"record";
                if (first && classKind && !looksLikeInterface(last)) {
                    metrics.baseClass = last;
                }
            }
            first = false;
            if (at(k).is(u",") || (!at(k).is(u"{") && !at(k).is(u";") && !at(k).isWord(u"where"))) {
                ++k;
            }
        }
    }
    while (k < end && !at(k).is(u"{") && !at(k).is(u";")) {
        ++k;   // klausa where
    }

    // Slot dipesan dulu supaya tipe luar tercatat sebelum tipe bersarangnya
    type.index = int(m_types.size());
    m_types.append(TypeMetrics());

    qsizetype typeEnd = k;
    QList<PendingMember> members;
    if (at(k).is(u"{")) {
        const qsizetype close = matching(k, end);
        if (metrics.kind != u"enum") {
            members = parseMembers(k + 1, close, type);
        }
        typeEnd = at(close + 1).is(u";") ? close + 1 : close;
    }

    // Token milik tipe ini sendiri, tanpa tipe bersarang
    QList<TokenRange> own;
    qsizetype cursor = start;
    for (const TokenRange &nested : std::as_const(type.nestedSpans)) {
        own.append({cursor, nested.first});
        cursor = nested.second;
    }
    own.append({cursor, typeEnd + 1});

    QSet<QString> coupled = type.coupled;
    for (const TokenRange &range : std::as_const(own)) {
        collectTypes(range, type, type.excluded, &coupled);
    }

    QList<TokenRange> memberRanges;
    int memberLines = 0;
    for (PendingMember &member : members) {
        QSet<QString> used = member.attributes;
        const QSet<QString> excluded = type.excluded + member.generics;
        collectTypes(member.range, type, excluded, &used);
        used -= excluded;
        member.metrics.coupling = int(used.size());
        coupled += member.attributes;

        metrics.complexity += member.metrics.complexity;
        memberLines += member.metrics.lines;
        memberRanges.append(member.range);
        metrics.members.append(member.metrics);
    }
    coupled -= type.excluded;
    metrics.coupling = int(coupled.size());
    metrics.volume = SourceTokens::halsteadVolume(m_t, memberRanges);
    metrics.maintainability = CodeMetrics::maintainabilityIndex(metrics.volume, metrics.complexity, memberLines,
                                                                int(metrics.members.size()));
    metrics.lines = SourceTokens::codeLines(m_t, {{start, typeEnd + 1}});
    metrics.namespaceName = type.namespaceName;
    metrics.name = type.name;
    m_types[type.index] = metrics;
    return typeEnd + 1;
}

QList<PendingMember> Parser::parseMembers(qsizetype begin, qsizetype end, TypeContext &type) {
    QList<PendingMember> members;
    qsizetype i = begin;
    while (i < end) {
        if (m_t.at(i).kind == Kind::Directive || m_t.at(i).is(u";")) {
            ++i;
            continue;
        }
        const qsizetype start = i;
        QSet<QString> attributes;
        while (i < end && m_t.at(i).is(u"[")) {
            const qsizetype close = matching(i, end);
            attributes += attributeNames(i, close);
            i = close + 1;
        }

        // Ujung header: "{", "=>", ";", atau "=" di luar kurung; deklarasi tipe bersarang didahulukan
        qsizetype terminator = -1;
        qsizetype nestedKeyword = -1;
        int depth = 0;
        for (qsizetype k = i; k < end; ++k) {
            const SourceToken &t = m_t.at(k);
            if (t.is(u"(") || t.is(u"[")) {
                ++depth;
            } else if (t.is(u")") || t.is(u"]")) {
                --depth;
            } else if (depth == 0) {
                if (t.kind == Kind::Word && typeWords().contains(t.text) && isTypeDeclaration(k)) {
                    nestedKeyword = k;
                    break;
                }
                if (t.is(u"{") || t.is(u"=>") || t.is(u";") || t.is(u"=")) {
                    terminator = k;
                    break;
                }
            }
        }
        if (nestedKeyword >= 0) {
            const qsizetype nestedEnd = parseType(start, nestedKeyword, end, type.namespaceName, &type);
            type.nestedSpans.append({start, nestedEnd});
            i = nestedEnd;
            continue;
        }
        if (terminator < 0) {
            break;
        }

        // Jenis member dari header
        qsizetype operatorWord = -1;
        qsizetype indexerWord = -1;
        qsizetype parameters = -1;
        bool isStatic = false;
        depth = 0;
        for (qsizetype k = i; k < terminator; ++k) {
            const SourceToken &t = m_t.at(k);
            if (t.isWord(u"operator") && operatorWord < 0) {
                operatorWord = k;
            } else if (t.isWord(u"this") && at(k + 1).is(u"[") && depth == 0) {
                indexerWord = k;
            } else if (t.isWord(u"static")) {
                isStatic = true;
            }
            if (t.is(u"(") && depth == 0 && parameters < 0 && operatorWord < 0) {
                // Daftar parameter = "(" yang didahului nama (boleh dengan argumen generic)
                qsizetype name = k - 1;
                if (at(name).is(u">")) {
                    int angle = 0;
                    for (; name >= i; --name) {
                        if (at(name).is(u">")) {
                            ++angle;
                        } else if (at(name).is(u"<") && --angle == 0) {
                            break;
                        }
                    }
                    --name;
                }
                if (at(name).kind == Kind::Word && !reservedWords().contains(at(name).text)) {
                    parameters = k;
                }
            }
            if (t.is(u"(") || t.is(u"[")) {
                ++depth;
            } else if (t.is(u")") || t.is(u"]")) {
                --depth;
            }
        }
        if (operatorWord >= 0) {
            for (qsizetype k = operatorWord + 1; k < terminator; ++k) {
                if (m_t.at(k).is(u"(")) {
                    parameters = k;
                    break;
                }
            }
        }

        const SourceToken &ending = m_t.at(terminator);
        const bool methodLike = parameters >= 0 && indexerWord < 0;
        qsizetype memberEnd = terminator;
        bool hasCode = false;
        if (ending.is(u"{")) {
            const qsizetype close = matching(terminator, end);
            memberEnd = close;
            if (methodLike) {
                hasCode = true;
            } else {
                // Property/indexer/event berkode bila ada accessor berbadan ("get { }", "get =>")
                for (qsizetype k = terminator + 1; k < close && !hasCode; ++k) {
                    hasCode = m_t.at(k).is(u"{") || m_t.at(k).is(u"=>");
                }
                if (at(close + 1).is(u"=")) {
                    memberEnd = expressionEnd(close + 1, end);   // "{ get; set; } = nilai;"
                }
            }
        } else if (ending.is(u"=>")) {
            memberEnd = expressionEnd(terminator, end);
            hasCode = true;
        } else if (ending.is(u"=")) {
            memberEnd = expressionEnd(terminator, end);   // field/property dengan inisialisasi
        }

        PendingMember member;
        if (parameters >= 0) {
            const qsizetype close = matching(parameters, end);
            const QString list = parameterTypes(parameters, close);
            if (operatorWord >= 0) {
                QString symbol;
                for (qsizetype k = operatorWord + 1; k < parameters; ++k) {
                    symbol += m_t.at(k).text;
                }
                member.metrics.name = QStringLiteral("operator %1(%2)").arg(symbol, list);
            } else {
                qsizetype name = parameters - 1;
                if (at(name).is(u">")) {
                    int angle = 0;
                    for (; name >= i; --name) {
                        if (at(name).is(u">")) {
                            ++angle;
                        } else if (at(name).is(u"<") && --angle == 0) {
                            break;
                        } else if (at(name).kind == Kind::Word) {
                            member.generics.insert(at(name).text);
                        }
                    }
                    --name;
                }
                const QString base = at(name).text;
                type.memberNames.insert(base);
                QString prefix;
                if (at(name - 1).is(u"~")) {
                    prefix = QStringLiteral("~");
                } else if (isStatic && base == type.simpleName) {
                    prefix = QStringLiteral("static ");
                }
                member.metrics.name = QStringLiteral("%1%2(%3)").arg(prefix, base, list);
            }
        } else if (indexerWord >= 0) {
            const qsizetype open = indexerWord + 1;
            member.metrics.name = QStringLiteral("this[%1]").arg(parameterTypes(open, matching(open, end)));
        } else {
            for (qsizetype k = terminator - 1; k >= i; --k) {
                if (m_t.at(k).kind == Kind::Word && !reservedWords().contains(m_t.at(k).text)) {
                    member.metrics.name = m_t.at(k).text;
                    break;
                }
            }
            type.memberNames.insert(member.metrics.name);
        }

        if (hasCode) {
            member.range = {start, memberEnd + 1};
            member.attributes = attributes;
            member.metrics.complexity = complexity(member.range);
            member.metrics.lines = SourceTokens::codeLines(m_t, {member.range});
            member.metrics.volume = SourceTokens::halsteadVolume(m_t, {member.range});
            member.metrics.maintainability = CodeMetrics::maintainabilityIndex(
                member.metrics.volume, member.metrics.complexity, member.metrics.lines, 1);
            members.append(member);
        } else {
            type.coupled += attributes;
        }
        i = memberEnd + 1;
    }
    return members;
}

QSet<QString> Parser::attributeNames(qsizetype open, qsizetype close) const {
    QSet<QString> names;
    int depth = 0;
    bool expectName = true;
    for (qsizetype k = open + 1; k < close; ++k) {
        const SourceToken &t = m_t.at(k);
        if (t.is(u"(")) {
            ++depth;
        } else if (t.is(u")")) {
            --depth;
        } else if (depth == 0 && (t.is(u",") || t.is(u":"))) {
            expectName = true;   // atribut berikutnya, atau setelah target "return:" / "assembly:"
        } else if (depth == 0 && expectName && t.kind == Kind::Word) {
            qsizetype last = k;
            while (at(last + 1).is(u".") && at(last + 2).kind == Kind::Word) {
                last += 2;
            }
            if (!at(last + 1).is(u":") && isTypeName(at(last).text)) {
                names.insert(at(last).text);
            }
            k = last;
            expectName = false;
        }
    }
    return names;
}

// "(int count, [FromBody] Order order = null)" → "int, Order"
QString Parser::parameterTypes(qsizetype open, qsizetype close) const {
    static const QSet<QString> modifiers = {"this", "ref", "out", "in", "params", "scoped", "readonly"};
    QStringList types;
    QList<qsizetype> current;
    auto flush = [&]() {
        qsizetype first = 0;
        while (first < current.size() && m_t.at(current.at(first)).is(u"[")) {
            int depth = 0;
            for (; first < current.size(); ++first) {
                const SourceToken &t = m_t.at(current.at(first));
                if (t.is(u"[")) {
                    ++depth;
                } else if (t.is(u"]") && --depth == 0) {
                    ++first;
                    break;
                }
            }
        }
        while (first < current.size() && modifiers.contains(m_t.at(current.at(first)).text)) {
            ++first;
        }
        qsizetype stop = current.size();
        for (qsizetype j = first; j < stop; ++j) {
            if (m_t.at(current.at(j)).is(u"=")) {
                stop = j;
                break;
            }
        }
        if (stop - 1 > first && m_t.at(current.at(stop - 1)).kind == Kind::Word) {
            --stop;   // nama parameter
        }
        QString text;
        for (qsizetype j = first; j < stop; ++j) {
            const SourceToken &t = m_t.at(current.at(j));
            text += t.text;
            if (t.is(u",")) {
                text += QLatin1Char(' ');
            }
        }
        if (!text.isEmpty()) {
            types.append(text);
        }
        current.clear();
    };

    int depth = 0;
    for (qsizetype k = open + 1; k < close; ++k) {
        const SourceToken &t = m_t.at(k);
        if (t.is(u"(") || t.is(u"[") || t.is(u"<")) {
            ++depth;
        } else if (t.is(u")") || t.is(u"]") || t.is(u">")) {
            --depth;
        } else if (depth == 0 && t.is(u",")) {
            flush();
            continue;
        }
        current.append(k);
    }
    flush();
    return types.join(QStringLiteral(", "));
}

// Kompleksitas siklomatik: 1 + if, perulangan, case, catch, filter "when", pola and/or, &&, ||,
// ??, ??=, ?., ?[, ternary, dan tiap arm switch expression selain "_"
int Parser::complexity(TokenRange range) const {
    int count = 1;
    for (qsizetype i = range.first; i < range.second; ++i) {
        const SourceToken &t = m_t.at(i);
        if (t.kind == Kind::Word) {
            if (decisionWords().contains(t.text)) {
                ++count;
            } else if (t.text == u"switch" && at(i + 1).is(u"{")) {
                count += switchArms(i + 1, range.second);
            }
        } else if (t.kind == Kind::Symbol) {
            if (t.is(u"&&") || t.is(u"||") || t.is(u"??") || t.is(u"?\?=") || t.is(u"?.")) {
                ++count;
            } else if (t.is(u"?")) {
                const bool conditionalIndex = at(i + 1).is(u"[") && !at(i + 2).is(u"]") && !at(i + 2).is(u",");
                if (conditionalIndex || isTernary(i, range.second)) {
                    ++count;
                }
            }
        }
    }
    return count;
}

// "cond ? a : b" punya ':' pasangannya sebelum pernyataan/argumen berakhir; "int? x = ..." tidak
bool Parser::isTernary(qsizetype question, qsizetype end) const {
    int depth = 0;
    int nested = 0;
    for (qsizetype i = question + 1; i < end; ++i) {
        const SourceToken &t = m_t.at(i);
        if (t.is(u"(") || t.is(u"[")) {
            ++depth;
        } else if (t.is(u")") || t.is(u"]")) {
            if (--depth < 0) {
                return false;
            }
        } else if (depth == 0) {
            if (t.is(u"?")) {
                ++nested;
            } else if (t.is(u":")) {
                if (nested == 0) {
                    return true;
                }
                --nested;
            } else if (t.is(u";") || t.is(u",") || t.is(u"{") || t.is(u"}") || t.is(u"=>") || t.is(u"=")) {
                return false;
            }
        }
    }
    return false;
}

int Parser::switchArms(qsizetype open, qsizetype end) const {
    const qsizetype close = matching(open, end);
    int arms = 0;
    int depth = 0;
    qsizetype armStart = open + 1;
    for (qsizetype i = open + 1; i < close; ++i) {
        const SourceToken &t = m_t.at(i);
        if (t.is(u"(") || t.is(u"[") || t.is(u"{")) {
            ++depth;
        } else if (t.is(u")") || t.is(u"]") || t.is(u"}")) {
            --depth;
        } else if (depth == 0 && t.is(u",")) {
            armStart = i + 1;
        } else if (depth == 0 && t.is(u"=>")) {
            const bool discard = i == armStart + 1 && m_t.at(armStart).isWord(u"_");
            if (!discard) {
                ++arms;
            }
        }
    }
    return arms;
}

// Class coupling: nama tipe di posisi tipe (new/as/is/typeof/catch, deklarasi "Tipe nama",
// argumen generic, cast, akses statis "Tipe.Member"). Tanpa kompilasi, jadi perkiraan.
void Parser::collectTypes(TokenRange range, const TypeContext &type, const QSet<QString> &excluded,
                          QSet<QString> *out) const {
    for (qsizetype i = range.first; i < range.second; ++i) {
        const SourceToken &t = m_t.at(i);
        if (t.kind != Kind::Word || !isTypeName(t.text) || excluded.contains(t.text)) {
            continue;
        }
        const SourceToken &previous = at(i - 1);
        if (previous.is(u".") || previous.is(u"?.") || previous.is(u"::")) {
            continue;   // segmen nama bertingkat atau akses member
        }
        qsizetype next = i + 1;
        if (at(next).is(u"<")) {
            const qsizetype close = genericEnd(next, range.second);
            if (close > 0) {
                for (qsizetype g = next + 1; g < close - 1; ++g) {
                    const SourceToken &argument = m_t.at(g);
                    if (argument.kind == Kind::Word && isTypeName(argument.text) && !excluded.contains(argument.text)
                        && !at(g - 1).is(u".")) {
                        out->insert(argument.text);
                    }
                }
                next = close;
            }
        }

        bool used = false;
        const SourceToken &beforeParen = at(i - 2);
        if (previous.isWord(u"new") || previous.isWord(u"as") || previous.isWord(u"is")) {
            used = true;
        } else if (previous.is(u"(") && (beforeParen.isWord(u"typeof") || beforeParen.isWord(u"default")
                                          || beforeParen.isWord(u"catch") || beforeParen.isWord(u"sizeof"))) {
            used = true;
        } else {
            qsizetype k = next;
            if (at(k).is(u"?")) {
                if (isTernary(k, range.second)) {
                    continue;   // "IsVip ? Hitung(x) : 0": properti dalam ternary, bukan tipe nullable
                }
                ++k;
            }
            while (at(k).is(u"[") && (at(k + 1).is(u"]") || at(k + 1).is(u","))) {
                k = matching(k, range.second) + 1;
            }
            const SourceToken &after = at(k);
            if (after.kind == Kind::Word && !reservedWords().contains(after.text) && !clauseWords().contains(after.text)) {
                used = true;   // deklarasi "Order order", "Order? order", "Order[] orders", "Order Place("
            } else if (k == i + 1 && after.is(u".")) {
                used = !type.memberNames.contains(t.text) && !m_namespaceRoots.contains(t.text);
            } else if (k == i + 1 && after.is(u")") && previous.is(u"(")) {
                // Cast "(Order)x": kurungnya bukan milik panggilan fungsi / pernyataan kontrol
                const bool call = beforeParen.kind == Kind::Word
                                      ? !(beforeParen.isWord(u"return") || beforeParen.isWord(u"await")
                                          || beforeParen.isWord(u"throw") || beforeParen.isWord(u"yield"))
                                      : (beforeParen.is(u")") || beforeParen.is(u"]") || beforeParen.is(u">"));
                const SourceToken &operand = at(k + 1);
                used = !call && (operand.kind == Kind::Word || operand.kind == Kind::Number
                                 || operand.kind == Kind::Text || operand.is(u"("));
            }
        }
        if (used) {
            out->insert(t.text);
        }
    }
}

int classDepth(const QString &name, const QHash<QString, QString> &bases, bool *open, int guard) {
    if (name.isEmpty()) {
        return 0;   // System.Object
    }
    if (guard > 32) {
        *open = true;   // rantai melingkar
        return 1;
    }
    const auto project = bases.constFind(name);
    if (project != bases.cend()) {
        return 1 + classDepth(*project, bases, open, guard + 1);
    }
    const auto known = frameworkDepths().constFind(name);
    if (known != frameworkDepths().cend()) {
        return *known;
    }
    *open = true;   // kelas dari luar project: minimal kelas itu sendiri
    return 1;
}

}

QList<TypeMetrics> CSharpMetrics::analyze(const QList<SourceToken> &tokens) {
    QList<TypeMetrics> types = Parser(tokens).run();
    QHash<QString, QString> bases;
    for (const TypeMetrics &type : std::as_const(types)) {
        bases.insert(type.name.section(QLatin1Char('.'), -1), type.baseClass);
    }
    resolveInheritance(&types, bases);
    return types;
}

QHash<QString, QString> CSharpMetrics::declaredBases(const QString &source) {
    static const QRegularExpression declaration(QStringLiteral(
        R"(\b(class|interface|struct|record(?:\s+(?:class|struct))?)\s+(@?[A-Za-z_]\w*)\s*)"
        R"((?:<[^<>;{}]*(?:<[^<>;{}]*>[^<>;{}]*)*>\s*)?(?:\([^)]*\)\s*)?(?::\s*([A-Za-z_][\w.]*))?)"));
    QHash<QString, QString> bases;
    QRegularExpressionMatchIterator it = declaration.globalMatch(source);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        const QString kind = match.captured(1);
        const QString name = match.captured(2);
        QString base = match.captured(3).section(QLatin1Char('.'), -1);
        const bool classKind = kind == u"class" || (kind.startsWith(u"record") && !kind.endsWith(u"struct"));
        if (!classKind || looksLikeInterface(base)) {
            base.clear();
        }
        // Kelas partial: base list cukup ada di salah satu bagiannya
        if (bases.value(name).isEmpty()) {
            bases.insert(name, base);
        }
    }
    return bases;
}

void CSharpMetrics::resolveInheritance(QList<TypeMetrics> *types, const QHash<QString, QString> &projectBases) {
    for (TypeMetrics &type : *types) {
        type.inheritanceOpen = false;
        if (type.kind == u"interface") {
            type.inheritanceDepth = 0;
        } else if (type.kind == u"enum") {
            type.inheritanceDepth = 3;   // Enum → ValueType → Object
        } else if (type.kind == u"struct" || type.kind == u"record struct") {
            type.inheritanceDepth = 2;   // ValueType → Object
        } else {
            type.inheritanceDepth = 1 + classDepth(type.baseClass, projectBases, &type.inheritanceOpen, 0);
        }
    }
}

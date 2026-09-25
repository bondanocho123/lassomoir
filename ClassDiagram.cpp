#include "ClassDiagram.h"
#include "WorkspaceDiff.h"

#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>

#include <algorithm>

namespace {

constexpr int kMaxDrawnTypes = 12;     // tipe yang digambar lengkap dengan anggotanya
constexpr int kMaxContextTypes = 10;   // tipe terkait yang digambar tanpa anggota
constexpr int kMaxMembers = 14;        // anggota per kelas; sisanya diringkas

enum class Status { Added, Changed, Unchanged, Context };

struct Node {
    QString id;
    QString label;                         // nama tampilan bila berbeda dari id (tipe bersarang)
    QList<const TypeMetrics *> parts;      // lebih dari satu untuk kelas partial
    Status status = Status::Context;
};

struct Edge {
    QString from;
    QString arrow;
    QString to;
    QString label;
    bool many = false;
};

// Mermaid menulis generic sebagai ~T~, tapi salah membaca "~" bersarang bila ada koma
// ("Dictionary~string, List~Order~~"). Jadi hanya tingkat terluar yang memakai ~; tingkat lebih
// dalam memakai ‹›. Tuple "(int, string)" ditulis Tuple~int, string~.
QString mermaidType(const QString &text) {
    QString result;
    int depth = 0;
    for (const QChar ch : text) {
        if (ch == QLatin1Char('@')) {
            continue;
        }
        if (ch == QLatin1Char('<') || ch == QLatin1Char('(')) {
            ++depth;
            if (ch == QLatin1Char('(')) {
                result += QStringLiteral("Tuple");
            }
            result += depth == 1 ? QChar(u'~') : QChar(u'‹');
        } else if (ch == QLatin1Char('>') || ch == QLatin1Char(')')) {
            result += depth == 1 ? QChar(u'~') : QChar(u'›');
            depth = qMax(0, depth - 1);
        } else {
            result += ch;
        }
    }
    return result;
}

QString nodeId(const QString &name) {
    QString id = name;
    id.replace(QLatin1Char('.'), QLatin1Char('_'));
    id.remove(QLatin1Char('@'));
    return id;
}

QString simpleName(const TypeMetrics &type) {
    return type.name.section(QLatin1Char('.'), -1);
}

bool looksLikeInterface(const QString &name) {
    return name.size() > 1 && name.at(0) == QLatin1Char('I') && name.at(1).isUpper();
}

// Isi tipe untuk menandai tipe yang berubah: anggota beserta metrik member berkode
QString fingerprint(const TypeMetrics &type) {
    QStringList parts = {type.kind, type.baseClass, type.interfaces.join(QLatin1Char(',')),
                         type.genericParameters.join(QLatin1Char(','))};
    for (const TypeMember &member : type.outline) {
        parts << QStringList{QString::number(int(member.kind)), QString(member.visibility),
                             member.isStatic ? QStringLiteral("s") : QString(),
                             member.isAbstract ? QStringLiteral("a") : QString(), member.name, member.type,
                             member.parameters}
                     .join(QLatin1Char('|'));
    }
    for (const MemberMetrics &member : type.members) {
        parts << QStringList{member.name, QString::number(member.complexity), QString::number(member.lines),
                             QString::number(qRound(member.volume))}
                     .join(QLatin1Char('|'));
    }
    return parts.join(QLatin1Char('\n'));
}

// Nama-nama di teks tipe ("Dictionary<string, List<Order>>" → Dictionary, string, List, Order)
QStringList typeNames(const QString &text) {
    static const QRegularExpression identifier(QStringLiteral(R"([A-Za-z_]\w*)"));
    QStringList names;
    QRegularExpressionMatchIterator it = identifier.globalMatch(text);
    while (it.hasNext()) {
        names.append(it.next().captured(0));
    }
    return names;
}

bool isCollection(const QString &name) {
    static const QSet<QString> names = {"List", "IList", "ICollection", "IEnumerable", "IReadOnlyList",
                                        "IReadOnlyCollection", "HashSet", "ISet", "Collection", "Dictionary",
                                        "IDictionary", "IReadOnlyDictionary", "ObservableCollection", "Queue",
                                        "Stack", "LinkedList", "ImmutableList", "ImmutableArray", "SortedList",
                                        "SortedSet", "ConcurrentDictionary", "ConcurrentBag"};
    return names.contains(name);
}

QString annotation(const TypeMetrics &type) {
    if (type.kind == u"interface") return QStringLiteral("<<interface>>");
    if (type.kind == u"enum") return QStringLiteral("<<enumeration>>");
    if (type.kind == u"struct" || type.kind == u"record struct") return QStringLiteral("<<struct>>");
    if (type.kind == u"record") return QStringLiteral("<<record>>");
    if (type.isStatic) return QStringLiteral("<<static>>");
    if (type.isAbstract) return QStringLiteral("<<abstract>>");
    return QString();
}

QString memberLine(const TypeMember &member) {
    const QString visibility = member.visibility.isNull() ? QString() : QString(member.visibility);
    switch (member.kind) {
    case TypeMember::Kind::EnumValue:
        return member.name;
    case TypeMember::Kind::Constructor:
        return QStringLiteral("%1%2(%3)%4").arg(visibility, member.name, mermaidType(member.parameters),
                                               member.isStatic ? QStringLiteral("$") : QString());
    case TypeMember::Kind::Method: {
        const QString marker = member.isAbstract ? QStringLiteral("*")
                               : member.isStatic ? QStringLiteral("$")
                                                 : QString();
        return QStringLiteral("%1%2(%3)%4 %5")
            .arg(visibility, member.name, mermaidType(member.parameters), marker, mermaidType(member.type))
            .trimmed();
    }
    case TypeMember::Kind::Field:
    case TypeMember::Kind::Property:
    case TypeMember::Kind::Event:
        break;
    }
    const QString name = member.name == u"this" ? QStringLiteral("this[%1]").arg(mermaidType(member.parameters))
                                                : member.name;
    return QStringLiteral("%1%2 %3%4").arg(visibility, mermaidType(member.type), name,
                                           member.isStatic ? QStringLiteral("$") : QString());
}

QString styleFor(Status status) {
    switch (status) {
    case Status::Added: return QStringLiteral("fill:#e6ffec,stroke:#2f7d32");
    case Status::Changed: return QStringLiteral("fill:#fbeccf,stroke:#b7791f");
    case Status::Context: return QStringLiteral("fill:#f4f1ec,stroke:#b9b0a3,color:#6f6557");
    case Status::Unchanged: break;
    }
    return QString();
}

}

ClassDiagram ClassDiagram::fromDiff(const WorkspaceDiff &diff) {
    ClassDiagram result;

    QHash<QString, QString> previous;   // nama lengkap → isi tipe sebelum perubahan
    for (const FileDiff &file : diff.files) {
        if (file.before) {
            for (const TypeMetrics &type : file.before->types) {
                previous.insert(type.fullName(), fingerprint(type));
            }
        }
    }

    // Tipe di file yang berubah; bagian kelas partial digabung ke satu node
    QList<Node> nodes;
    QHash<QString, qsizetype> byFullName;
    for (const FileDiff &file : diff.files) {
        if (!file.after) {
            continue;
        }
        for (const TypeMetrics &type : file.after->types) {
            const auto existing = byFullName.constFind(type.fullName());
            if (existing != byFullName.cend()) {
                nodes[*existing].parts.append(&type);
                continue;
            }
            Node node;
            node.id = nodeId(type.name);
            node.label = type.name;
            node.parts = {&type};
            const auto old = previous.constFind(type.fullName());
            node.status = old == previous.cend()          ? Status::Added
                          : *old == fingerprint(type) ? Status::Unchanged
                                                      : Status::Changed;
            byFullName.insert(type.fullName(), nodes.size());
            nodes.append(node);
        }
    }
    if (nodes.isEmpty()) {
        return result;
    }

    // Tipe baru dan berubah didahulukan bila diagram harus dipangkas
    std::stable_sort(nodes.begin(), nodes.end(), [](const Node &a, const Node &b) {
        return int(a.status) < int(b.status);
    });
    if (nodes.size() > kMaxDrawnTypes) {
        result.omittedTypes = int(nodes.size()) - kMaxDrawnTypes;
        nodes.resize(kMaxDrawnTypes);
    }
    result.drawnTypes = int(nodes.size());

    // Id unik: tipe bernama sama di namespace berbeda diberi akhiran
    QSet<QString> usedIds;
    QHash<QString, QString> idByName;   // nama sederhana → id node
    for (Node &node : nodes) {
        QString id = node.id;
        for (int suffix = 2; usedIds.contains(id); ++suffix) {
            id = node.id + QStringLiteral("_%1").arg(suffix);
        }
        node.id = id;
        usedIds.insert(id);
        idByName.insert(simpleName(*node.parts.first()), id);
    }

    QList<Node> context;
    // Id node tujuan; tipe di luar diagram dibuat sebagai node konteks selama masih ada tempat
    auto target = [&](const QString &name) -> QString {
        const auto found = idByName.constFind(name);
        if (found != idByName.cend()) {
            return *found;
        }
        if (context.size() >= kMaxContextTypes) {
            return QString();
        }
        QString id = nodeId(name);
        for (int suffix = 2; usedIds.contains(id); ++suffix) {
            id = nodeId(name) + QStringLiteral("_%1").arg(suffix);
        }
        usedIds.insert(id);
        idByName.insert(name, id);
        Node node;
        node.id = id;
        node.label = name;
        context.append(node);
        return id;
    };
    auto known = [&](const QString &name) {
        return idByName.contains(name) || diff.csharpTypes.contains(name);
    };

    QList<Edge> edges;
    QSet<QString> linked;   // "dari→ke": satu garis per pasangan tipe
    auto addEdge = [&](const QString &from, const QString &arrow, const QString &to, const QString &label, bool many) {
        if (to.isEmpty() || from == to || linked.contains(from + QStringLiteral("→") + to)) {
            return;
        }
        linked.insert(from + QStringLiteral("→") + to);
        edges.append({from, arrow, to, label, many});
    };

    for (const Node &node : std::as_const(nodes)) {
        const TypeMetrics &type = *node.parts.first();
        QSet<QString> generics(type.genericParameters.cbegin(), type.genericParameters.cend());
        for (const TypeMetrics *part : node.parts) {
            if (!part->baseClass.isEmpty()) {
                // Kelas dasar selalu digambar, termasuk dari framework (ControllerBase, MonoBehaviour)
                addEdge(target(part->baseClass), QStringLiteral("<|--"), node.id, QString(), false);
            }
            for (const QString &contract : part->interfaces) {
                if (known(contract)) {
                    const QString arrow = type.kind == u"interface" ? QStringLiteral("<|--") : QStringLiteral("<|..");
                    addEdge(target(contract), arrow, node.id, QString(), false);
                }
            }
        }
        // Asosiasi lewat field/property; dependensi lewat parameter dan tipe kembalian
        for (const TypeMetrics *part : node.parts) {
            for (const TypeMember &member : part->outline) {
                const bool attribute = member.kind == TypeMember::Kind::Field
                                       || member.kind == TypeMember::Kind::Property
                                       || member.kind == TypeMember::Kind::Event;
                if (!attribute) {
                    continue;
                }
                const QStringList names = typeNames(member.type);
                const bool many = member.type.contains(QStringLiteral("[]"))
                                  || (!names.isEmpty() && isCollection(names.first()));
                for (qsizetype n = many && !member.type.contains(QStringLiteral("[]")) ? 1 : 0; n < names.size(); ++n) {
                    const QString &name = names.at(n);
                    if (!generics.contains(name) && known(name)) {
                        addEdge(node.id, QStringLiteral("-->"), target(name), member.name, many);
                    }
                    if (!many) {
                        break;   // "IRepository<Order>": yang dirujuk IRepository, bukan argumennya
                    }
                }
            }
        }
        for (const TypeMetrics *part : node.parts) {
            for (const TypeMember &member : part->outline) {
                if (member.kind != TypeMember::Kind::Method && member.kind != TypeMember::Kind::Constructor) {
                    continue;
                }
                for (const QString &name : typeNames(member.parameters + QLatin1Char(' ') + member.type)) {
                    if (!generics.contains(name) && known(name)) {
                        addEdge(node.id, QStringLiteral("..>"), target(name), QString(), false);
                    }
                }
            }
        }
    }

    QStringList lines = {QStringLiteral("classDiagram")};
    for (const Node &node : std::as_const(nodes)) {
        const TypeMetrics &type = *node.parts.first();
        QString header = QStringLiteral("class %1").arg(node.id);
        if (!type.genericParameters.isEmpty() && node.label == node.id) {
            header += QStringLiteral("~%1~").arg(type.genericParameters.join(QStringLiteral(", ")));
        } else if (node.label != node.id) {
            header += QStringLiteral("[\"%1\"]").arg(node.label);
        }
        lines << QStringLiteral("    %1 {").arg(header);
        const QString marker = annotation(type);
        if (!marker.isEmpty()) {
            lines << QStringLiteral("        %1").arg(marker);
        }
        int shown = 0;
        int hidden = 0;
        for (const TypeMetrics *part : node.parts) {
            for (const TypeMember &member : part->outline) {
                if (shown < kMaxMembers) {
                    lines << QStringLiteral("        %1").arg(memberLine(member));
                    ++shown;
                } else {
                    ++hidden;
                }
            }
        }
        if (hidden > 0) {
            lines << QStringLiteral("        … %1 anggota lain").arg(hidden);
        }
        lines << QStringLiteral("    }");
    }
    for (const Node &node : std::as_const(context)) {
        if (looksLikeInterface(node.label)) {
            lines << QStringLiteral("    class %1 {").arg(node.id) << QStringLiteral("        <<interface>>")
                  << QStringLiteral("    }");
        } else {
            lines << QStringLiteral("    class %1").arg(node.id);
        }
    }
    for (const Edge &edge : std::as_const(edges)) {
        QString line = QStringLiteral("    %1 %2 %3%4").arg(edge.from, edge.arrow,
                                                          edge.many ? QStringLiteral("\"*\" ") : QString(), edge.to);
        if (!edge.label.isEmpty()) {
            line += QStringLiteral(" : %1").arg(edge.label);
        }
        lines << line;
    }
    for (const Node &node : std::as_const(nodes)) {
        const QString style = styleFor(node.status);
        if (!style.isEmpty()) {
            lines << QStringLiteral("    style %1 %2").arg(node.id, style);
        }
    }
    for (const Node &node : std::as_const(context)) {
        lines << QStringLiteral("    style %1 %2").arg(node.id, styleFor(Status::Context));
    }
    result.code = lines.join(QLatin1Char('\n'));
    return result;
}

#include "CanvasBoard.h"

#include <QHash>
#include <QJsonArray>
#include <QSet>

#include <algorithm>

namespace {

QString outputKey(CanvasStepOutput output) {
    return output == CanvasStepOutput::Tasks ? QStringLiteral("tasks") : QStringLiteral("document");
}

CanvasStepOutput outputFromKey(const QString &key) {
    return key == QLatin1String("tasks") ? CanvasStepOutput::Tasks : CanvasStepOutput::Document;
}

QJsonObject runToJson(const AgentResult &result, const QDateTime &finishedAt) {
    QJsonObject obj;
    obj[QStringLiteral("success")] = result.success;
    obj[QStringLiteral("outcome")] = result.outcome;
    obj[QStringLiteral("message")] = result.message;
    obj[QStringLiteral("sessionId")] = result.sessionId;
    obj[QStringLiteral("durationMs")] = result.durationMs;
    obj[QStringLiteral("totalTokens")] = result.totalTokens;
    obj[QStringLiteral("costUsd")] = result.costUsd;
    if (!result.deniedTools.isEmpty()) {
        obj[QStringLiteral("deniedTools")] = QJsonArray::fromStringList(result.deniedTools);
    }
    obj[QStringLiteral("finishedAt")] = finishedAt.toString(Qt::ISODate);
    return obj;
}

void runFromJson(const QJsonObject &obj, CanvasNode *node) {
    AgentResult &result = node->result;
    result.success = obj.value(QStringLiteral("success")).toBool();
    result.outcome = obj.value(QStringLiteral("outcome")).toString();
    result.message = obj.value(QStringLiteral("message")).toString();
    result.sessionId = obj.value(QStringLiteral("sessionId")).toString();
    result.durationMs = obj.value(QStringLiteral("durationMs")).toInteger();
    result.totalTokens = obj.value(QStringLiteral("totalTokens")).toInteger();
    result.costUsd = obj.value(QStringLiteral("costUsd")).toDouble();
    const QJsonArray denied = obj.value(QStringLiteral("deniedTools")).toArray();
    for (const QJsonValue &tool : denied) {
        result.deniedTools.append(tool.toString());
    }
    node->finishedAt = QDateTime::fromString(obj.value(QStringLiteral("finishedAt")).toString(), Qt::ISODate);
}

QJsonObject nodeToJson(const CanvasNode &node) {
    QJsonObject obj;
    obj[QStringLiteral("id")] = node.id;
    obj[QStringLiteral("kind")] = canvasNodeKindKey(node.kind);
    obj[QStringLiteral("x")] = node.pos.x();
    obj[QStringLiteral("y")] = node.pos.y();
    obj[QStringLiteral("width")] = node.size.width();
    obj[QStringLiteral("height")] = node.size.height();
    if (!node.text.isEmpty()) {
        obj[QStringLiteral("text")] = node.text;
    }
    switch (node.kind) {
    case CanvasNodeKind::Note:
        if (!node.color.isEmpty()) {
            obj[QStringLiteral("color")] = node.color;
        }
        break;
    case CanvasNodeKind::Artifact:
    case CanvasNodeKind::Task:
        obj[QStringLiteral("source")] = node.source.toJson();
        obj[QStringLiteral("title")] = node.title;
        if (!node.detail.isEmpty()) {
            obj[QStringLiteral("detail")] = node.detail;
        }
        break;
    case CanvasNodeKind::Step:
        if (!node.model.isEmpty()) {
            obj[QStringLiteral("model")] = node.model;
        }
        if (!node.effort.isEmpty()) {
            obj[QStringLiteral("effort")] = node.effort;
        }
        obj[QStringLiteral("output")] = outputKey(node.output);
        if (node.hasResult()) {
            obj[QStringLiteral("run")] = runToJson(node.result, node.finishedAt);
        }
        break;
    }
    return obj;
}

}

QString canvasNodeKindKey(CanvasNodeKind kind) {
    switch (kind) {
    case CanvasNodeKind::Artifact: return QStringLiteral("artifact");
    case CanvasNodeKind::Task: return QStringLiteral("task");
    case CanvasNodeKind::Step: return QStringLiteral("step");
    case CanvasNodeKind::Note: break;
    }
    return QStringLiteral("note");
}

std::optional<CanvasNodeKind> canvasNodeKindFromKey(const QString &key) {
    if (key == QLatin1String("note")) return CanvasNodeKind::Note;
    if (key == QLatin1String("artifact")) return CanvasNodeKind::Artifact;
    if (key == QLatin1String("task")) return CanvasNodeKind::Task;
    if (key == QLatin1String("step")) return CanvasNodeKind::Step;
    return std::nullopt;
}

QJsonObject CanvasSource::toJson() const {
    QJsonObject obj;
    obj[QStringLiteral("projectId")] = projectId;
    obj[QStringLiteral("taskId")] = taskId;
    if (!stage.isEmpty()) {
        obj[QStringLiteral("stage")] = stage;
    }
    if (!attachment.isEmpty()) {
        obj[QStringLiteral("attachment")] = attachment;
    }
    return obj;
}

CanvasSource CanvasSource::fromJson(const QJsonObject &object) {
    CanvasSource source;
    source.projectId = object.value(QStringLiteral("projectId")).toString();
    source.taskId = object.value(QStringLiteral("taskId")).toString();
    source.stage = object.value(QStringLiteral("stage")).toString();
    source.attachment = object.value(QStringLiteral("attachment")).toString();
    return source;
}

QSizeF CanvasNode::defaultSize(CanvasNodeKind kind) {
    switch (kind) {
    case CanvasNodeKind::Artifact: return QSizeF(280, 180);
    case CanvasNodeKind::Task: return QSizeF(260, 132);
    case CanvasNodeKind::Step: return QSizeF(300, 200);
    case CanvasNodeKind::Note: break;
    }
    return QSizeF(220, 150);
}

QSizeF CanvasNode::minimumSize(CanvasNodeKind kind) {
    return kind == CanvasNodeKind::Note ? QSizeF(120, 72) : QSizeF(180, 96);
}

qsizetype CanvasBoard::indexOf(const QString &nodeId) const {
    for (qsizetype i = 0; i < nodes.size(); ++i) {
        if (nodes.at(i).id == nodeId) {
            return i;
        }
    }
    return -1;
}

const CanvasNode *CanvasBoard::node(const QString &nodeId) const {
    const qsizetype index = indexOf(nodeId);
    return index >= 0 ? &nodes.at(index) : nullptr;
}

CanvasNode *CanvasBoard::node(const QString &nodeId) {
    const qsizetype index = indexOf(nodeId);
    return index >= 0 ? &nodes[index] : nullptr;
}

const CanvasEdge *CanvasBoard::edge(const QString &edgeId) const {
    for (const CanvasEdge &edge : edges) {
        if (edge.id == edgeId) {
            return &edge;
        }
    }
    return nullptr;
}

bool CanvasBoard::hasEdge(const QString &from, const QString &to) const {
    return std::any_of(edges.cbegin(), edges.cend(), [&](const CanvasEdge &edge) {
        return edge.from == from && edge.to == to;
    });
}

QStringList CanvasBoard::inputsOf(const QString &nodeId) const {
    QStringList inputs;
    for (const CanvasEdge &edge : edges) {
        if (edge.to == nodeId && !inputs.contains(edge.from)) {
            inputs.append(edge.from);
        }
    }
    return inputs;
}

QStringList CanvasBoard::outputsOf(const QString &nodeId) const {
    QStringList outputs;
    for (const CanvasEdge &edge : edges) {
        if (edge.from == nodeId && !outputs.contains(edge.to)) {
            outputs.append(edge.to);
        }
    }
    return outputs;
}

bool CanvasBoard::reaches(const QString &from, const QString &to) const {
    QSet<QString> seen;
    QStringList stack = {from};
    while (!stack.isEmpty()) {
        const QString current = stack.takeLast();
        if (current == to) {
            return true;
        }
        if (seen.contains(current)) {
            continue;
        }
        seen.insert(current);
        for (const CanvasEdge &edge : edges) {
            if (edge.from == current) {
                stack.append(edge.to);
            }
        }
    }
    return false;
}

QStringList CanvasBoard::stepIds() const {
    QStringList ids;
    for (const CanvasNode &node : nodes) {
        if (node.kind == CanvasNodeKind::Step) {
            ids.append(node.id);
        }
    }
    return ids;
}

QStringList CanvasBoard::upstreamSteps(const QString &stepId) const {
    QStringList steps;
    const QStringList inputs = inputsOf(stepId);
    for (const QString &input : inputs) {
        const CanvasNode *candidate = node(input);
        if (candidate && candidate->kind == CanvasNodeKind::Step) {
            steps.append(input);
        }
    }
    return steps;
}

QStringList CanvasBoard::stepOrder(const QStringList &ids) const {
    QHash<QString, int> waiting;            // jumlah langkah hulu (di dalam ids) yang belum terurut
    QHash<QString, QStringList> dependents;
    for (const QString &id : ids) {
        const CanvasNode *candidate = node(id);
        if (candidate && candidate->kind == CanvasNodeKind::Step) {
            waiting.insert(id, 0);
        }
    }
    for (auto it = waiting.begin(); it != waiting.end(); ++it) {
        const QStringList upstream = upstreamSteps(it.key());
        for (const QString &up : upstream) {
            if (waiting.contains(up)) {
                ++it.value();
                dependents[up].append(it.key());
            }
        }
    }

    auto readingOrder = [this](const QString &a, const QString &b) {
        const CanvasNode *first = node(a);
        const CanvasNode *second = node(b);
        if (first->pos.y() != second->pos.y()) {
            return first->pos.y() < second->pos.y();
        }
        if (first->pos.x() != second->pos.x()) {
            return first->pos.x() < second->pos.x();
        }
        return indexOf(a) < indexOf(b);
    };

    QStringList ready;
    for (auto it = waiting.cbegin(); it != waiting.cend(); ++it) {
        if (it.value() == 0) {
            ready.append(it.key());
        }
    }
    QStringList order;
    while (!ready.isEmpty()) {
        std::sort(ready.begin(), ready.end(), readingOrder);
        const QString next = ready.takeFirst();
        order.append(next);
        const QStringList downstream = dependents.value(next);
        for (const QString &dependent : downstream) {
            if (--waiting[dependent] == 0) {
                ready.append(dependent);
            }
        }
    }
    return order;
}

QStringList CanvasBoard::withUpstream(const QString &stepId) const {
    QStringList collected;
    QStringList stack = {stepId};
    while (!stack.isEmpty()) {
        const QString current = stack.takeLast();
        if (collected.contains(current)) {
            continue;
        }
        collected.append(current);
        stack += upstreamSteps(current);
    }
    return stepOrder(collected);
}

QRectF CanvasBoard::bounds() const {
    QRectF united;
    for (const CanvasNode &node : nodes) {
        united = united.isNull() ? node.rect() : united.united(node.rect());
    }
    return united;
}

QPointF CanvasBoard::openSpot(const QRectF &want, qreal gap) const {
    auto fits = [this, gap](const QRectF &rect) {
        return std::none_of(nodes.cbegin(), nodes.cend(), [&rect, gap](const CanvasNode &node) {
            return node.rect().adjusted(-gap, -gap, gap, gap).intersects(rect);
        });
    };
    if (fits(want)) {
        return want.topLeft();
    }
    // Kandidat di kisi berlangkah setengah ukuran kartu, dicoba dari yang terdekat. Jarak sama:
    // bawah dulu, lalu kanan (arah baca kanvas).
    constexpr int kReach = 16;
    const qreal stepX = qMax<qreal>(40.0, want.width() / 2);
    const qreal stepY = qMax<qreal>(40.0, want.height() / 2);
    QList<QPointF> offsets;
    offsets.reserve((2 * kReach + 1) * (2 * kReach + 1));
    for (int i = -kReach; i <= kReach; ++i) {
        for (int j = -kReach; j <= kReach; ++j) {
            offsets.append(QPointF(i * stepX, j * stepY));
        }
    }
    std::sort(offsets.begin(), offsets.end(), [](const QPointF &a, const QPointF &b) {
        const qreal first = a.x() * a.x() + a.y() * a.y();
        const qreal second = b.x() * b.x() + b.y() * b.y();
        if (first != second) {
            return first < second;
        }
        return a.y() != b.y() ? a.y() > b.y() : a.x() > b.x();
    });
    for (const QPointF &offset : std::as_const(offsets)) {
        if (fits(want.translated(offset))) {
            return want.topLeft() + offset;
        }
    }
    return want.topLeft();
}

QJsonObject CanvasBoard::toJson() const {
    QJsonArray nodeArray;
    for (const CanvasNode &node : nodes) {
        nodeArray.append(nodeToJson(node));
    }
    QJsonArray edgeArray;
    for (const CanvasEdge &edge : edges) {
        edgeArray.append(QJsonObject{{QStringLiteral("id"), edge.id},
                                     {QStringLiteral("from"), edge.from},
                                     {QStringLiteral("to"), edge.to}});
    }

    QJsonObject root;
    root[QStringLiteral("schemaVersion")] = kSchemaVersion;
    root[QStringLiteral("view")] = QJsonObject{{QStringLiteral("x"), viewCenter.x()},
                                               {QStringLiteral("y"), viewCenter.y()},
                                               {QStringLiteral("zoom"), zoom}};
    root[QStringLiteral("nodes")] = nodeArray;
    root[QStringLiteral("edges")] = edgeArray;
    return root;
}

std::optional<CanvasBoard> CanvasBoard::fromJson(const QJsonObject &root, QString *error, QStringList *warnings) {
    const int version = root.value(QStringLiteral("schemaVersion")).toInt(-1);
    if (version != kSchemaVersion) {
        if (error) {
            *error = QStringLiteral("schemaVersion kanvas %1 tidak dikenal (diharapkan %2)").arg(version).arg(kSchemaVersion);
        }
        return std::nullopt;
    }
    auto warn = [warnings](const QString &text) {
        if (warnings) {
            warnings->append(text);
        }
    };

    CanvasBoard board;
    const QJsonObject view = root.value(QStringLiteral("view")).toObject();
    board.viewCenter = QPointF(view.value(QStringLiteral("x")).toDouble(), view.value(QStringLiteral("y")).toDouble());
    const double zoom = view.value(QStringLiteral("zoom")).toDouble(1.0);
    board.zoom = zoom > 0.0 ? zoom : 1.0;

    const QJsonArray nodeArray = root.value(QStringLiteral("nodes")).toArray();
    for (const QJsonValue &value : nodeArray) {
        const QJsonObject obj = value.toObject();
        CanvasNode node;
        node.id = obj.value(QStringLiteral("id")).toString();
        const std::optional<CanvasNodeKind> kind = canvasNodeKindFromKey(obj.value(QStringLiteral("kind")).toString());
        if (node.id.isEmpty() || !kind) {
            warn(QStringLiteral("kartu tanpa id atau berjenis \"%1\" dilewati").arg(obj.value(QStringLiteral("kind")).toString()));
            continue;
        }
        if (board.node(node.id)) {
            warn(QStringLiteral("kartu %1 ganda dilewati").arg(node.id));
            continue;
        }
        node.kind = *kind;
        node.pos = QPointF(obj.value(QStringLiteral("x")).toDouble(), obj.value(QStringLiteral("y")).toDouble());
        const QSizeF minimum = CanvasNode::minimumSize(node.kind);
        const QSizeF fallback = CanvasNode::defaultSize(node.kind);
        node.size = QSizeF(qMax(minimum.width(), obj.value(QStringLiteral("width")).toDouble(fallback.width())),
                           qMax(minimum.height(), obj.value(QStringLiteral("height")).toDouble(fallback.height())));
        node.text = obj.value(QStringLiteral("text")).toString();
        node.color = obj.value(QStringLiteral("color")).toString();
        node.source = CanvasSource::fromJson(obj.value(QStringLiteral("source")).toObject());
        node.title = obj.value(QStringLiteral("title")).toString();
        node.detail = obj.value(QStringLiteral("detail")).toString();
        node.model = obj.value(QStringLiteral("model")).toString();
        node.effort = obj.value(QStringLiteral("effort")).toString();
        node.output = outputFromKey(obj.value(QStringLiteral("output")).toString());
        if (obj.contains(QStringLiteral("run"))) {
            runFromJson(obj.value(QStringLiteral("run")).toObject(), &node);
        }
        board.nodes.append(node);
    }

    const QJsonArray edgeArray = root.value(QStringLiteral("edges")).toArray();
    for (const QJsonValue &value : edgeArray) {
        const QJsonObject obj = value.toObject();
        CanvasEdge edge;
        edge.id = obj.value(QStringLiteral("id")).toString();
        edge.from = obj.value(QStringLiteral("from")).toString();
        edge.to = obj.value(QStringLiteral("to")).toString();
        if (edge.id.isEmpty() || !board.node(edge.from) || !board.node(edge.to) || edge.from == edge.to
            || board.edge(edge.id) || board.hasEdge(edge.from, edge.to)) {
            warn(QStringLiteral("garis %1 tidak valid dilewati").arg(edge.id));
            continue;
        }
        if (board.reaches(edge.to, edge.from)) {
            warn(QStringLiteral("garis %1 membuat putaran, dilewati").arg(edge.id));
            continue;
        }
        board.edges.append(edge);
    }
    return board;
}

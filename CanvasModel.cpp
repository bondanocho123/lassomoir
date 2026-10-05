#include "CanvasModel.h"

#include <QSet>
#include <QUuid>

namespace {

constexpr int kUndoLimit = 200;

QString newId() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

bool fail(QString *reason, const QString &why) {
    if (reason) {
        *reason = why;
    }
    return false;
}

}

CanvasModel::CanvasModel(const QString &projectId, QObject *parent)
    : QObject(parent), m_projectId(projectId) {
}

void CanvasModel::load(const CanvasBoard &board) {
    m_board = board;
    m_undo.clear();
    m_redo.clear();
    emit boardReset();
    emit undoStateChanged();
}

QString CanvasModel::addNote(const QPointF &pos, const QString &text, const QString &color) {
    CanvasNode node;
    node.id = newId();
    node.kind = CanvasNodeKind::Note;
    node.pos = pos;
    node.size = CanvasNode::defaultSize(node.kind);
    node.text = text;
    node.color = color;
    return insert(node);
}

QString CanvasModel::addStep(const QPointF &pos, const QString &instruction, CanvasStepOutput output) {
    CanvasNode node;
    node.id = newId();
    node.kind = CanvasNodeKind::Step;
    node.pos = pos;
    node.size = CanvasNode::defaultSize(node.kind);
    node.text = instruction;
    node.output = output;
    return insert(node);
}

QString CanvasModel::addReference(const CanvasSource &source, const QPointF &pos, const QString &title,
                                  const QString &detail, const QString &text, bool available) {
    if (!source.isValid()) {
        return QString();
    }
    CanvasNode node;
    node.id = newId();
    node.kind = source.isArtifact() ? CanvasNodeKind::Artifact : CanvasNodeKind::Task;
    node.pos = pos;
    node.size = CanvasNode::defaultSize(node.kind);
    node.source = source;
    node.title = title;
    node.detail = detail;
    node.text = text;
    node.available = available;
    return insert(node);
}

QString CanvasModel::insert(const CanvasNode &node) {
    record();
    m_board.nodes.append(node);
    emit nodeAdded(node);
    emit changed();
    return node.id;
}

bool CanvasModel::moveNodes(const QHash<QString, QPointF> &positions) {
    QStringList moved;
    for (auto it = positions.cbegin(); it != positions.cend(); ++it) {
        const CanvasNode *node = m_board.node(it.key());
        if (node && node->pos != it.value()) {
            moved.append(it.key());
        }
    }
    if (moved.isEmpty()) {
        return false;
    }
    record();
    for (const QString &id : std::as_const(moved)) {
        CanvasNode *node = m_board.node(id);
        node->pos = positions.value(id);
        emit nodeChanged(*node);
    }
    emit changed();
    return true;
}

bool CanvasModel::resizeNode(const QString &id, const QSizeF &size) {
    const CanvasNode *node = m_board.node(id);
    if (!node || !size.isValid()) {
        return false;
    }
    const QSizeF bounded = size.expandedTo(CanvasNode::minimumSize(node->kind));
    if (bounded == node->size) {
        return false;
    }
    record();
    CanvasNode *target = m_board.node(id);
    target->size = bounded;
    emit nodeChanged(*target);
    emit changed();
    return true;
}

bool CanvasModel::setText(const QString &id, const QString &text, const QSizeF &grow) {
    const CanvasNode *node = m_board.node(id);
    if (!node || (node->kind != CanvasNodeKind::Note && node->kind != CanvasNodeKind::Step)) {
        return false;
    }
    const QSizeF size = grow.isValid() ? node->size.expandedTo(grow) : node->size;
    if (node->text == text && size == node->size) {
        return false;
    }
    record();
    CanvasNode *target = m_board.node(id);
    target->text = text;
    target->size = size;
    emit nodeChanged(*target);
    emit changed();
    return true;
}

bool CanvasModel::setColor(const QString &id, const QString &color) {
    const CanvasNode *node = m_board.node(id);
    if (!node || node->kind != CanvasNodeKind::Note || node->color == color) {
        return false;
    }
    record();
    CanvasNode *target = m_board.node(id);
    target->color = color;
    emit nodeChanged(*target);
    emit changed();
    return true;
}

bool CanvasModel::setStepOptions(const QString &id, const QString &model, const QString &effort,
                                 CanvasStepOutput output) {
    const CanvasNode *node = m_board.node(id);
    if (!node || node->kind != CanvasNodeKind::Step) {
        return false;
    }
    if (node->model == model.trimmed() && node->effort == effort.trimmed() && node->output == output) {
        return false;
    }
    record();
    CanvasNode *target = m_board.node(id);
    target->model = model.trimmed();
    target->effort = effort.trimmed();
    target->output = output;
    emit nodeChanged(*target);
    emit changed();
    return true;
}

bool CanvasModel::removeNodes(const QStringList &ids) {
    QSet<QString> doomed;
    for (const QString &id : ids) {
        if (m_board.node(id)) {
            doomed.insert(id);
        }
    }
    if (doomed.isEmpty()) {
        return false;
    }
    record();
    for (qsizetype i = m_board.edges.size() - 1; i >= 0; --i) {
        const CanvasEdge &edge = m_board.edges.at(i);
        if (doomed.contains(edge.from) || doomed.contains(edge.to)) {
            const QString edgeId = edge.id;
            m_board.edges.removeAt(i);
            emit edgeRemoved(edgeId);
        }
    }
    for (qsizetype i = m_board.nodes.size() - 1; i >= 0; --i) {
        if (doomed.contains(m_board.nodes.at(i).id)) {
            const QString nodeId = m_board.nodes.at(i).id;
            m_board.nodes.removeAt(i);
            emit nodeRemoved(nodeId);
        }
    }
    emit changed();
    return true;
}

QString CanvasModel::connectNodes(const QString &from, const QString &to, QString *reason) {
    if (!m_board.node(from) || !m_board.node(to)) {
        fail(reason, QStringLiteral("kartu tidak ditemukan"));
        return QString();
    }
    if (from == to) {
        fail(reason, QStringLiteral("kartu tidak bisa disambungkan ke dirinya sendiri"));
        return QString();
    }
    if (m_board.hasEdge(from, to)) {
        fail(reason, QStringLiteral("kedua kartu sudah tersambung"));
        return QString();
    }
    if (m_board.reaches(to, from)) {
        fail(reason, QStringLiteral("garis ini membuat putaran: kartu tujuan sudah mengalir ke kartu asal"));
        return QString();
    }
    record();
    CanvasEdge edge;
    edge.id = newId();
    edge.from = from;
    edge.to = to;
    m_board.edges.append(edge);
    emit edgeAdded(edge);
    emit changed();
    return edge.id;
}

bool CanvasModel::removeEdges(const QStringList &ids) {
    QStringList existing;
    for (const QString &id : ids) {
        if (m_board.edge(id) && !existing.contains(id)) {
            existing.append(id);
        }
    }
    if (existing.isEmpty()) {
        return false;
    }
    record();
    for (const QString &id : std::as_const(existing)) {
        for (qsizetype i = 0; i < m_board.edges.size(); ++i) {
            if (m_board.edges.at(i).id == id) {
                m_board.edges.removeAt(i);
                break;
            }
        }
        emit edgeRemoved(id);
    }
    emit changed();
    return true;
}

QStringList CanvasModel::duplicateNodes(const QStringList &ids, const QPointF &offset) {
    QHash<QString, QString> copies;   // id lama -> id salinan
    QList<CanvasNode> added;
    for (const QString &id : ids) {
        const CanvasNode *node = m_board.node(id);
        if (!node || copies.contains(id)) {
            continue;
        }
        CanvasNode copy = *node;
        copy.id = newId();
        copy.pos += offset;
        copy.result = AgentResult();
        copy.finishedAt = QDateTime();
        copies.insert(id, copy.id);
        added.append(copy);
    }
    if (added.isEmpty()) {
        return QStringList();
    }
    record();
    QStringList result;
    for (const CanvasNode &copy : std::as_const(added)) {
        m_board.nodes.append(copy);
        result.append(copy.id);
        emit nodeAdded(copy);
    }
    const QList<CanvasEdge> originals = m_board.edges;
    for (const CanvasEdge &edge : originals) {
        if (copies.contains(edge.from) && copies.contains(edge.to)) {
            CanvasEdge copy;
            copy.id = newId();
            copy.from = copies.value(edge.from);
            copy.to = copies.value(edge.to);
            m_board.edges.append(copy);
            emit edgeAdded(copy);
        }
    }
    emit changed();
    return result;
}

void CanvasModel::beginMacro() {
    if (m_macroDepth++ == 0) {
        m_macroRecorded = false;
    }
}

void CanvasModel::endMacro() {
    if (m_macroDepth > 0) {
        --m_macroDepth;
    }
}

bool CanvasModel::setStepResult(const QString &id, const AgentResult &result, const QDateTime &finishedAt) {
    CanvasNode *node = m_board.node(id);
    if (!node || node->kind != CanvasNodeKind::Step) {
        return false;
    }
    node->result = result;
    node->finishedAt = finishedAt;
    emit nodeChanged(*node);
    emit changed();
    return true;
}

bool CanvasModel::refreshReference(const QString &id, const QString &title, const QString &detail,
                                   const QString &text, bool available) {
    CanvasNode *node = m_board.node(id);
    if (!node || !node->isReference()) {
        return false;
    }
    const bool stored = node->title != title || node->detail != detail || node->text != text;
    if (!stored && node->available == available) {
        return false;
    }
    node->title = title;
    node->detail = detail;
    node->text = text;
    node->available = available;
    emit nodeChanged(*node);
    if (stored) {
        emit changed();
    }
    return true;
}

void CanvasModel::setView(const QPointF &center, qreal zoom) {
    m_board.viewCenter = center;
    m_board.zoom = zoom;
}

bool CanvasModel::undo() {
    if (m_undo.isEmpty()) {
        return false;
    }
    m_redo.append(m_board);
    restore(m_undo.takeLast());
    return true;
}

bool CanvasModel::redo() {
    if (m_redo.isEmpty()) {
        return false;
    }
    m_undo.append(m_board);
    restore(m_redo.takeLast());
    return true;
}

void CanvasModel::record() {
    if (m_macroDepth > 0) {
        if (m_macroRecorded) {
            return;
        }
        m_macroRecorded = true;
    }
    m_undo.append(m_board);
    if (m_undo.size() > kUndoLimit) {
        m_undo.removeFirst();
    }
    m_redo.clear();
    emit undoStateChanged();
}

void CanvasModel::restore(const CanvasBoard &snapshot) {
    CanvasBoard next = snapshot;
    for (CanvasNode &node : next.nodes) {
        const CanvasNode *current = m_board.node(node.id);
        if (!current) {
            continue;
        }
        node.result = current->result;
        node.finishedAt = current->finishedAt;
        if (node.isReference()) {
            node.title = current->title;
            node.detail = current->detail;
            node.text = current->text;
            node.available = current->available;
        }
    }
    next.viewCenter = m_board.viewCenter;
    next.zoom = m_board.zoom;
    m_board = next;
    emit boardReset();
    emit changed();
    emit undoStateChanged();
}

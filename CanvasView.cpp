#include "CanvasView.h"
#include "CanvasItems.h"
#include "CanvasLibrary.h"
#include "CanvasModel.h"
#include "Theme.h"

#include <QAction>
#include <QContextMenuEvent>
#include <QCursor>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QGraphicsDropShadowEffect>
#include <QGraphicsPathItem>
#include <QGraphicsScene>
#include <QKeyEvent>
#include <QLabel>
#include <QLineF>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QPainter>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QWheelEvent>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <iterator>

namespace {

constexpr qreal kExtent = 500000.0;   // tepi scene jauh di luar jangkauan: terasa tanpa batas
constexpr qreal kMinZoom = 0.1;
constexpr qreal kMaxZoom = 3.0;
constexpr qreal kWheelStep = 1.15;
constexpr qreal kGrid = 24.0;
constexpr qreal kScrollPixelsPerNotch = 60.0;
constexpr int kPulsePeriodMs = 1200;
constexpr qreal kZoomSteps[] = {0.1, 0.25, 0.33, 0.5, 0.67, 0.75, 1.0, 1.25, 1.5, 2.0, 3.0};
// Jarak widget mengambang dari tepi kanvas
constexpr int kOverlayInset = 12;

}

CanvasView::CanvasView(CanvasModel &model, QWidget *parent)
    : QGraphicsView(parent), m_model(model), m_scene(new QGraphicsScene(this)) {
    setObjectName("canvasView");
    m_scene->setSceneRect(-kExtent, -kExtent, 2 * kExtent, 2 * kExtent);
    setScene(m_scene);
    setFrameShape(QFrame::NoFrame);
    setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // Titik jangkar zoom dijaga applyZoom(); saat jendela diubah ukurannya tengah tampilan tetap
    setTransformationAnchor(QGraphicsView::NoAnchor);
    setResizeAnchor(QGraphicsView::AnchorViewCenter);
    setDragMode(QGraphicsView::RubberBandDrag);
    setRubberBandSelectionMode(Qt::IntersectsItemShape);
    setAcceptDrops(true);
    setFocusPolicy(Qt::StrongFocus);

    m_emptyHint = new QLabel(viewport());
    m_emptyHint->setObjectName("canvasEmptyHint");
    m_emptyHint->setAlignment(Qt::AlignCenter);
    m_emptyHint->setWordWrap(true);
    m_emptyHint->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_emptyHint->setText(QStringLiteral(
        "<p style='font-size:16px'><b>Kanvas masih kosong</b></p>"
        "<p>Klik dua kali untuk menulis catatan · tekan <b>L</b> untuk langkah AI<br>"
        "Seret dokumen hasil stage dari <b>Pustaka</b> di kiri untuk merujuk task lain<br>"
        "Tarik dari titik ● di tepi kartu untuk menyambungkan bahan ke langkah AI</p>"));

    connect(m_scene, &QGraphicsScene::selectionChanged, this, &CanvasView::selectionChanged);
    connect(&m_model, &CanvasModel::nodeAdded, this, &CanvasView::addNodeItem);
    connect(&m_model, &CanvasModel::nodeChanged, this, [this](const CanvasNode &node) {
        if (CanvasNodeItem *item = m_nodes.value(node.id)) {
            item->setNode(node);
        }
    });
    connect(&m_model, &CanvasModel::nodeRemoved, this, &CanvasView::removeNodeItem);
    connect(&m_model, &CanvasModel::edgeAdded, this, &CanvasView::addEdgeItem);
    connect(&m_model, &CanvasModel::edgeRemoved, this, &CanvasView::removeEdgeItem);
    connect(&m_model, &CanvasModel::boardReset, this, &CanvasView::rebuild);
    // Warna kartu dan grid dilukis sendiri: gambar ulang saat tema berganti
    connect(Theme::Notifier::instance(), &Theme::Notifier::changed, this, [this]() {
        m_scene->update();
        viewport()->update();
    });

    m_pulseTimer.setInterval(40);
    connect(&m_pulseTimer, &QTimer::timeout, this, &CanvasView::updatePulse);

    rebuild();
    setTransform(QTransform::fromScale(m_zoom, m_zoom));
    centerOn(0, 0);
}

CanvasView::~CanvasView() {
    // Scene anak view ini; tanpa dibongkar di sini ia baru dihapus QWidget sesudah anggota view hilang,
    // dan selectionChanged dari kartu terpilih yang ikut terhapus sampai ke view/halaman yang setengah hancur
    disconnect(m_scene, nullptr, this, nullptr);
    setScene(nullptr);
    delete m_scene;
}

void CanvasView::finishEditing() {
    if (isEditingText()) {
        // Editor catatan menyimpan isinya saat fokusnya dilepas
        m_scene->setFocusItem(nullptr);
    }
}

void CanvasView::setThumbnailProvider(ThumbnailProvider provider) {
    m_thumbnails = std::move(provider);
    for (CanvasNodeItem *item : std::as_const(m_nodes)) {
        if (item->wantsThumbnail() && m_thumbnails) {
            item->setThumbnail(m_thumbnails(item->node().source));
        }
    }
}

void CanvasView::rebuild() {
    const QStringList selected = selectedNodeIds();
    cancelConnection();
    {
        const QSignalBlocker blocker(m_scene);
        // Fokus dilepas dulu: editor catatan yang sedang dibuka menyimpan isinya selagi kartunya masih utuh
        m_scene->setFocusItem(nullptr);
        for (CanvasEdgeItem *edge : std::as_const(m_edges)) {
            edge->from()->removeEdge(edge);
            edge->to()->removeEdge(edge);
            delete edge;
        }
        m_edges.clear();
        qDeleteAll(m_nodes);
        m_nodes.clear();

        const CanvasBoard &board = m_model.board();
        for (const CanvasNode &node : board.nodes) {
            addNodeItem(node);
        }
        for (const CanvasEdge &edge : board.edges) {
            addEdgeItem(edge);
        }
        for (const QString &id : selected) {
            if (CanvasNodeItem *item = m_nodes.value(id)) {
                item->setSelected(true);
            }
        }
    }
    updateEmptyHint();
    emit selectionChanged();
}

void CanvasView::addNodeItem(const CanvasNode &node) {
    if (m_nodes.contains(node.id)) {
        return;
    }
    auto *item = new CanvasNodeItem(node);
    item->setZValue(++m_topZ);
    m_scene->addItem(item);
    m_nodes.insert(node.id, item);
    item->setRunState(m_runStates.value(node.id, RunState::Idle));
    if (item->wantsThumbnail() && m_thumbnails) {
        item->setThumbnail(m_thumbnails(node.source));
    }
    connect(item, &CanvasNodeItem::resized, this, [this](const QString &id, const QSizeF &size) {
        if (!m_model.resizeNode(id, size)) {
            const CanvasNode *current = m_model.node(id);
            CanvasNodeItem *target = m_nodes.value(id);
            if (current && target) {
                target->setNode(*current);
            }
        }
    });
    connect(item, &CanvasNodeItem::edited, this, [this](const QString &id, const QString &text, const QSizeF &needed) {
        // Tanpa perubahan, ukuran yang sempat memanjang selama diketik dikembalikan ke data model
        if (!m_model.setText(id, text, needed)) {
            if (const CanvasNode *current = m_model.node(id)) {
                if (CanvasNodeItem *target = m_nodes.value(id)) {
                    target->setNode(*current);
                }
            }
        }
    });
    updateEmptyHint();
}

void CanvasView::addEdgeItem(const CanvasEdge &edge) {
    CanvasNodeItem *from = m_nodes.value(edge.from);
    CanvasNodeItem *to = m_nodes.value(edge.to);
    if (!from || !to || m_edges.contains(edge.id)) {
        return;
    }
    auto *item = new CanvasEdgeItem(edge, from, to);
    m_scene->addItem(item);
    m_edges.insert(edge.id, item);
}

void CanvasView::removeEdgeItem(const QString &id) {
    CanvasEdgeItem *edge = m_edges.take(id);
    if (!edge) {
        return;
    }
    edge->from()->removeEdge(edge);
    edge->to()->removeEdge(edge);
    delete edge;
}

void CanvasView::removeNodeItem(const QString &id) {
    CanvasNodeItem *item = m_nodes.value(id);
    if (!item) {
        return;
    }
    if (item == m_connectFrom) {
        cancelConnection();
    }
    if (item->isEditing()) {
        m_scene->setFocusItem(nullptr);
    }
    const QList<CanvasEdgeItem *> attached = item->edges();
    for (CanvasEdgeItem *edge : attached) {
        removeEdgeItem(edge->id());
    }
    m_nodes.remove(id);
    m_runStates.remove(id);
    delete item;
    updatePulse();
    updateEmptyHint();
}

void CanvasView::updateEmptyHint() {
    m_emptyHint->setVisible(m_nodes.isEmpty());
}

void CanvasView::addOverlay(QWidget *overlay, Qt::Corner corner) {
    // Anak view, bukan anak viewport: isi viewport ikut bergeser saat kanvas digeser. Klik di sela
    // tombolnya juga tidak sampai ke kanvas, karena kanvas hanya menerima klik lewat viewport.
    overlay->setParent(this);
    // Bayangan tipis: tombolnya terlihat melayang di atas kartu yang lewat di bawahnya
    auto *shadow = new QGraphicsDropShadowEffect(overlay);
    shadow->setBlurRadius(14);
    shadow->setOffset(0, 2);
    shadow->setColor(QColor(0x24, 0x20, 0x1b, 60));
    overlay->setGraphicsEffect(shadow);
    m_overlays.insert(overlay, corner);
    overlay->show();
    overlay->raise();
    layoutOverlays();
}

void CanvasView::layoutOverlays() {
    const QRect area = viewport()->geometry().adjusted(kOverlayInset, kOverlayInset, -kOverlayInset, -kOverlayInset);
    QSize needed(0, kOverlayInset);
    for (auto it = m_overlays.cbegin(); it != m_overlays.cend(); ++it) {
        const QSize size = it.key()->sizeHint();
        const bool right = it.value() == Qt::TopRightCorner || it.value() == Qt::BottomRightCorner;
        const bool bottom = it.value() == Qt::BottomLeftCorner || it.value() == Qt::BottomRightCorner;
        it.key()->setGeometry(right ? area.right() + 1 - size.width() : area.left(),
                              bottom ? area.bottom() + 1 - size.height() : area.top(), size.width(), size.height());
        needed = QSize(qMax(needed.width(), size.width() + 2 * kOverlayInset),
                       needed.height() + size.height() + kOverlayInset);
    }
    // Kanvas tidak menyempit sampai widget mengambangnya saling menimpa atau terpotong
    if (!m_overlays.isEmpty() && minimumSize() != needed) {
        setMinimumSize(needed);
    }
}

QStringList CanvasView::selectedNodeIds() const {
    QStringList ids;
    const QList<QGraphicsItem *> selected = m_scene->selectedItems();
    for (QGraphicsItem *item : selected) {
        if (auto *node = qgraphicsitem_cast<CanvasNodeItem *>(item)) {
            ids.append(node->id());
        }
    }
    // Urutan tetap (urutan kartu di kanvas), bukan urutan internal scene
    const CanvasBoard &board = m_model.board();
    std::sort(ids.begin(), ids.end(), [&board](const QString &a, const QString &b) {
        return board.indexOf(a) < board.indexOf(b);
    });
    return ids;
}

QStringList CanvasView::selectedStepIds() const {
    QStringList steps;
    const QStringList ids = selectedNodeIds();
    for (const QString &id : ids) {
        const CanvasNode *node = m_model.node(id);
        if (node && node->kind == CanvasNodeKind::Step) {
            steps.append(id);
        }
    }
    return steps;
}

void CanvasView::selectNodes(const QStringList &ids) {
    m_scene->clearSelection();
    for (const QString &id : ids) {
        if (CanvasNodeItem *item = m_nodes.value(id)) {
            item->setSelected(true);
            item->setZValue(++m_topZ);
        }
    }
}

void CanvasView::centerOnNode(const QString &id) {
    if (CanvasNodeItem *item = m_nodes.value(id)) {
        centerOn(item->sceneBoundingRect().center());
    }
}

void CanvasView::setRunState(const QString &stepId, RunState state) {
    if (state == RunState::Idle) {
        m_runStates.remove(stepId);
    } else {
        m_runStates.insert(stepId, state);
    }
    if (CanvasNodeItem *item = m_nodes.value(stepId)) {
        item->setRunState(state);
    }
    updatePulse();
}

void CanvasView::updatePulse() {
    const bool running = std::any_of(m_runStates.cbegin(), m_runStates.cend(),
                                     [](RunState state) { return state == RunState::Running; });
    if (!running) {
        m_pulseTimer.stop();
        return;
    }
    if (!m_pulseTimer.isActive()) {
        m_pulseClock.start();
        m_pulseTimer.start();
    }
    const qreal phase = qreal(m_pulseClock.elapsed() % kPulsePeriodMs) / kPulsePeriodMs;
    const qreal level = 0.5 - 0.5 * std::cos(2.0 * M_PI * phase);
    for (auto it = m_runStates.cbegin(); it != m_runStates.cend(); ++it) {
        if (it.value() == RunState::Running) {
            if (CanvasNodeItem *item = m_nodes.value(it.key())) {
                item->setPulse(level);
            }
        }
    }
}

QString CanvasView::addNoteAt(const QPointF &scenePos, bool edit) {
    const QString id = m_model.addNote(freeSpot(scenePos), QString(), CanvasPalette::defaultNoteColor());
    selectNodes({id});
    if (edit) {
        editNode(id);
    }
    return id;
}

QString CanvasView::addStepAt(const QPointF &scenePos, CanvasStepOutput output) {
    const QString instruction = output == CanvasStepOutput::Tasks
                                    ? QStringLiteral("Pecah bahan yang tersambung menjadi task-task kecil yang bisa "
                                                     "dikerjakan pipeline, lengkap dengan kriteria selesainya.")
                                    : QString();
    const QString id = m_model.addStep(freeSpot(scenePos), instruction, output);
    selectNodes({id});
    return id;
}

void CanvasView::editNode(const QString &id) {
    CanvasNodeItem *item = m_nodes.value(id);
    if (!item) {
        return;
    }
    if (item->node().kind != CanvasNodeKind::Note) {
        emit nodeActivated(id);
        return;
    }
    setFocus(Qt::OtherFocusReason);
    item->beginEdit();
}

void CanvasView::deleteSelection() {
    const QStringList nodes = selectedNodeIds();
    QStringList edges;
    const QList<QGraphicsItem *> selected = m_scene->selectedItems();
    for (QGraphicsItem *item : selected) {
        if (auto *edge = qgraphicsitem_cast<CanvasEdgeItem *>(item)) {
            edges.append(edge->id());
        }
    }
    if (nodes.isEmpty() && edges.isEmpty()) {
        return;
    }
    m_model.beginMacro();
    m_model.removeEdges(edges);
    m_model.removeNodes(nodes);
    m_model.endMacro();
}

void CanvasView::duplicateSelection() {
    const QStringList copies = m_model.duplicateNodes(selectedNodeIds(), QPointF(32, 32));
    if (!copies.isEmpty()) {
        selectNodes(copies);
    }
}

QPointF CanvasView::centerScenePos() const {
    return mapToScene(viewport()->rect().center());
}

QPointF CanvasView::insertionPoint() const {
    const QPoint cursor = viewport()->mapFromGlobal(QCursor::pos());
    return viewport()->rect().contains(cursor) ? mapToScene(cursor) : centerScenePos();
}

QPointF CanvasView::freeSpot(const QPointF &pos) const {
    QPointF spot = pos;
    for (int attempt = 0; attempt < 60; ++attempt) {
        const bool taken = std::any_of(m_nodes.cbegin(), m_nodes.cend(), [&spot](const CanvasNodeItem *item) {
            return QLineF(item->pos(), spot).length() < 16.0;
        });
        if (!taken) {
            return spot;
        }
        spot += QPointF(28, 28);
    }
    return spot;
}

void CanvasView::zoomIn() {
    const auto next = std::find_if(std::begin(kZoomSteps), std::end(kZoomSteps),
                                   [this](qreal step) { return step > m_zoom * 1.01; });
    if (next != std::end(kZoomSteps)) {
        applyZoom(*next, viewport()->rect().center());
    }
}

void CanvasView::zoomOut() {
    const auto previous = std::find_if(std::rbegin(kZoomSteps), std::rend(kZoomSteps),
                                       [this](qreal step) { return step < m_zoom / 1.01; });
    if (previous != std::rend(kZoomSteps)) {
        applyZoom(*previous, viewport()->rect().center());
    }
}

void CanvasView::resetZoom() {
    applyZoom(1.0, viewport()->rect().center());
}

void CanvasView::fitAll() {
    const QRectF bounds = m_model.board().bounds();
    if (bounds.isNull()) {
        resetZoom();
        centerOn(0, 0);
        return;
    }
    const QRectF padded = bounds.adjusted(-60, -60, 60, 60);
    const QSize available = viewport()->size();
    // Kanvas yang kecil tidak diperbesar melewati 100%
    m_zoom = std::clamp(std::min(available.width() / padded.width(), available.height() / padded.height()), kMinZoom, 1.0);
    setTransform(QTransform::fromScale(m_zoom, m_zoom));
    centerOn(padded.center());
    emit zoomChanged(m_zoom);
}

void CanvasView::restoreView(const QPointF &center, qreal zoom) {
    m_zoom = std::clamp(zoom, kMinZoom, kMaxZoom);
    setTransform(QTransform::fromScale(m_zoom, m_zoom));
    centerOn(center);
    emit zoomChanged(m_zoom);
}

void CanvasView::applyZoom(qreal zoom, const QPoint &anchor) {
    zoom = std::clamp(zoom, kMinZoom, kMaxZoom);
    const QPointF focus = mapToScene(anchor);
    m_zoom = zoom;
    setTransform(QTransform::fromScale(zoom, zoom));
    scrollBy(QPointF(mapFromScene(focus) - anchor));
    emit zoomChanged(m_zoom);
}

void CanvasView::scrollBy(const QPointF &delta) {
    horizontalScrollBar()->setValue(horizontalScrollBar()->value() + qRound(delta.x()));
    verticalScrollBar()->setValue(verticalScrollBar()->value() + qRound(delta.y()));
}

bool CanvasView::event(QEvent *event) {
    // Isi widget mengambang berubah ukuran (font berganti, persen zoom melebar): pasang lagi di sudutnya
    if (event->type() == QEvent::LayoutRequest) {
        layoutOverlays();
    }
    return QGraphicsView::event(event);
}

void CanvasView::drawBackground(QPainter *painter, const QRectF &rect) {
    painter->fillRect(rect, Theme::fill(0xf8f4ee));
    // Jarak titik di layar paling sedikit 16 px: saat diperkecil jaraknya dikali empat
    qreal step = kGrid;
    while (step * m_zoom < 16.0) {
        step *= 4.0;
    }
    const qint64 firstColumn = qint64(std::floor(rect.left() / step));
    const qint64 firstRow = qint64(std::floor(rect.top() / step));
    QList<QPointF> minor;
    QList<QPointF> major;
    for (qint64 column = firstColumn; column * step <= rect.right(); ++column) {
        for (qint64 row = firstRow; row * step <= rect.bottom(); ++row) {
            const QPointF point(column * step, row * step);
            ((column % 4 == 0 && row % 4 == 0) ? major : minor).append(point);
        }
    }
    QPen pen(Theme::fill(0xd9cdbb), 1.5);
    pen.setCosmetic(true);
    pen.setCapStyle(Qt::RoundCap);
    painter->setPen(pen);
    painter->drawPoints(minor.constData(), int(minor.size()));
    pen.setColor(Theme::fill(0xb9b0a3));
    pen.setWidthF(2.5);
    painter->setPen(pen);
    painter->drawPoints(major.constData(), int(major.size()));
}

void CanvasView::wheelEvent(QWheelEvent *event) {
    if (event->modifiers() & Qt::ControlModifier) {
        // Ctrl + scroll, termasuk pinch touchpad yang dikirim Windows sebagai Ctrl + scroll halus
        const qreal steps = event->angleDelta().y() / 120.0;
        if (steps != 0.0) {
            applyZoom(m_zoom * std::pow(kWheelStep, steps), event->position().toPoint());
        }
        event->accept();
        return;
    }
    // Scroll biasa menggeser kanvas ke segala arah (dua jari di touchpad); Shift + scroll = mendatar
    QPointF delta = !event->pixelDelta().isNull() ? QPointF(event->pixelDelta())
                                                  : QPointF(event->angleDelta()) / 120.0 * kScrollPixelsPerNotch;
    if ((event->modifiers() & Qt::ShiftModifier) && qFuzzyIsNull(delta.x())) {
        delta = QPointF(delta.y(), 0);
    }
    scrollBy(-delta);
    event->accept();
}

bool CanvasView::viewportEvent(QEvent *event) {
    if (event->type() == QEvent::NativeGesture) {
        const auto *gesture = static_cast<QNativeGestureEvent *>(event);
        if (gesture->gestureType() == Qt::ZoomNativeGesture) {
            applyZoom(m_zoom * (1.0 + gesture->value()), gesture->position().toPoint());
            return true;
        }
    }
    return QGraphicsView::viewportEvent(event);
}

void CanvasView::mousePressEvent(QMouseEvent *event) {
    const QPoint pos = event->position().toPoint();
    if (event->button() == Qt::MiddleButton || (event->button() == Qt::LeftButton && m_spaceHeld)) {
        m_drag = Drag::Pan;
        m_lastPos = pos;
        viewport()->setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton) {
        CanvasNodeItem *node = nodeAt(pos);
        if (node && !node->isEditing()) {
            const QPointF scenePos = mapToScene(pos);
            // Titik di tepi kanan kartu, atau Alt + seret dari mana saja di kartu: tarik garis baru
            if (node->hitsOutPort(scenePos, 10.0 / m_zoom) || (event->modifiers() & Qt::AltModifier)) {
                m_scene->setFocusItem(nullptr);
                startConnection(node, scenePos);
                event->accept();
                return;
            }
            node->setZValue(++m_topZ);
        }
    }
    QGraphicsView::mousePressEvent(event);
}

void CanvasView::mouseMoveEvent(QMouseEvent *event) {
    const QPoint pos = event->position().toPoint();
    if (m_drag == Drag::Pan) {
        scrollBy(QPointF(m_lastPos - pos));
        m_lastPos = pos;
        event->accept();
        return;
    }
    if (m_drag == Drag::Connect) {
        updateConnection(mapToScene(pos));
        event->accept();
        return;
    }
    QGraphicsView::mouseMoveEvent(event);
}

void CanvasView::mouseReleaseEvent(QMouseEvent *event) {
    if (m_drag == Drag::Pan && (event->button() == Qt::MiddleButton || event->button() == Qt::LeftButton)) {
        m_drag = Drag::None;
        if (m_spaceHeld) {
            viewport()->setCursor(Qt::OpenHandCursor);
        } else {
            viewport()->unsetCursor();
        }
        event->accept();
        return;
    }
    if (m_drag == Drag::Connect && event->button() == Qt::LeftButton) {
        finishConnection(event->position().toPoint());
        event->accept();
        return;
    }
    QGraphicsView::mouseReleaseEvent(event);
    if (event->button() == Qt::LeftButton) {
        commitMoves();
    }
}

void CanvasView::mouseDoubleClickEvent(QMouseEvent *event) {
    const QPoint pos = event->position().toPoint();
    if (event->button() != Qt::LeftButton) {
        QGraphicsView::mouseDoubleClickEvent(event);
        return;
    }
    CanvasNodeItem *node = nodeAt(pos);
    if (node && node->isEditing()) {
        QGraphicsView::mouseDoubleClickEvent(event);
        return;
    }
    if (node) {
        editNode(node->id());
        event->accept();
        return;
    }
    if (edgeAt(pos)) {
        QGraphicsView::mouseDoubleClickEvent(event);
        return;
    }
    const QSizeF size = CanvasNode::defaultSize(CanvasNodeKind::Note);
    addNoteAt(mapToScene(pos) - QPointF(size.width() / 2, 24), true);
    event->accept();
}

void CanvasView::commitMoves() {
    QHash<QString, QPointF> moved;
    const QList<QGraphicsItem *> selected = m_scene->selectedItems();
    for (QGraphicsItem *item : selected) {
        if (auto *node = qgraphicsitem_cast<CanvasNodeItem *>(item)) {
            if (node->pos() != node->node().pos) {
                moved.insert(node->id(), node->pos());
            }
        }
    }
    if (!moved.isEmpty()) {
        m_model.moveNodes(moved);
    }
}

void CanvasView::nudgeSelection(const QPointF &delta) {
    QHash<QString, QPointF> moved;
    const QStringList ids = selectedNodeIds();
    for (const QString &id : ids) {
        if (const CanvasNode *node = m_model.node(id)) {
            moved.insert(id, node->pos + delta);
        }
    }
    m_model.moveNodes(moved);
}

bool CanvasView::isEditingText() const {
    const QGraphicsItem *focus = m_scene->focusItem();
    return focus && focus->type() == QGraphicsTextItem::Type;
}

void CanvasView::keyPressEvent(QKeyEvent *event) {
    if (isEditingText()) {
        QGraphicsView::keyPressEvent(event);
        return;
    }
    const bool ctrl = event->modifiers() & Qt::ControlModifier;
    const bool shift = event->modifiers() & Qt::ShiftModifier;
    const qreal nudge = shift ? 50.0 : 10.0;
    switch (event->key()) {
    case Qt::Key_Space:
        if (!event->isAutoRepeat() && m_drag == Drag::None) {
            m_spaceHeld = true;
            viewport()->setCursor(Qt::OpenHandCursor);
        }
        event->accept();
        return;
    case Qt::Key_Delete:
    case Qt::Key_Backspace:
        deleteSelection();
        event->accept();
        return;
    case Qt::Key_Escape:
        if (m_drag == Drag::Connect) {
            cancelConnection();
        } else {
            m_scene->clearSelection();
        }
        event->accept();
        return;
    case Qt::Key_N:
        if (!ctrl) {
            const QSizeF size = CanvasNode::defaultSize(CanvasNodeKind::Note);
            addNoteAt(insertionPoint() - QPointF(size.width() / 2, 24), true);
            event->accept();
            return;
        }
        break;
    case Qt::Key_L:
        if (!ctrl) {
            const QSizeF size = CanvasNode::defaultSize(CanvasNodeKind::Step);
            addStepAt(insertionPoint() - QPointF(size.width() / 2, 24));
            event->accept();
            return;
        }
        break;
    case Qt::Key_A:
        if (ctrl) {
            for (CanvasNodeItem *item : std::as_const(m_nodes)) {
                item->setSelected(true);
            }
            event->accept();
            return;
        }
        break;
    case Qt::Key_D:
        if (ctrl) {
            duplicateSelection();
            event->accept();
            return;
        }
        break;
    case Qt::Key_Z:
        if (ctrl) {
            if (shift) {
                m_model.redo();
            } else {
                m_model.undo();
            }
            event->accept();
            return;
        }
        break;
    case Qt::Key_Y:
        if (ctrl) {
            m_model.redo();
            event->accept();
            return;
        }
        break;
    case Qt::Key_0:
        if (ctrl) {
            fitAll();
            event->accept();
            return;
        }
        break;
    case Qt::Key_1:
        if (ctrl) {
            resetZoom();
            event->accept();
            return;
        }
        break;
    case Qt::Key_Plus:
    case Qt::Key_Equal:
        zoomIn();
        event->accept();
        return;
    case Qt::Key_Minus:
        zoomOut();
        event->accept();
        return;
    case Qt::Key_Return:
    case Qt::Key_Enter: {
        const QStringList steps = selectedStepIds();
        if (ctrl && !steps.isEmpty()) {
            emit runRequested(steps, shift);
        } else if (!ctrl) {
            const QStringList ids = selectedNodeIds();
            if (ids.size() == 1) {
                editNode(ids.first());
            }
        }
        event->accept();
        return;
    }
    case Qt::Key_C:
        if (!ctrl) {
            emit chatRequested(selectedNodeIds());
            event->accept();
            return;
        }
        break;
    case Qt::Key_F2: {
        const QStringList ids = selectedNodeIds();
        if (ids.size() == 1) {
            editNode(ids.first());
        }
        event->accept();
        return;
    }
    case Qt::Key_Left:
        nudgeSelection(QPointF(-nudge, 0));
        event->accept();
        return;
    case Qt::Key_Right:
        nudgeSelection(QPointF(nudge, 0));
        event->accept();
        return;
    case Qt::Key_Up:
        nudgeSelection(QPointF(0, -nudge));
        event->accept();
        return;
    case Qt::Key_Down:
        nudgeSelection(QPointF(0, nudge));
        event->accept();
        return;
    default:
        break;
    }
    QGraphicsView::keyPressEvent(event);
}

void CanvasView::keyReleaseEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat()) {
        m_spaceHeld = false;
        if (m_drag == Drag::None) {
            viewport()->unsetCursor();
        }
        event->accept();
        return;
    }
    QGraphicsView::keyReleaseEvent(event);
}

void CanvasView::focusOutEvent(QFocusEvent *event) {
    m_spaceHeld = false;
    if (m_drag == Drag::None) {
        viewport()->unsetCursor();
    }
    QGraphicsView::focusOutEvent(event);
}

void CanvasView::contextMenuEvent(QContextMenuEvent *event) {
    if (isEditingText()) {
        QGraphicsView::contextMenuEvent(event);
        return;
    }
    const QPointF scenePos = mapToScene(event->pos());
    CanvasNodeItem *node = nodeAt(event->pos());
    CanvasEdgeItem *edge = node ? nullptr : edgeAt(event->pos());

    auto *menu = new QMenu(this);
    menu->setObjectName("canvasContextMenu");
    menu->setAttribute(Qt::WA_DeleteOnClose);

    if (node) {
        if (!node->isSelected()) {
            m_scene->clearSelection();
            node->setSelected(true);
        }
        const QStringList ids = selectedNodeIds();
        const CanvasNode data = node->node();
        const QString id = data.id;
        if (data.kind == CanvasNodeKind::Note) {
            menu->addAction(QStringLiteral("Edit catatan\tF2"), this, [this, id]() { editNode(id); });
            menu->addAction(Theme::icon(":/icons/preview.svg"), QStringLiteral("Pratinjau Markdown"), this,
                            [this, id]() { emit previewRequested(id); });
            QMenu *colors = menu->addMenu(QStringLiteral("Warna kertas"));
            colors->setObjectName("canvasContextMenu");
            const QStringList keys = CanvasPalette::noteColors();
            for (const QString &key : keys) {
                QAction *action = colors->addAction(CanvasPalette::noteColorLabel(key));
                action->setCheckable(true);
                action->setChecked(key == (data.color.isEmpty() ? CanvasPalette::defaultNoteColor() : data.color));
                connect(action, &QAction::triggered, this, [this, ids, key]() {
                    m_model.beginMacro();
                    for (const QString &target : ids) {
                        m_model.setColor(target, key);
                    }
                    m_model.endMacro();
                });
            }
        } else if (data.kind == CanvasNodeKind::Step) {
            const QStringList steps = selectedStepIds();
            menu->addAction(QStringLiteral("Jalankan langkah\tCtrl+Enter"), this,
                            [this, steps]() { emit runRequested(steps, false); });
            menu->addAction(QStringLiteral("Jalankan beserta langkah hulunya\tCtrl+Shift+Enter"), this,
                            [this, steps]() { emit runRequested(steps, true); });
            menu->addAction(QStringLiteral("Buka detail"), this, [this, id]() { emit nodeActivated(id); });
        } else {
            QAction *open = menu->addAction(QStringLiteral("Buka task di board"), this,
                                            [this, data]() { emit openTaskRequested(data.source.taskId); });
            open->setEnabled(data.available);
            menu->addAction(QStringLiteral("Buka detail"), this, [this, id]() { emit nodeActivated(id); });
        }
        menu->addAction(ids.size() > 1 ? QStringLiteral("Jadikan satu task…") : QStringLiteral("Jadikan task…"), this,
                        [this, ids]() { emit createTaskRequested(ids); });
        menu->addAction(ids.size() > 1 ? QStringLiteral("Tanyakan kartu-kartu ini di chat\tC")
                                       : QStringLiteral("Tanyakan kartu ini di chat\tC"),
                        this, [this, ids]() { emit chatRequested(ids); });
        menu->addSeparator();
        menu->addAction(QStringLiteral("Duplikat\tCtrl+D"), this, &CanvasView::duplicateSelection);
        menu->addAction(Theme::icon(":/icons/trash.svg"), QStringLiteral("Hapus\tDel"), this, &CanvasView::deleteSelection);
    } else if (edge) {
        const QString edgeId = edge->id();
        menu->addAction(Theme::icon(":/icons/trash.svg"), QStringLiteral("Hapus garis"), this,
                        [this, edgeId]() { m_model.removeEdges({edgeId}); });
    } else {
        const QSizeF note = CanvasNode::defaultSize(CanvasNodeKind::Note);
        const QSizeF step = CanvasNode::defaultSize(CanvasNodeKind::Step);
        menu->addAction(QStringLiteral("Catatan baru di sini\tN"), this, [this, scenePos, note]() {
            addNoteAt(scenePos - QPointF(note.width() / 2, 24), true);
        });
        menu->addAction(QStringLiteral("Langkah AI di sini\tL"), this, [this, scenePos, step]() {
            addStepAt(scenePos - QPointF(step.width() / 2, 24));
        });
        menu->addAction(QStringLiteral("Langkah AI: pecah jadi task"), this, [this, scenePos, step]() {
            addStepAt(scenePos - QPointF(step.width() / 2, 24), CanvasStepOutput::Tasks);
        });
        menu->addSeparator();
        QAction *selectAll = menu->addAction(QStringLiteral("Pilih semua\tCtrl+A"), this, [this]() {
            for (CanvasNodeItem *item : std::as_const(m_nodes)) {
                item->setSelected(true);
            }
        });
        selectAll->setEnabled(!m_nodes.isEmpty());
        menu->addAction(QStringLiteral("Tampilkan semua kartu\tCtrl+0"), this, &CanvasView::fitAll);
        menu->addAction(QStringLiteral("Tanya tentang seluruh kanvas di chat\tC"), this, [this]() {
            m_scene->clearSelection();
            emit chatRequested({});
        });
    }
    menu->popup(event->globalPos());
    event->accept();
}

void CanvasView::dragEnterEvent(QDragEnterEvent *event) {
    if (event->mimeData()->hasFormat(CanvasLibrary::kMimeType)) {
        event->setDropAction(Qt::CopyAction);
        event->accept();
        return;
    }
    event->ignore();
}

void CanvasView::dragMoveEvent(QDragMoveEvent *event) {
    if (event->mimeData()->hasFormat(CanvasLibrary::kMimeType)) {
        event->setDropAction(Qt::CopyAction);
        event->accept();
        return;
    }
    event->ignore();
}

void CanvasView::dropEvent(QDropEvent *event) {
    const QList<CanvasSource> sources = CanvasLibrary::sources(event->mimeData());
    if (sources.isEmpty()) {
        event->ignore();
        return;
    }
    emit sourcesDropped(sources, mapToScene(event->position().toPoint()));
    event->setDropAction(Qt::CopyAction);
    event->accept();
    setFocus(Qt::OtherFocusReason);
}

void CanvasView::resizeEvent(QResizeEvent *event) {
    QGraphicsView::resizeEvent(event);
    m_emptyHint->setGeometry(viewport()->rect().adjusted(40, 40, -40, -40));
    layoutOverlays();
}

CanvasNodeItem *CanvasView::nodeAt(const QPoint &viewportPos) const {
    const QList<QGraphicsItem *> hits = items(viewportPos);
    for (QGraphicsItem *hit : hits) {
        for (QGraphicsItem *item = hit; item; item = item->parentItem()) {
            if (auto *node = qgraphicsitem_cast<CanvasNodeItem *>(item)) {
                return node;
            }
        }
    }
    return nullptr;
}

CanvasEdgeItem *CanvasView::edgeAt(const QPoint &viewportPos) const {
    const QList<QGraphicsItem *> hits = items(viewportPos);
    for (QGraphicsItem *hit : hits) {
        if (auto *edge = qgraphicsitem_cast<CanvasEdgeItem *>(hit)) {
            return edge;
        }
    }
    return nullptr;
}

void CanvasView::startConnection(CanvasNodeItem *from, const QPointF &scenePos) {
    cancelConnection();
    m_drag = Drag::Connect;
    m_connectFrom = from;
    m_connectLine = new QGraphicsPathItem();
    QPen pen(Theme::fill(0xa9743f), 1.6, Qt::DashLine);
    pen.setCosmetic(true);
    m_connectLine->setPen(pen);
    m_connectLine->setZValue(1e9);
    m_scene->addItem(m_connectLine);
    updateConnection(scenePos);
    viewport()->setCursor(Qt::CrossCursor);
}

void CanvasView::updateConnection(const QPointF &scenePos) {
    if (!m_connectLine || !m_connectFrom) {
        return;
    }
    QPointF end = scenePos;
    CanvasNodeItem *target = nodeAt(mapFromScene(scenePos));
    if (target && target != m_connectFrom) {
        end = target->inPort();
    }
    m_connectLine->setPath(canvasConnectorPath(m_connectFrom->outPort(), end));
}

void CanvasView::finishConnection(const QPoint &viewportPos) {
    CanvasNodeItem *from = m_connectFrom;
    CanvasNodeItem *target = nodeAt(viewportPos);
    const QPointF scenePos = mapToScene(viewportPos);
    cancelConnection();
    if (!from || target == from) {
        return;
    }
    if (target) {
        QString reason;
        if (m_model.connectNodes(from->id(), target->id(), &reason).isEmpty()) {
            emit message(QStringLiteral("Tidak tersambung: %1").arg(reason), true);
        }
        return;
    }
    // Dilepas di ruang kosong: catatan baru yang langsung tersambung, siap diketik
    const QString fromId = from->id();
    const QSizeF size = CanvasNode::defaultSize(CanvasNodeKind::Note);
    m_model.beginMacro();
    const QString note = m_model.addNote(scenePos - QPointF(0, size.height() / 2), QString(),
                                         CanvasPalette::defaultNoteColor());
    m_model.connectNodes(fromId, note);
    m_model.endMacro();
    selectNodes({note});
    editNode(note);
}

void CanvasView::cancelConnection() {
    if (m_connectLine) {
        delete m_connectLine;
        m_connectLine = nullptr;
    }
    m_connectFrom = nullptr;
    if (m_drag == Drag::Connect) {
        m_drag = Drag::None;
        viewport()->unsetCursor();
    }
}

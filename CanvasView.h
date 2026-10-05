#pragma once

#include "AgentTypes.h"
#include "CanvasBoard.h"

#include <QElapsedTimer>
#include <QGraphicsView>
#include <QHash>
#include <QImage>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <functional>

class CanvasEdgeItem;
class CanvasModel;
class CanvasNodeItem;
class QGraphicsPathItem;
class QLabel;

// Kanvas tak terbatas: grid titik tanpa tepi, geser (spasi + seret, tombol tengah, scroll touchpad),
// zoom di titik kursor (Ctrl + scroll, pinch, tombol), seleksi kotak, dan garis sambungan yang ditarik
// dari titik di tepi kanan kartu. Tampilan mengikuti CanvasModel lewat sinyalnya; setiap perubahan
// dari pengguna dikirim sebagai intent ke model, termasuk posisi kartu setelah selesai digeser.
class CanvasView : public QGraphicsView {
    Q_OBJECT

public:
    using ThumbnailProvider = std::function<QImage(const CanvasSource &source)>;

    explicit CanvasView(CanvasModel &model, QWidget *parent = nullptr);
    ~CanvasView() override;

    void setThumbnailProvider(ThumbnailProvider provider);
    // Catatan yang sedang diketik langsung di kanvas disimpan ke model sekarang
    void finishEditing();

    qreal zoom() const { return m_zoom; }
    void zoomIn();
    void zoomOut();
    void resetZoom();
    void fitAll();
    void restoreView(const QPointF &center, qreal zoom);
    QPointF centerScenePos() const;
    // Titik di dekat pos yang belum ditempati pojok kartu lain, supaya kartu baru tidak menumpuk
    QPointF freeSpot(const QPointF &pos) const;

    QStringList selectedNodeIds() const;
    void selectNodes(const QStringList &ids);
    void centerOnNode(const QString &id);

    void setRunState(const QString &stepId, RunState state);

    // Kartu baru: posisi = pojok kiri atas di koordinat kanvas
    QString addNoteAt(const QPointF &scenePos, bool edit);
    QString addStepAt(const QPointF &scenePos, CanvasStepOutput output = CanvasStepOutput::Document);
    void editNode(const QString &id);
    void deleteSelection();
    void duplicateSelection();
    // Posisi kursor bila di dalam kanvas, selain itu tengah tampilan
    QPointF insertionPoint() const;

signals:
    void selectionChanged();
    void zoomChanged(qreal zoom);
    void message(const QString &text, bool error);
    void sourcesDropped(const QList<CanvasSource> &sources, const QPointF &scenePos);
    void nodeActivated(const QString &id);
    void runRequested(const QStringList &stepIds, bool withUpstream);
    void createTaskRequested(const QStringList &ids);
    void openTaskRequested(const QString &taskId);

protected:
    void drawBackground(QPainter *painter, const QRectF &rect) override;
    void wheelEvent(QWheelEvent *event) override;
    bool viewportEvent(QEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void rebuild();
    void addNodeItem(const CanvasNode &node);
    void addEdgeItem(const CanvasEdge &edge);
    void removeNodeItem(const QString &id);
    void removeEdgeItem(const QString &id);
    void updateEmptyHint();
    void commitMoves();
    void nudgeSelection(const QPointF &delta);
    void applyZoom(qreal zoom, const QPoint &anchor);
    void scrollBy(const QPointF &delta);
    bool isEditingText() const;
    CanvasNodeItem *nodeAt(const QPoint &viewportPos) const;
    CanvasEdgeItem *edgeAt(const QPoint &viewportPos) const;
    void startConnection(CanvasNodeItem *from, const QPointF &scenePos);
    void updateConnection(const QPointF &scenePos);
    void finishConnection(const QPoint &viewportPos);
    void cancelConnection();
    void updatePulse();
    QStringList selectedStepIds() const;

    CanvasModel &m_model;
    QGraphicsScene *m_scene;
    QHash<QString, CanvasNodeItem *> m_nodes;
    QHash<QString, CanvasEdgeItem *> m_edges;
    QHash<QString, RunState> m_runStates;
    ThumbnailProvider m_thumbnails;
    QLabel *m_emptyHint;
    qreal m_zoom = 1.0;

    enum class Drag { None, Pan, Connect };
    Drag m_drag = Drag::None;
    QPoint m_lastPos;
    bool m_spaceHeld = false;
    CanvasNodeItem *m_connectFrom = nullptr;
    QGraphicsPathItem *m_connectLine = nullptr;

    QTimer m_pulseTimer;
    QElapsedTimer m_pulseClock;
    qreal m_topZ = 0.0;
};

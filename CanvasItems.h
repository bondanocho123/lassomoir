#pragma once

#include "AgentTypes.h"
#include "CanvasBoard.h"

#include <QColor>
#include <QGraphicsObject>
#include <QGraphicsTextItem>
#include <QImage>
#include <QList>
#include <QPainterPath>
#include <QPolygonF>
#include <QString>
#include <QStringList>

#include <functional>

class CanvasEdgeItem;

// Warna kertas catatan. Kuncinya yang disimpan di canvas.json; warnanya ikut tema terang/gelap.
namespace CanvasPalette {
QStringList noteColors();
QString defaultNoteColor();
QString noteColorLabel(const QString &key);
QColor noteFill(const QString &key);
}

// Jalur garis sambungan: keluar ke kanan dari `from`, masuk dari kiri ke `to`
QPainterPath canvasConnectorPath(const QPointF &from, const QPointF &to);

// Editor teks catatan langsung di atas kartunya. Selesai (isi disimpan) saat fokus pindah, Esc,
// atau Ctrl+Enter.
class CanvasTextEditor final : public QGraphicsTextItem {
public:
    explicit CanvasTextEditor(QGraphicsItem *parent);
    std::function<void()> finished;

    void paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget) override;

protected:
    void focusOutEvent(QFocusEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    bool m_done = false;
};

// Satu kartu kanvas. Data datang dari CanvasModel (setNode); posisi selama digeser dan ukuran selama
// ditarik hanya sementara sampai CanvasView mengirim intent-nya ke model.
class CanvasNodeItem final : public QGraphicsObject {
    Q_OBJECT

public:
    enum { Type = UserType + 1 };

    explicit CanvasNodeItem(const CanvasNode &node);
    ~CanvasNodeItem() override;
    int type() const override { return Type; }

    const CanvasNode &node() const { return m_node; }
    QString id() const { return m_node.id; }
    void setNode(const CanvasNode &node);

    void setRunState(RunState state);
    RunState runState() const { return m_runState; }
    void setPulse(qreal level);   // 0..1, digerakkan CanvasView selama langkah berjalan

    bool wantsThumbnail() const;
    void setThumbnail(const QImage &image);

    QRectF cardRect() const { return QRectF(QPointF(0, 0), m_size); }
    QPointF inPort() const;    // koordinat scene
    QPointF outPort() const;
    // tolerance dalam satuan scene, supaya titiknya tetap mudah dikenai di zoom berapa pun
    bool hitsOutPort(const QPointF &scenePos, qreal tolerance) const;

    void addEdge(CanvasEdgeItem *edge);
    void removeEdge(CanvasEdgeItem *edge);
    const QList<CanvasEdgeItem *> &edges() const { return m_edges; }

    void beginEdit();
    bool isEditing() const { return m_editor != nullptr; }

    QRectF boundingRect() const override;
    QPainterPath shape() const override;
    void paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget) override;

signals:
    void resized(const QString &id, const QSizeF &size);
    // needed: ukuran yang dibutuhkan isinya (kartu hanya membesar)
    void edited(const QString &id, const QString &text, const QSizeF &needed);

protected:
    QVariant itemChange(GraphicsItemChange change, const QVariant &value) override;
    void hoverEnterEvent(QGraphicsSceneHoverEvent *event) override;
    void hoverMoveEvent(QGraphicsSceneHoverEvent *event) override;
    void hoverLeaveEvent(QGraphicsSceneHoverEvent *event) override;
    void mousePressEvent(QGraphicsSceneMouseEvent *event) override;
    void mouseMoveEvent(QGraphicsSceneMouseEvent *event) override;
    void mouseReleaseEvent(QGraphicsSceneMouseEvent *event) override;

private:
    qreal viewScale() const;
    bool hitsResizeHandle(const QPointF &localPos) const;
    void finishEdit();
    void updateEdges();
    void refreshTexts();
    void paintNote(QPainter *painter, const QFont &base);
    void paintCard(QPainter *painter, const QFont &base);
    void paintOverview(QPainter *painter, const QFont &base, qreal detail);

    CanvasNode m_node;
    QSizeF m_size;
    RunState m_runState = RunState::Idle;
    qreal m_pulse = 0.0;
    bool m_hovered = false;
    bool m_resizing = false;
    QPointF m_resizeOrigin;
    QSizeF m_resizeStart;
    QList<CanvasEdgeItem *> m_edges;
    CanvasTextEditor *m_editor = nullptr;
    QImage m_thumbnail;
    // Teks yang dilukis, dihitung sekali per perubahan data (paint sering dipanggil)
    QString m_heading;
    QString m_meta;
    QString m_preview;
};

// Garis berarah antar kartu; ikut bergerak bersama kedua kartunya
class CanvasEdgeItem final : public QGraphicsItem {
public:
    enum { Type = UserType + 2 };

    CanvasEdgeItem(const CanvasEdge &edge, CanvasNodeItem *from, CanvasNodeItem *to);
    int type() const override { return Type; }

    QString id() const { return m_edge.id; }
    CanvasNodeItem *from() const { return m_from; }
    CanvasNodeItem *to() const { return m_to; }
    void updatePath();

    QRectF boundingRect() const override;
    QPainterPath shape() const override;
    void paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget) override;

protected:
    void hoverEnterEvent(QGraphicsSceneHoverEvent *event) override;
    void hoverLeaveEvent(QGraphicsSceneHoverEvent *event) override;

private:
    CanvasEdge m_edge;
    CanvasNodeItem *m_from;
    CanvasNodeItem *m_to;
    QPainterPath m_path;
    QPolygonF m_arrow;
    bool m_hovered = false;
};

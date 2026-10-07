#pragma once

#include "AgentTypes.h"
#include "CanvasBoard.h"

#include <QColor>
#include <QFont>
#include <QGraphicsObject>
#include <QGraphicsTextItem>
#include <QImage>
#include <QList>
#include <QPainterPath>
#include <QPolygonF>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>

class CanvasEdgeItem;
class CanvasNodeItem;

// Warna kertas catatan. Kuncinya yang disimpan di canvas.json; warnanya ikut tema terang/gelap.
namespace CanvasPalette {
QStringList noteColors();
QString defaultNoteColor();
QString noteColorLabel(const QString &key);
QColor noteFill(const QString &key);
}

// Sisi kartu tempat garis menempel; titik tempelnya di tengah sisi itu
enum class CanvasSide { Left, Top, Right, Bottom };

// Sisi keluar garis di kartu asal dan sisi masuknya di kartu tujuan
struct CanvasRoute {
    CanvasSide from = CanvasSide::Right;
    CanvasSide to = CanvasSide::Left;

    bool operator==(const CanvasRoute &other) const { return from == other.from && to == other.to; }
    bool operator!=(const CanvasRoute &other) const { return !(*this == other); }
};

QPointF canvasPort(const QRectF &rect, CanvasSide side);

// Garis menempel di dua sisi yang saling berhadapan, menurut letak tujuannya: mendatar bila tujuan di
// kiri/kanan, tegak bila di atas/bawah. Tujuan yang letaknya serong punya dua pilihan lorong; dipakai
// lorong yang tidak diisi kartu lain (`occupied`), dan bila sama-sama kosong atau sama-sama terisi,
// lorong yang garisnya lebih lurus (seri: mendatar, arah baca kanvas). Tanpa `occupied` semua lorong
// dianggap kosong. `to` boleh berupa titik (ukuran nol), mis. posisi kursor selagi garis ditarik.
CanvasRoute canvasRoute(const QRectF &from, const QRectF &to,
                        const std::function<bool(const QRectF &lane)> &occupied = {});

// Jalur garis antara dua titik tempel: meninggalkan dan mendatangi kartu tegak lurus sisinya
QPainterPath canvasConnectorPath(const QPointF &from, CanvasSide fromSide, const QPointF &to, CanvasSide toSide);

// Ada kartu selain `from` dan `to` yang masuk ke lorong itu (jawaban `occupied` untuk canvasRoute)
bool canvasLaneOccupied(const QGraphicsScene *scene, const QRectF &lane, const CanvasNodeItem *from,
                        const CanvasNodeItem *to);

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
//
// Isi kartu ditata menurut ukurannya di layar. Pada zoom biasa hurufnya ikut membesar dan mengecil
// bersama kartunya; saat kanvas diperkecil huruf berhenti mengecil di batas yang masih terbaca, dan
// kartu menampilkan sebanyak yang muat: judul lebih dulu, lalu keterangan dan isinya. Yang tidak muat
// dipotong di batas baris dengan elipsis, tidak pernah di tengah huruf.
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
    // Zoom tampilan: titik sambung, pegangan, dan garis tepi berukuran px layar, jadi daerah lukis
    // kartu di kanvas melebar saat diperkecil
    void setViewScale(qreal scale);

    bool wantsThumbnail() const;
    void setThumbnail(const QImage &image);

    QRectF cardRect() const { return QRectF(QPointF(0, 0), m_size); }
    QRectF sceneCardRect() const { return QRectF(pos(), m_size); }
    QPointF port(CanvasSide side) const;   // koordinat scene
    // Di salah satu dari empat titik sambung. tolerance dalam satuan scene, supaya titiknya tetap
    // mudah dikenai di zoom berapa pun.
    bool hitsPort(const QPointF &scenePos, qreal tolerance) const;

    void addEdge(CanvasEdgeItem *edge);
    void removeEdge(CanvasEdgeItem *edge);
    const QList<CanvasEdgeItem *> &edges() const { return m_edges; }

    void beginEdit();
    bool isEditing() const { return m_editor != nullptr; }

    // Baris teks yang tampil di kartu pada skala tampilan itu, dari atas ke bawah: judul, keterangan,
    // lalu isi. Nama jenis dan status di pita kartu tidak termasuk.
    QStringList visibleText(qreal scale) const;

    QRectF boundingRect() const override;
    QPainterPath shape() const override;
    void paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget) override;

signals:
    void resized(const QString &id, const QSizeF &size);
    // needed: ukuran yang dibutuhkan isinya (kartu hanya membesar)
    void edited(const QString &id, const QString &text, const QSizeF &needed);
    // Posisi atau ukuran kartu berubah: garis kartu lain yang lorongnya dilewati kartu ini perlu ditata lagi
    void geometryChanged();

protected:
    QVariant itemChange(GraphicsItemChange change, const QVariant &value) override;
    void hoverEnterEvent(QGraphicsSceneHoverEvent *event) override;
    void hoverMoveEvent(QGraphicsSceneHoverEvent *event) override;
    void hoverLeaveEvent(QGraphicsSceneHoverEvent *event) override;
    void mousePressEvent(QGraphicsSceneMouseEvent *event) override;
    void mouseMoveEvent(QGraphicsSceneMouseEvent *event) override;
    void mouseReleaseEvent(QGraphicsSceneMouseEvent *event) override;

private:
    struct Layout;

    QFont baseFont() const;
    qreal resizeHandleSide() const;
    bool hitsResizeHandle(const QPointF &localPos) const;
    void finishEdit();
    void updateEdges();
    void refreshTexts();
    const Layout &layoutFor(qreal scale, const QFont &base) const;
    void layoutNote(Layout &layout) const;
    void layoutCard(Layout &layout) const;
    void paintNote(QPainter *painter, const Layout &layout);
    void paintCard(QPainter *painter, const Layout &layout);
    void paintHandles(QPainter *painter, qreal scale);

    CanvasNode m_node;
    QSizeF m_size;
    RunState m_runState = RunState::Idle;
    qreal m_pulse = 0.0;
    qreal m_viewScale = 1.0;
    qreal m_boundsScale = 1.0;   // zoom yang dibulatkan untuk daerah lukis
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
    QString m_summary;   // Catatan: isinya sesudah baris judul, tanpa tanda Markdown
    // Tata letak terakhir; berlaku selama isi (m_revision), ukuran, dan skala tampilannya sama
    int m_revision = 0;
    mutable std::unique_ptr<Layout> m_layout;
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
    // Pilih lagi sisi tempelnya menurut letak kedua kartu dan kartu lain di sekitarnya
    void updatePath();
    // Zoom tampilan: mata panah dan daerah kliknya tidak mengecil di bawah ukuran yang masih terlihat
    void setViewScale(qreal scale);

    CanvasRoute route() const { return m_route; }
    QPointF start() const { return m_start; }   // titik tempel di kartu asal, koordinat scene
    QPointF tip() const { return m_tip; }       // ujung mata panah di kartu tujuan

    QRectF boundingRect() const override;
    QPainterPath shape() const override;
    void paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget) override;

protected:
    QVariant itemChange(GraphicsItemChange change, const QVariant &value) override;
    void hoverEnterEvent(QGraphicsSceneHoverEvent *event) override;
    void hoverLeaveEvent(QGraphicsSceneHoverEvent *event) override;

private:
    // Garis, mata panah, dan daerah lukis untuk rute dan zoom sekarang. Daerah lukis hanya berubah
    // bersama rute, titik tempel, atau m_boundsScale; pemanggil memberi tahu scene lebih dulu.
    void rebuild();

    CanvasEdge m_edge;
    CanvasNodeItem *m_from;
    CanvasNodeItem *m_to;
    CanvasRoute m_route;
    QPointF m_start;
    QPointF m_tip;
    qreal m_viewScale = 1.0;
    qreal m_boundsScale = 1.0;   // zoom yang dibulatkan untuk daerah lukis
    QRectF m_bounds;
    QPainterPath m_path;
    QPolygonF m_arrow;
    mutable QPainterPath m_shape;   // daerah klik: garis + mata panah; kosong = belum dihitung
    bool m_hovered = false;
};

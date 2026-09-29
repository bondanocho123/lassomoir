#include "DiagramViewer.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHash>
#include <QKeySequence>
#include <QLabel>
#include <QList>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScreen>
#include <QScrollBar>
#include <QShortcut>
#include <QStandardPaths>
#include <QStyleOptionGraphicsItem>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <functional>
#include <iterator>

namespace {

constexpr qreal kMinZoom = 0.1;      // batas bawah, kecuali Paskan butuh lebih kecil
constexpr qreal kMaxZoom = 4.0;      // 400%: teks diagram 15 px jadi 60 px
constexpr qreal kWheelStep = 1.2;    // satu takik roda mouse (120) = ×1.2; pinch touchpad lebih halus
constexpr int kSheetPadding = 16;    // tepi kertas putih di sekeliling diagram (satuan diagram)
constexpr int kViewMargin = 24;      // jarak kertas ke tepi jendela (piksel layar), berapa pun zoom-nya
// Tangga zoom tombol −/+ dan pintasan keyboard
constexpr qreal kZoomSteps[] = {0.1, 0.25, 0.33, 0.5, 0.67, 0.75, 1.0, 1.25, 1.5, 2.0, 3.0, 4.0};

// Diagram dengan mipmap. Diagram besar diperkecil jauh saat Paskan, dan bilinear langsung dari
// gambar penuh membuat garis tipis serta teks bergerigi. Jadi yang digambar adalah salinan yang
// sudah diperkecil halus: tiap tingkat separuh tingkat sebelumnya, dibuat saat pertama dibutuhkan.
class DiagramItem final : public QGraphicsItem {
public:
    explicit DiagramItem(const QImage &image) : m_size(image.deviceIndependentSize()) {
        const bool drawable = image.format() == QImage::Format_ARGB32_Premultiplied
                              || image.format() == QImage::Format_RGB32;
        m_levels.append(drawable ? image : image.convertToFormat(QImage::Format_ARGB32_Premultiplied));
        setFlag(QGraphicsItem::ItemUsesExtendedStyleOption);   // exposedRect: gambar bagian yang terlihat saja
    }

    QRectF boundingRect() const override { return QRectF(QPointF(0, 0), m_size); }

    void paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget) override {
        Q_UNUSED(widget);
        const QRectF target = option->exposedRect.intersected(boundingRect());
        if (target.isEmpty() || m_levels.first().isNull()) {
            return;
        }
        // Piksel layar per piksel gambar penuh
        const qreal scale = option->levelOfDetailFromTransform(painter->worldTransform())
                            * painter->device()->devicePixelRatio() * m_size.width() / m_levels.first().width();
        const QImage &level = levelFor(scale);
        const qreal fx = level.width() / m_size.width();
        const qreal fy = level.height() / m_size.height();
        painter->setRenderHint(QPainter::SmoothPixmapTransform);
        painter->drawImage(target, level,
                           QRectF(target.x() * fx, target.y() * fy, target.width() * fx, target.height() * fy));
    }

private:
    // Tingkat terkecil yang masih >= skala tampil, jadi saat digambar diperkecil paling banyak 2×
    const QImage &levelFor(qreal scale) {
        qsizetype index = 0;
        while (scale < 0.5) {
            const QImage &current = m_levels.at(index);
            if (current.width() < 2 || current.height() < 2) {
                break;
            }
            if (index + 1 == m_levels.size()) {
                m_levels.append(current.scaled((current.width() + 1) / 2, (current.height() + 1) / 2,
                                               Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
            }
            ++index;
            scale *= 2;
        }
        return m_levels.at(index);
    }

    QSizeF m_size;              // ukuran logis diagram (100%)
    QList<QImage> m_levels;     // [0] = gambar penuh
};

}

// Kanvas: diagram di atas kertas putih, meja di sekelilingnya. Zoom lewat transform view,
// geser lewat scrollbar (seret = ScrollHandDrag).
class DiagramCanvas final : public QGraphicsView {
public:
    DiagramCanvas(const QImage &image, QWidget *parent) : QGraphicsView(parent) {
        setObjectName("diagramCanvas");
        setFrameShape(QFrame::NoFrame);
        setFocusPolicy(Qt::StrongFocus);
        setDragMode(QGraphicsView::ScrollHandDrag);
        setTransformationAnchor(QGraphicsView::NoAnchor);   // titik jangkar dijaga applyZoom()
        setResizeAnchor(QGraphicsView::AnchorViewCenter);
        setAlignment(Qt::AlignCenter);

        auto *scene = new QGraphicsScene(this);
        auto *diagram = new DiagramItem(image);
        m_sheet = diagram->boundingRect().adjusted(-kSheetPadding, -kSheetPadding, kSheetPadding, kSheetPadding);
        // Kertas putih seperti panel dokumen di drawer; tepi 1 px di semua zoom (pena kosmetik)
        scene->addRect(m_sheet, QPen(QColor(0xe2, 0xd5, 0xc1), 0), QBrush(Qt::white));
        scene->addItem(diagram);
        setScene(scene);
        applyZoom(1.0, QPoint());
    }

    qreal zoom() const { return m_zoom; }
    bool fitMode() const { return m_fitMode; }

    qreal fitZoom() const {
        // Ukuran tanpa scrollbar, karena saat Paskan seluruh diagram muat. Cadangan 2 px untuk
        // pembulatan, supaya scrollbar tidak muncul-hilang bergantian saat jendela diubah ukurannya.
        const QSize available = maximumViewportSize() - QSize(2 * kViewMargin + 2, 2 * kViewMargin + 2);
        const qreal zoom = std::min(available.width() / m_sheet.width(), available.height() / m_sheet.height());
        return std::clamp(zoom, 0.01, kMaxZoom);
    }

    qreal minimumZoom() const { return std::min(kMinZoom, fitZoom()); }

    // Zoom dengan titik diagram di bawah `anchor` (koordinat viewport) tetap di tempatnya
    void zoomAt(qreal zoom, const QPoint &anchor) {
        m_fitMode = false;
        applyZoom(zoom, anchor);
    }

    void zoomAtCenter(qreal zoom) { zoomAt(zoom, viewport()->rect().center()); }

    void fit() {
        m_fitMode = true;
        applyZoom(fitZoom(), viewport()->rect().center());
    }

    std::function<void()> zoomChanged;

protected:
    void resizeEvent(QResizeEvent *event) override {
        QGraphicsView::resizeEvent(event);
        if (!m_placed) {
            // Ukuran sungguhan pertama: diagram yang lebih besar dari jendela dipaskan, yang kecil 100%
            if (viewport()->width() <= 2 * kViewMargin || viewport()->height() <= 2 * kViewMargin) {
                return;
            }
            m_placed = true;
            if (fitZoom() < 1.0) {
                fit();
            } else {
                zoomAtCenter(1.0);
            }
        } else if (m_fitMode) {
            fit();
        } else if (zoomChanged) {
            zoomChanged();   // batas zoom terkecil ikut ukuran jendela
        }
    }

    void wheelEvent(QWheelEvent *event) override {
        if (event->modifiers() & Qt::ControlModifier) {
            // Ctrl + scroll, termasuk pinch touchpad (Windows mengirimnya sebagai Ctrl + scroll halus)
            const qreal steps = event->angleDelta().y() / 120.0;
            if (steps != 0.0) {
                zoomAt(m_zoom * std::pow(kWheelStep, steps), event->position().toPoint());
            }
            event->accept();
            return;
        }
        if ((event->modifiers() & Qt::ShiftModifier) && event->angleDelta().x() == 0) {
            // Shift + scroll: geser mendatar
            QWheelEvent sideways(event->position(), event->globalPosition(), event->pixelDelta().transposed(),
                                 event->angleDelta().transposed(), event->buttons(),
                                 event->modifiers() & ~Qt::ShiftModifier, event->phase(), event->inverted(),
                                 Qt::MouseEventNotSynthesized, event->pointingDevice());
            QGraphicsView::wheelEvent(&sideways);
            event->setAccepted(sideways.isAccepted());
            return;
        }
        QGraphicsView::wheelEvent(event);
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override {
        if (event->button() != Qt::LeftButton) {
            QGraphicsView::mouseDoubleClickEvent(event);
            return;
        }
        // Paskan ↔ 100% di titik yang diklik
        if (m_fitMode) {
            zoomAt(1.0, event->position().toPoint());
        } else {
            fit();
        }
        event->accept();
    }

    bool viewportEvent(QEvent *event) override {
        // Pinch touchpad di platform yang mengirimnya sebagai gesture (bukan Ctrl + scroll)
        if (event->type() == QEvent::NativeGesture) {
            const auto *gesture = static_cast<QNativeGestureEvent *>(event);
            if (gesture->gestureType() == Qt::ZoomNativeGesture) {
                zoomAt(m_zoom * (1.0 + gesture->value()), gesture->position().toPoint());
                return true;
            }
        }
        return QGraphicsView::viewportEvent(event);
    }

private:
    void applyZoom(qreal zoom, const QPoint &anchor) {
        zoom = std::clamp(zoom, minimumZoom(), kMaxZoom);
        const QPointF focus = mapToScene(anchor);
        m_zoom = zoom;
        // Ruang di sekeliling kertas tetap kViewMargin piksel layar, jadi tepi diagram bisa digeser
        // sampai terlihat utuh di zoom berapa pun
        const qreal margin = kViewMargin / zoom;
        setSceneRect(m_sheet.adjusted(-margin, -margin, margin, margin));
        setTransform(QTransform::fromScale(zoom, zoom));
        const QPoint drift = mapFromScene(focus) - anchor;
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() + drift.x());
        verticalScrollBar()->setValue(verticalScrollBar()->value() + drift.y());
        if (zoomChanged) {
            zoomChanged();
        }
    }

    QRectF m_sheet;            // kertas: diagram + kSheetPadding
    qreal m_zoom = 1.0;
    bool m_fitMode = false;
    bool m_placed = false;     // zoom awal sudah dipilih (butuh ukuran viewport sungguhan)
};

DiagramViewer::DiagramViewer(const QString &key, const QImage &image, const QString &title, QWidget *parent)
    : QDialog(parent), m_key(key), m_image(image) {
    setObjectName("diagramViewer");
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(title);
    // Bisa dimaksimalkan. Tanpa minimize: jendela milik jendela utama tidak punya tombol taskbar,
    // jadi di Windows ia mengecil jadi bilah kecil di pojok layar.
    setWindowFlag(Qt::WindowMaximizeButtonHint, true);

    m_canvas = new DiagramCanvas(image, this);

    // Tombol tidak mengambil fokus: panah, Ctrl + scroll, dan pintasan tetap diterima kanvas
    auto button = [this](const QString &text, const char *name, const QString &tip) {
        auto *result = new QPushButton(text, this);
        result->setObjectName(QLatin1String(name));
        result->setToolTip(tip);
        result->setCursor(Qt::PointingHandCursor);
        result->setFocusPolicy(Qt::NoFocus);
        result->setAutoDefault(false);
        return result;
    };
    m_zoomOut = button(QStringLiteral("−"), "btnDiagramZoomOut", QStringLiteral("Perkecil (Ctrl + −)"));
    m_zoomOut->setFixedSize(28, 26);
    m_zoomLevel = new QLabel(this);
    m_zoomLevel->setObjectName("diagramZoomLevel");
    m_zoomLevel->setAlignment(Qt::AlignCenter);
    m_zoomIn = button(QStringLiteral("+"), "btnDiagramZoomIn", QStringLiteral("Perbesar (Ctrl + +)"));
    m_zoomIn->setFixedSize(28, 26);
    m_fit = button(QStringLiteral("Paskan"), "btnDiagramFit", QStringLiteral("Tampilkan seluruh diagram (Ctrl + 0)"));
    m_fit->setCheckable(true);
    m_actualSize = button(QStringLiteral("100%"), "btnDiagramActualSize", QStringLiteral("Ukuran asli (Ctrl + 1)"));
    m_actualSize->setCheckable(true);
    auto *hint = new QLabel(QStringLiteral("Ctrl + scroll untuk zoom · seret untuk menggeser"), this);
    hint->setObjectName("diagramHint");
    hint->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto *external = button(QStringLiteral("Buka di aplikasi lain"), "btnDiagramOpenExternal",
                            QStringLiteral("Buka PNG diagram di penampil gambar bawaan Windows"));

    connect(m_zoomOut, &QPushButton::clicked, this, &DiagramViewer::zoomOut);
    connect(m_zoomIn, &QPushButton::clicked, this, &DiagramViewer::zoomIn);
    connect(m_fit, &QPushButton::clicked, this, &DiagramViewer::fitToWindow);
    connect(m_actualSize, &QPushButton::clicked, this, &DiagramViewer::showActualSize);
    connect(external, &QPushButton::clicked, this, &DiagramViewer::openExternally);
    m_canvas->zoomChanged = [this]() { updateControls(); };

    auto *toolbar = new QWidget(this);
    toolbar->setObjectName("diagramToolbar");
    toolbar->setAttribute(Qt::WA_StyledBackground, true);
    auto *tools = new QHBoxLayout(toolbar);
    tools->setContentsMargins(12, 8, 12, 8);
    tools->setSpacing(6);
    tools->addWidget(m_zoomOut);
    tools->addWidget(m_zoomLevel);
    tools->addWidget(m_zoomIn);
    tools->addSpacing(8);
    tools->addWidget(m_fit);
    tools->addWidget(m_actualSize);
    tools->addSpacing(12);
    tools->addWidget(hint, 1);
    tools->addWidget(external);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addWidget(toolbar);
    root->addWidget(m_canvas, 1);

    // Ctrl + +/=/− (juga +/− tanpa Ctrl), Ctrl + 0 = Paskan, Ctrl + 1 = 100%. Esc menutup (QDialog).
    // Urutan tombol yang sama tidak boleh terdaftar dua kali: QShortcut ganda jadi ambigu dan diam.
    auto bind = [this](const QList<QKeySequence> &keys, void (DiagramViewer::*action)()) {
        QList<QKeySequence> unique;
        for (const QKeySequence &key : keys) {
            if (!key.isEmpty() && !unique.contains(key)) {
                unique.append(key);
            }
        }
        for (const QKeySequence &key : std::as_const(unique)) {
            connect(new QShortcut(key, this), &QShortcut::activated, this, action);
        }
    };
    bind(QKeySequence::keyBindings(QKeySequence::ZoomIn)
             + QList<QKeySequence>{QKeySequence(Qt::CTRL | Qt::Key_Equal), QKeySequence(Qt::Key_Plus),
                                   QKeySequence(Qt::Key_Equal)},
         &DiagramViewer::zoomIn);
    bind(QKeySequence::keyBindings(QKeySequence::ZoomOut) + QList<QKeySequence>{QKeySequence(Qt::Key_Minus)},
         &DiagramViewer::zoomOut);
    bind({QKeySequence(Qt::CTRL | Qt::Key_0)}, &DiagramViewer::fitToWindow);
    bind({QKeySequence(Qt::CTRL | Qt::Key_1)}, &DiagramViewer::showActualSize);

    // Diagram yang muat 100% di layar: jendela seukuran diagram (minimal 720 × 480). Yang lebih
    // besar: jendela 90% × 85% layar, dan kanvas mulai di mode Paskan.
    const QScreen *screen = parent ? parent->screen() : QGuiApplication::primaryScreen();
    const QRect available = screen ? screen->availableGeometry() : QRect(0, 0, 1280, 800);
    const int around = 2 * (kSheetPadding + kViewMargin) + 4;
    const QSize large(int(available.width() * 0.9), int(available.height() * 0.85));
    QSize size = image.deviceIndependentSize().toSize() + QSize(around, around + toolbar->sizeHint().height());
    if (size.width() > large.width() || size.height() > large.height()) {
        size = large;
    }
    resize(size.expandedTo(QSize(720, 480)));

    QRect placed(QPoint(0, 0), this->size());
    placed.moveCenter(parent ? parent->window()->frameGeometry().center() : available.center());
    placed.moveLeft(std::clamp(placed.left(), available.left(), std::max(available.left(), available.right() - placed.width())));
    placed.moveTop(std::clamp(placed.top(), available.top(), std::max(available.top(), available.bottom() - placed.height())));
    move(placed.topLeft());

    m_canvas->setFocus();
    updateControls();
}

DiagramViewer *DiagramViewer::showDiagram(const QString &key, const QImage &image, const QString &title,
                                          QWidget *parent) {
    QWidget *owner = parent ? parent->window() : nullptr;
    if (owner) {
        const QList<DiagramViewer *> viewers = owner->findChildren<DiagramViewer *>(Qt::FindDirectChildrenOnly);
        for (DiagramViewer *viewer : viewers) {
            // Jendela yang ditutup sudah tersembunyi, tinggal menunggu deleteLater
            if (viewer->key() == key && viewer->isVisible()) {
                viewer->setWindowState(viewer->windowState() & ~Qt::WindowMinimized);
                viewer->raise();
                viewer->activateWindow();
                return viewer;
            }
        }
    }
    auto *viewer = new DiagramViewer(key, image, title, owner);
    viewer->show();
    viewer->raise();
    viewer->activateWindow();
    return viewer;
}

QString DiagramViewer::titleFor(const QString &code) {
    static const QHash<QString, QString> titles = {
        {QStringLiteral("classDiagram"), QStringLiteral("Diagram kelas")},
        {QStringLiteral("classDiagram-v2"), QStringLiteral("Diagram kelas")},
        {QStringLiteral("sequenceDiagram"), QStringLiteral("Diagram sequence")},
        {QStringLiteral("flowchart"), QStringLiteral("Flowchart")},
        {QStringLiteral("graph"), QStringLiteral("Flowchart")},
        {QStringLiteral("stateDiagram"), QStringLiteral("Diagram state")},
        {QStringLiteral("stateDiagram-v2"), QStringLiteral("Diagram state")},
        {QStringLiteral("erDiagram"), QStringLiteral("Diagram ER")},
    };
    static const QRegularExpression word(QStringLiteral(R"(^[\w-]+)"));

    // Jenis diagram = kata pertama baris pertama yang bukan komentar (%%) atau front matter (---)
    bool frontMatter = false;
    const QStringList lines = code.split(QLatin1Char('\n'));
    for (const QString &raw : lines) {
        const QString line = raw.trimmed();
        if (line == QLatin1String("---")) {
            frontMatter = !frontMatter;
            continue;
        }
        if (frontMatter || line.isEmpty() || line.startsWith(QLatin1String("%%"))) {
            continue;
        }
        return titles.value(word.match(line).captured(0), QStringLiteral("Diagram"));
    }
    return QStringLiteral("Diagram");
}

qreal DiagramViewer::zoom() const {
    return m_canvas->zoom();
}

qreal DiagramViewer::fitZoom() const {
    return m_canvas->fitZoom();
}

bool DiagramViewer::fitsToWindow() const {
    return m_canvas->fitMode();
}

void DiagramViewer::zoomIn() {
    const qreal zoom = m_canvas->zoom();
    const auto next = std::find_if(std::begin(kZoomSteps), std::end(kZoomSteps),
                                   [zoom](qreal step) { return step > zoom * 1.01; });
    if (next != std::end(kZoomSteps)) {
        m_canvas->zoomAtCenter(*next);
    }
}

void DiagramViewer::zoomOut() {
    const qreal zoom = m_canvas->zoom();
    const qreal floor = m_canvas->minimumZoom();
    const auto previous = std::find_if(std::rbegin(kZoomSteps), std::rend(kZoomSteps),
                                       [zoom](qreal step) { return step < zoom / 1.01; });
    const qreal target = previous != std::rend(kZoomSteps) ? std::max(*previous, floor) : floor;
    // Sudah di batas bawah: jangan keluar dari mode Paskan tanpa perubahan apa pun
    if (target < zoom / 1.001) {
        m_canvas->zoomAtCenter(target);
    }
}

void DiagramViewer::fitToWindow() {
    m_canvas->fit();
}

void DiagramViewer::showActualSize() {
    m_canvas->zoomAtCenter(1.0);
}

void DiagramViewer::openExternally() {
    const QString path = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                             .filePath(QStringLiteral("lassomoir-diagram-%1.png").arg(m_key));
    if (!QFileInfo::exists(path)) {
        m_image.save(path, "PNG");
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void DiagramViewer::updateControls() {
    const qreal zoom = m_canvas->zoom();
    m_zoomLevel->setText(QStringLiteral("%1%").arg(qRound(zoom * 100)));
    m_zoomOut->setEnabled(zoom > m_canvas->minimumZoom() * 1.001);
    m_zoomIn->setEnabled(zoom < kMaxZoom / 1.001);
    m_fit->setChecked(m_canvas->fitMode());
    m_actualSize->setChecked(!m_canvas->fitMode() && qAbs(zoom - 1.0) < 0.001);
}

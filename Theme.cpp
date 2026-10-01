#include "Theme.h"

#include <QApplication>
#include <QFile>
#include <QHash>
#include <QIconEngine>
#include <QPainter>
#include <QPixmap>
#include <QRegularExpression>
#include <QStyleHints>
#include <QSvgRenderer>
#include <QWidget>
#include <QtDebug>

#include <memory>

namespace {

std::optional<Theme::Scheme> g_override;
QString g_uiFontFamily;

constexpr QRgb kRgbMask = 0xffffff;

// Palet gelap "blue night": latar navy lembut (bukan hitam), teks biru keabuan, aksen coklat-amber
// yang menyambung dengan merek, dan biru periwinkle untuk elemen navy.
//
// Warna teks: dipakai untuk properti `color`, garis ikon, dan teks yang dilukis sendiri
const QHash<QRgb, QRgb> &textMap() {
    static const QHash<QRgb, QRgb> map = {
        // Teks utama
        {0x3d3730, 0xdbe3ef}, {0x111827, 0xdbe3ef}, {0x1f2937, 0xdbe3ef}, {0x374151, 0xdbe3ef},
        {0x24201b, 0xdbe3ef},
        // Teks redup / nonaktif
        {0x8a7f70, 0x93a1b8}, {0x6f6557, 0x93a1b8}, {0xa39887, 0x8592a8},
        {0xb3aa9c, 0x6f7d94}, {0xb3a898, 0x6f7d94}, {0xb9b0a3, 0x6f7d94},
        // Navy -> periwinkle
        {0x33517a, 0x9dbbe6}, {0x2f4b73, 0x9dbbe6}, {0x1f3656, 0x9dbbe6},
        // Coklat -> amber
        {0xa9743f, 0xe0b07a}, {0x8f5f31, 0xe0b07a}, {0x8a5a2f, 0xe0b07a}, {0x7a5024, 0xe0b07a},
        {0x6a441f, 0xe0b07a}, {0x7a4a12, 0xe0b07a},
        // Status
        {0x9b2c1f, 0xf09a8c}, {0xb3261e, 0xf09a8c}, {0xb4442f, 0xf09a8c},
        {0xb7791f, 0xe8c27a}, {0x2f7d32, 0x7fd39e},
        // Jenis dokumen
        {0x1f7a4d, 0x6fd0a0}, {0x2b5797, 0x8db4ec},
    };
    return map;
}

// Warna bidang: latar, garis tepi, seleksi, isian yang dilukis sendiri
const QHash<QRgb, QRgb> &fillMap() {
    static const QHash<QRgb, QRgb> map = {
        // Permukaan: kartu, panel, input
        {0xffffff, 0x222d40},
        // Latar halaman
        {0xfbf8f3, 0x1b2433}, {0xfafbfb, 0x1b2433}, {0xf8f4ee, 0x1b2433},
        // Bilah samping & atas
        {0xf7f9f8, 0x1e2839},
        // Kolom stage & kanvas diagram
        {0xf1e8da, 0x263247}, {0xece5da, 0x263247},
        // Hover lembut
        {0xf8f1e6, 0x2a3650}, {0xf6f1e9, 0x2a3650},
        // Coklat muda (tag, hover, terpilih) -> amber redup
        {0xf2e3d1, 0x3d3529},
        // Navy muda
        {0xe3ebf6, 0x2a3954}, {0xeef2f8, 0x2a3954}, {0xdfe7f2, 0x2a3954},
        // Garis tepi
        {0xe7ddcd, 0x34435c}, {0xe2d5c1, 0x34435c}, {0xefe6d8, 0x34435c}, {0xefe7da, 0x34435c},
        {0xd9cdbb, 0x34435c}, {0xd3d8d5, 0x34435c}, {0xd1d5db, 0x34435c}, {0xe5e7eb, 0x34435c},
        {0xdfe5ee, 0x34435c},
        {0xdccbb4, 0x3e4f6b}, {0xc9d3e2, 0x3e4f6b}, {0xccd9ea, 0x3e4f6b}, {0xd9c6ac, 0x3e4f6b},
        {0xb9c6da, 0x4a5d7c},
        {0xb3aa9c, 0x56637a}, {0xb3a898, 0x56637a}, {0xa39887, 0x56637a}, {0xb9b0a3, 0x56637a},
        {0x8a7f70, 0x56637a}, {0x6f6557, 0x56637a},
        // Tombol coklat: primer tetap (teks putih di atasnya), hover/pressed menyesuaikan
        {0x8a5a2f, 0xbd8550}, {0x8f5f31, 0xbd8550},
        {0x7a5024, 0x8f6036}, {0x6a441f, 0x8f6036}, {0x7a4a12, 0x8f6036},
        // Tombol navy
        {0x33517a, 0x4a6ea3}, {0x2f4b73, 0x5a7fb5}, {0x1f3656, 0x3d5e8f},
        // Hover/pressed gelap (ikon putih di atasnya)
        {0x3d3730, 0x3b4d6b}, {0x24201b, 0x2f3f59},
        {0x111827, 0x3b4d6b}, {0x1f2937, 0x3b4d6b}, {0x374151, 0x3b4d6b},
        // Status: latar
        {0xfbeccf, 0x4a3d22}, {0xf6e0db, 0x4a2b28}, {0xffebe9, 0x4a2b28},
        {0xe3f0e5, 0x23402f}, {0xe6ffec, 0x23402f}, {0xefe9e1, 0x2c3546}, {0xe3ebe6, 0x2c4038},
        {0xabf2bc, 0x2f6b45}, {0xffc0bc, 0x7a3a35},
        // Status: garis
        {0xe2c4bd, 0x6b3a33}, {0x9cc9a0, 0x3f7a52},
        {0xb4442f, 0xc0614f}, {0x9b2c1f, 0xc0614f}, {0xb3261e, 0xc0614f},
        {0xb7791f, 0xc99a43}, {0x2f7d32, 0x4fa36c},
        // Jenis dokumen
        {0x1f7a4d, 0x2f8f5f}, {0x2b5797, 0x3f6fb5},
    };
    return map;
}

QColor mapped(QRgb light, const QHash<QRgb, QRgb> &map) {
    const QRgb rgb = light & kRgbMask;
    if (!Theme::isDark()) {
        return QColor(QRgb(0xff000000u | rgb));
    }
    return QColor(QRgb(0xff000000u | map.value(rgb, rgb)));
}

// Ganti setiap #rrggbb di teks lewat peta
QString replaceHex(const QString &text, const QHash<QRgb, QRgb> &map) {
    static const QRegularExpression hex(QStringLiteral("#([0-9a-fA-F]{6})\\b"));
    QString result;
    result.reserve(text.size());
    qsizetype last = 0;
    QRegularExpressionMatchIterator it = hex.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        const QRgb rgb = QRgb(match.captured(1).toUInt(nullptr, 16));
        result += text.mid(last, match.capturedStart() - last);
        result += QColor(QRgb(0xff000000u | map.value(rgb, rgb))).name();
        last = match.capturedEnd();
    }
    result += text.mid(last);
    return result;
}

QString readFile(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return QString();
    }
    return QString::fromUtf8(file.readAll());
}

// Ikon SVG yang garisnya ikut tema: warna garis dipetakan seperti warna teks, digambar ulang
// tiap kali dilukis sehingga pergantian mode langsung terlihat tanpa memasang ulang ikon
class ThemedSvgEngine final : public QIconEngine {
public:
    explicit ThemedSvgEngine(QString path) : m_path(std::move(path)) {}

    void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode, QIcon::State state) override {
        Q_UNUSED(state);
        QSvgRenderer &renderer = rendererFor(Theme::scheme());
        if (mode == QIcon::Disabled) {
            painter->save();
            painter->setOpacity(painter->opacity() * 0.4);
            renderer.render(painter, rect);
            painter->restore();
            return;
        }
        renderer.render(painter, rect);
    }

    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override {
        return scaledPixmap(size, mode, state, 1.0);
    }

    QPixmap scaledPixmap(const QSize &size, QIcon::Mode mode, QIcon::State state, qreal scale) override {
        const QSize device = size * scale;
        QPixmap pixmap(device);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        paint(&painter, QRect(QPoint(0, 0), device), mode, state);
        painter.end();
        pixmap.setDevicePixelRatio(scale);
        return pixmap;
    }

    QIconEngine *clone() const override { return new ThemedSvgEngine(m_path); }
    QString key() const override { return QStringLiteral("ThemedSvgEngine"); }

private:
    // SVG diurai sekali per mode, bukan tiap kali tombol dilukis ulang
    QSvgRenderer &rendererFor(Theme::Scheme scheme) {
        std::unique_ptr<QSvgRenderer> &cached = scheme == Theme::Scheme::Dark ? m_dark : m_light;
        if (!cached) {
            const QString light = readFile(m_path);
            cached = std::make_unique<QSvgRenderer>(
                (scheme == Theme::Scheme::Dark ? replaceHex(light, textMap()) : light).toUtf8());
        }
        return *cached;
    }

    QString m_path;
    std::unique_ptr<QSvgRenderer> m_light;
    std::unique_ptr<QSvgRenderer> m_dark;
};

// Stylesheet milik widget sendiri (dari .ui atau setStyleSheet di kode) juga ditulis untuk mode
// terang. Versi terangnya disimpan di property, lalu yang terpasang selalu versi sesuai scheme.
constexpr char kLightSheet[] = "_themeLightSheet";
constexpr char kThemedSheet[] = "_themeThemedSheet";

void themeWidgetSheet(QWidget *widget) {
    const QString current = widget->styleSheet();
    const QVariant themed = widget->property(kThemedSheet);
    // Stylesheet baru dari kode/.ui (bukan hasil pemetaan kita): itulah versi terangnya
    if (!themed.isValid() || themed.toString() != current) {
        if (current.isEmpty() && !themed.isValid()) {
            return;
        }
        widget->setProperty(kLightSheet, current);
    }
    const QString target = Theme::styleSheetFor(widget->property(kLightSheet).toString(), Theme::scheme());
    widget->setProperty(kThemedSheet, target);
    if (target != current) {
        widget->setStyleSheet(target);
    }
}

class WidgetSheetThemer final : public QObject {
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        // Polish: widget baru sebelum tampil pertama kali; StyleChange: setStyleSheet() dipanggil lagi
        if ((event->type() == QEvent::Polish || event->type() == QEvent::StyleChange) && watched->isWidgetType()) {
            themeWidgetSheet(static_cast<QWidget *>(watched));
        }
        return QObject::eventFilter(watched, event);
    }
};

}

Theme::Scheme Theme::scheme() {
    if (g_override) {
        return *g_override;
    }
    const QStyleHints *hints = QGuiApplication::styleHints();
    return hints && hints->colorScheme() == Qt::ColorScheme::Dark ? Scheme::Dark : Scheme::Light;
}

void Theme::setSchemeOverride(std::optional<Scheme> scheme) {
    g_override = scheme;
}

QColor Theme::text(QRgb light) {
    return mapped(light, textMap());
}

QColor Theme::fill(QRgb light) {
    return mapped(light, fillMap());
}

QString Theme::html(const QString &lightHtml) {
    return isDark() ? replaceHex(lightHtml, textMap()) : lightHtml;
}

QIcon Theme::icon(const QString &path) {
    return QIcon(new ThemedSvgEngine(path));
}

QString Theme::styleSheetFor(const QString &lightStyleSheet, Scheme scheme) {
    if (scheme == Scheme::Light) {
        return lightStyleSheet;
    }
    // Stylesheet widget yang sama dipetakan berulang (tiap polish / setStyleSheet); hasilnya disimpan.
    // Hanya dipanggil dari thread GUI.
    static QHash<QString, QString> cache;
    if (const auto it = cache.constFind(lightStyleSheet); it != cache.constEnd()) {
        return *it;
    }
    // Tiap deklarasi "properti: nilai": warna di `color` memakai peta teks, sisanya peta bidang.
    // Selektor seperti "QPushButton:hover" ikut tertangkap pola ini, tapi tidak berisi warna.
    static const QRegularExpression declaration(QStringLiteral("([A-Za-z-]+)(\\s*:\\s*)([^;{}]*)"));
    QString result;
    result.reserve(lightStyleSheet.size());
    qsizetype last = 0;
    QRegularExpressionMatchIterator it = declaration.globalMatch(lightStyleSheet);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        const QString property = match.captured(1).toLower();
        const bool isText = property == QLatin1String("color") || property == QLatin1String("selection-color");
        result += lightStyleSheet.mid(last, match.capturedStart() - last);
        result += match.captured(1) + match.captured(2) + replaceHex(match.captured(3), isText ? textMap() : fillMap());
        last = match.capturedEnd();
    }
    result += lightStyleSheet.mid(last);
    cache.insert(lightStyleSheet, result);
    return result;
}

QPalette Theme::palette() {
    // Bagian yang tidak diatur styles.qss (viewport scroll area, handle splitter, dialog bawaan)
    // memakai palet ini. Tanpa ini Qt memakai palet sesuai mode Windows: area hitam di tema terang,
    // atau abu-abu netral yang tidak senada di tema gelap.
    QPalette palette;
    const auto set = [&palette](QPalette::ColorRole role, const QColor &color) {
        palette.setColor(QPalette::All, role, color);
    };
    set(QPalette::Window, fill(0xfbf8f3));
    set(QPalette::WindowText, text(0x3d3730));
    set(QPalette::Base, fill(0xffffff));
    set(QPalette::AlternateBase, fill(0xf8f1e6));
    set(QPalette::Text, text(0x3d3730));
    set(QPalette::PlaceholderText, text(0x8a7f70));
    set(QPalette::Button, fill(0xffffff));
    set(QPalette::ButtonText, text(0x3d3730));
    set(QPalette::BrightText, text(0xffffff));
    set(QPalette::ToolTipBase, fill(0xffffff));
    set(QPalette::ToolTipText, text(0x3d3730));
    set(QPalette::Highlight, fill(0xa9743f));
    set(QPalette::HighlightedText, text(0xffffff));
    set(QPalette::Link, text(0x33517a));
    set(QPalette::Light, fill(0xffffff));
    set(QPalette::Midlight, fill(0xefe6d8));
    set(QPalette::Mid, fill(0xe2d5c1));
    set(QPalette::Dark, fill(0xd9c6ac));
    set(QPalette::Shadow, isDark() ? QColor(0x10, 0x16, 0x20) : QColor(0xb3, 0xaa, 0x9c));
    palette.setColor(QPalette::Disabled, QPalette::WindowText, text(0xb3aa9c));
    palette.setColor(QPalette::Disabled, QPalette::Text, text(0xb3aa9c));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, text(0xb3aa9c));
    return palette;
}

bool Theme::apply(QApplication &app) {
    const QString light = readFile(QStringLiteral(":/styles.qss"));
    if (light.isEmpty()) {
        qWarning() << "Peringatan: Gagal memuat :/styles.qss. Tampilan akan menggunakan default Qt.";
        return false;
    }
    QString sheet = styleSheetFor(light, scheme());
    if (scheme() == Scheme::Dark) {
        // Tambahan khusus gelap yang tidak bisa diturunkan dari peta warna (mis. gambar panah)
        sheet += QLatin1Char('\n') + readFile(QStringLiteral(":/styles-dark.qss"));
    }
    if (!g_uiFontFamily.isEmpty()) {
        // Menimpa aturan "*" di awal styles.qss (sama spesifiknya, ditulis lebih akhir). Selector ID
        // monospace (log, diff) lebih spesifik, jadi tetap.
        QString family = g_uiFontFamily;
        family.remove(QLatin1Char('"'));
        sheet += QStringLiteral("\n* {\n    font-family: \"%1\";\n}\n").arg(family);
    }
    QApplication::setPalette(palette());
    app.setStyleSheet(sheet);
    const QWidgetList widgets = QApplication::allWidgets();
    for (QWidget *widget : widgets) {
        themeWidgetSheet(widget);
    }
    emit Notifier::instance()->changed();
    return true;
}

void Theme::setUiFontFamily(const QString &family) {
    g_uiFontFamily = family;
}

QString Theme::uiFontFamily() {
    return g_uiFontFamily;
}

bool Theme::install(QApplication &app) {
    app.installEventFilter(new WidgetSheetThemer(&app));
    QObject::connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, &app,
                     [&app]() { apply(app); });
    return apply(app);
}

Theme::Notifier *Theme::Notifier::instance() {
    static Notifier notifier;
    return &notifier;
}

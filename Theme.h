#ifndef THEME_H
#define THEME_H

#pragma once

#include <QColor>
#include <QIcon>
#include <QObject>
#include <QPalette>
#include <QString>

#include <optional>

class QApplication;

// Tema aplikasi mengikuti mode terang/gelap Windows.
//
// styles.qss ditulis untuk mode terang (krem-coklat). Mode gelap ("blue night") tidak punya file
// salinan: tiap warna hex di stylesheet dipetakan ke pasangan gelapnya, dan petanya dibedakan
// antara warna teks (properti `color`) dan warna bidang (latar, garis, seleksi). Dengan begitu
// putih sebagai latar kartu menjadi navy, sedangkan putih sebagai teks di atas tombol tetap putih.
// Widget yang melukis sendiri memakai text() / fill() dengan warna terangnya.
namespace Theme {

enum class Scheme { Light, Dark };

// Mode yang berlaku: paksaan test bila ada, selain itu mode sistem
Scheme scheme();
inline bool isDark() { return scheme() == Scheme::Dark; }

// Hanya untuk test (platform offscreen tidak punya mode sistem); nullopt = ikut sistem lagi
void setSchemeOverride(std::optional<Scheme> scheme);

// Warna terang -> warna yang berlaku sekarang, sebagai teks atau sebagai bidang
QColor text(QRgb light);
QColor fill(QRgb light);

// Teks kaya dengan warna inline mode terang -> warnanya dipetakan seperti text()
QString html(const QString &lightHtml);

// Ikon SVG yang warna garisnya ikut tema (dipetakan seperti text()), diganti saat dilukis
QIcon icon(const QString &path);

// Palet aplikasi untuk scheme() sekarang (dipasang apply), untuk widget yang tidak diatur stylesheet
QPalette palette();

// Stylesheet terang -> stylesheet untuk scheme
QString styleSheetFor(const QString &lightStyleSheet, Scheme scheme);

// Font antarmuka pilihan pengguna (lihat AppFonts); kosong = font bawaan styles.qss.
// Berlaku pada apply() berikutnya. Font monospace (log, diff) tidak ikut berganti.
void setUiFontFamily(const QString &family);
QString uiFontFamily();

// Muat :/styles.qss (+ :/styles-dark.qss di mode gelap) ke aplikasi, dan pasang ulang setiap
// kali mode sistem berubah. Kembalikan false bila stylesheet tidak terbaca.
bool install(QApplication &app);
// Pasang ulang stylesheet sesuai scheme() sekarang (dipanggil install dan saat mode berubah)
bool apply(QApplication &app);

// Pemancar sinyal perubahan tema, untuk widget yang menyimpan warna sendiri
class Notifier : public QObject {
    Q_OBJECT
public:
    static Notifier *instance();
signals:
    void changed();
};

}

#endif // THEME_H

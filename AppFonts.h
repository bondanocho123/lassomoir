#ifndef APPFONTS_H
#define APPFONTS_H

#pragma once

#include <QString>
#include <QStringList>

// Font open-source yang ditanam di resource (:/fonts/*.ttf, Google Fonts, lisensi OFL/Apache)
// dan pilihan font antarmuka pengguna. Pilihan kosong = font bawaan styles.qss (Garamond).
namespace AppFonts {

// Daftarkan semua font tertanam ke QFontDatabase (sekali saja); kembalikan nama keluarganya, urut abjad
QStringList registerBundled();

// Nama yang ditampilkan untuk pilihan bawaan
QString defaultLabel();

// Pilihan tersimpan di <AppConfigLocation>/settings.ini; kosong = bawaan
QString saved();
void save(const QString &family);

}

#endif // APPFONTS_H

#ifndef STAGEINFO_H
#define STAGEINFO_H

#pragma once

#include <QString>

class StageCatalog;

// Penjelasan satu stage untuk popup info di judul kolomnya: tugas stage itu, lalu fitur-fiturnya.
// Peran dan tugas ditulis tangan per key stage. Fitur (tool, model, paralelisme, gate, ke mana task
// lanjut) dibaca dari profil di katalog, jadi penjelasannya ikut berubah bila konfigurasi stage diubah.
namespace StageInfo {

// Teks kaya (HTML) untuk HoverInfoPopup; kosong bila key tidak ada di katalog
QString html(const StageCatalog &catalog, const QString &stageKey);

}

#endif // STAGEINFO_H

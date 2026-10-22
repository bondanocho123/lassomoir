#ifndef PROJECTGUIDELINES_H
#define PROJECTGUIDELINES_H

#pragma once

#include "TaskMaterials.h"

#include <QString>
#include <QStringList>

// Pedoman project: dokumen batasan dan guideline (framework, arsitektur, gaya kode) yang berlaku
// untuk semua task di satu project. Salinannya ada di folder pedoman project,
// <AppData>/projects/<project>/guidelines/ (lihat FileManager::guidelineDirectory); isi folder itu
// adalah daftarnya, urut nama.
// - .md / .txt / .docx / .xlsx / .csv: isi teksnya ikut prompt setiap run, di bagian
//   "# Pedoman project" (lihat TaskPromptComposer);
// - .pdf: tidak diubah ke teks; prompt menyebut path-nya dan agent membacanya sendiri dengan Read.
namespace ProjectGuidelines {

// Isi teks pedoman di prompt, per dokumen dan total satu project. Yang lebih panjang dipotong;
// teks lengkapnya ditulis ke .teks/<nama>.txt di folder pedoman supaya bisa dibaca dengan Read.
constexpr qsizetype kDocumentPromptChars = 30000;
constexpr qsizetype kTotalPromptChars = 60000;
constexpr qint64 kMaxFileBytes = 50 * 1024 * 1024;

bool isPdf(const QString &fileName);
bool isSupported(const QString &fileName);   // DocumentText::supportedSuffixes() + pdf
QString fileFilter();                        // untuk QFileDialog

// Nama file pedoman di folder, urut nama tanpa beda huruf besar/kecil. Folder yang tidak ada = kosong.
QStringList list(const QString &directory);

// Salin file ke folder pedoman. Pedoman dengan nama yang sama diganti (cara memperbarui pedoman).
// Nama yang tersimpan, atau kosong + *error (kalimat untuk pengguna).
QString add(const QString &directory, const QString &sourcePath, QString *error = nullptr);

// Hapus salinan satu pedoman (file aslinya tidak disentuh). File yang sudah tidak ada = sukses.
bool remove(const QString &directory, const QString &fileName, QString *error = nullptr);

// Baca pedoman di folder ke materials (guidelines, guidelineDirectory, warnings). Membaca disk;
// panggil tepat sebelum run.
void addTo(TaskMaterials *materials, const QString &directory);

}

#endif // PROJECTGUIDELINES_H

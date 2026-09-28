#ifndef DOCUMENTTEXT_H
#define DOCUMENTTEXT_H

#pragma once

#include <QString>
#include <QStringList>

// Isi teks dokumen lampiran untuk prompt agent. Tool Read milik agent tidak bisa membuka
// .xlsx/.docx, jadi isinya diubah ke teks di sini:
// - .csv / .tsv: teks apa adanya (UTF-8, atau code page Windows bila bukan UTF-8 yang sah);
// - .xlsx / .xlsm: tiap sheet jadi baris CSV di bawah penanda "[Sheet: <nama>]", sel bertanggal
//   ditulis ISO (2026-09-28), rumus diwakili hasil terakhirnya;
// - .docx: paragraf per baris, judul diawali '#', butir daftar "- ", tabel jadi baris "| a | b |".
// Format biner lama (.xls, .doc) dan file Office berpassword tidak didukung.
namespace DocumentText {

// Ekstensi yang didukung, huruf kecil tanpa titik
const QStringList &supportedSuffixes();
bool isSupported(const QString &fileName);

// Pemeriksaan cepat saat file dilampirkan: ekstensinya didukung, filenya terbaca, dan arsip
// Office-nya punya bagian utama. false + *error (kalimat untuk pengguna) bila tidak.
bool check(const QString &path, QString *error = nullptr);

// Seluruh isi teks dokumen; kosong + *error bila tidak bisa dibaca. Dokumen raksasa dipotong
// di sekitar dua juta karakter dengan catatan di akhir.
QString extract(const QString &path, QString *error = nullptr);

}

#endif // DOCUMENTTEXT_H

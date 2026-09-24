#ifndef CODEMETRICS_H
#define CODEMETRICS_H

#pragma once

#include "CSharpMetrics.h"

#include <QList>
#include <QString>

// Maintainability Index satu file kode sumber beserta bahan hitungnya. Dihitung dari token
// (perkiraan lintas bahasa, bukan parser penuh), jadi paling berguna untuk membandingkan
// sebelum dan sesudah perubahan.
struct CodeMetrics {
    int sloc = 0;             // baris berisi kode; baris kosong dan komentar tidak dihitung
    int functions = 0;        // fungsi/method/lambda yang punya badan
    int complexity = 0;       // kompleksitas siklomatik: 1 per fungsi + tiap titik keputusan
    double volume = 0.0;      // volume Halstead: panjang · log2(kosakata)
    int maintainability = 0;  // 0–100 (skala Visual Studio), dari rata-rata per fungsi
    QList<TypeMetrics> types; // C#: rincian per tipe dan member ala Code Metrics Visual Studio

    // Ekstensi file termasuk bahasa yang bisa diukur
    static bool supports(const QString &path);

    // path hanya dipakai untuk memilih aturan bahasa
    static CodeMetrics measure(const QString &path, const QString &source);

    // Rumus Visual Studio dengan rata-rata per fungsi:
    // 171 − 5,2·ln(volume) − 0,23·kompleksitas − 16,2·ln(baris), dinormalisasi ke 0–100
    static int maintainabilityIndex(double volume, int complexity, int sloc, int functions);
};

// Ambang Visual Studio: ≥ 20 baik, 10–19 sedang, < 10 rendah
enum class MaintainabilityRating { Good, Moderate, Low };
MaintainabilityRating maintainabilityRating(int index);

#endif // CODEMETRICS_H

#ifndef SOURCETOKENS_H
#define SOURCETOKENS_H

#pragma once

#include <QList>
#include <QString>
#include <QStringView>

#include <optional>
#include <utility>

// Pemecah kode sumber jadi token untuk perhitungan metrik (CodeMetrics, CSharpMetrics).
// Lintas bahasa dan sengaja sederhana: komentar dibuang, string dan direktif jadi satu token.

enum class SourceSyntax { CFamily, Python };

struct SourceRules {
    SourceSyntax syntax = SourceSyntax::CFamily;
    bool hashComments = false;   // '#' memulai komentar (Python, PHP)
    bool preprocessor = false;   // baris berawalan '#' adalah direktif (C/C++, C#, Swift)
    bool phpTags = false;        // hanya isi <?php ... ?> yang kode; di luarnya HTML
    bool ternary = true;         // '?' adalah percabangan, bukan penanda tipe nullable
    bool lifetimes = false;      // Rust: 'a adalah lifetime, bukan literal karakter
    bool csharp = false;         // string verbatim @"..", interpolasi $"..{..}..", raw """.."""
};

struct SourceToken {
    enum class Kind { Word, Number, Text, Symbol, Directive };

    Kind kind = Kind::Symbol;
    QString text;
    int line = 0;       // baris awal, mulai dari 0
    int endLine = 0;    // baris akhir; berbeda dari line pada string/direktif multi-baris

    bool is(QStringView symbol) const { return kind == Kind::Symbol && text == symbol; }
    bool isWord(QStringView word) const { return kind == Kind::Word && text == word; }
};

// Rentang token [first, second)
using TokenRange = std::pair<qsizetype, qsizetype>;

namespace SourceTokens {

// Aturan bahasa dari ekstensi file; kosong = bukan kode sumber yang didukung
std::optional<SourceRules> rulesFor(const QString &path);

QList<SourceToken> tokenize(const QString &source, const SourceRules &rules);

// Jumlah baris berbeda yang memuat token dalam rentang-rentang (urut, tidak tumpang tindih)
int codeLines(const QList<SourceToken> &tokens, const QList<TokenRange> &ranges);

// Volume Halstead: panjang · log2(kosakata). Pasangan kurung dihitung sekali lewat kurung bukanya.
double halsteadVolume(const QList<SourceToken> &tokens, const QList<TokenRange> &ranges);

}

#endif // SOURCETOKENS_H

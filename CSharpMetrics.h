#ifndef CSHARPMETRICS_H
#define CSHARPMETRICS_H

#pragma once

#include "SourceTokens.h"

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

// Metrik satu member C# yang punya kode (method, constructor, property/indexer/event dengan
// accessor berbadan, operator), seperti baris member di Code Metrics Visual Studio
struct MemberMetrics {
    QString name;              // "Place(Customer, int)", "Total", "this[int]", "operator +(Money, Money)"
    int maintainability = 0;   // 0–100
    int complexity = 0;        // 1 + titik keputusan
    int coupling = 0;          // jumlah tipe berbeda yang dipakai
    int lines = 0;             // baris kode, tanpa baris kosong dan komentar
    double volume = 0.0;       // volume Halstead
};

// Satu anggota tipe untuk diagram kelas UML (semua anggota, termasuk field dan auto-property)
struct TypeMember {
    enum class Kind { Field, Property, Event, Constructor, Method, EnumValue };

    Kind kind = Kind::Field;
    QString name;              // "Place", "_repository", "this" (indexer)
    QString type;              // tipe field/property/event, atau tipe kembalian method
    QString parameters;        // "Customer, int" untuk method/constructor/indexer
    QChar visibility;          // '+' public, '-' private, '#' protected, '~' internal
    bool isStatic = false;
    bool isAbstract = false;
};

// Metrik satu tipe C#. Tipe bersarang punya entri sendiri ("Luar.Dalam").
struct TypeMetrics {
    QString namespaceName;         // "Shop.Orders"; kosong = namespace global
    QString name;                  // di dalam namespace: "OrderService", "OrderService.Snapshot"
    QString kind;                  // "class", "struct", "record", "record struct", "interface", "enum"
    QString baseClass;             // kelas dasar langsung (bukan interface); kosong = System.Object
    QStringList interfaces;        // interface yang diimplementasikan / diturunkan
    QStringList genericParameters; // "T", "TKey"
    bool isAbstract = false;
    bool isStatic = false;
    QList<TypeMember> outline;     // semua anggota, urut sumber
    int maintainability = 100;     // dari rata-rata member berkode; 100 bila tidak ada
    int complexity = 0;            // jumlah kompleksitas member
    int coupling = 0;
    int lines = 0;                 // baris kode seluruh deklarasi tipe
    double volume = 0.0;           // volume Halstead gabungan member berkode
    int inheritanceDepth = 0;      // DIT: jumlah kelas leluhur sampai System.Object
    bool inheritanceOpen = false;  // ada leluhur dari luar project yang tidak dikenal: DIT minimal
    QList<MemberMetrics> members;

    QString fullName() const { return namespaceName.isEmpty() ? name : namespaceName + QLatin1Char('.') + name; }
};

namespace CSharpMetrics {

// Tipe dan member dari token satu file C# (aturan SourceTokens::rulesFor("*.cs"))
QList<TypeMetrics> analyze(const QList<SourceToken> &tokens);

// Nama tipe (tanpa namespace) → nama kelas dasarnya, dari teks sumber; untuk DIT lintas file dan
// tipe konteks di diagram kelas. Enum, struct, dan interface tercatat dengan kelas dasar kosong.
QHash<QString, QString> declaredBases(const QString &source);

// Isi inheritanceDepth/inheritanceOpen memakai peta kelas dasar seluruh project
void resolveInheritance(QList<TypeMetrics> *types, const QHash<QString, QString> &projectBases);

}

#endif // CSHARPMETRICS_H

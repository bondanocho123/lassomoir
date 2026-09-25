#ifndef CLASSDIAGRAM_H
#define CLASSDIAGRAM_H

#pragma once

#include <QString>

struct WorkspaceDiff;

// Diagram kelas UML (Mermaid classDiagram) dari tipe C# di file yang berubah, beserta tipe
// terkaitnya: kelas dasar, interface project, dan tipe project yang dipakai field/parameter
struct ClassDiagram {
    QString code;           // kode Mermaid; kosong bila perubahan tidak memuat tipe C#
    int drawnTypes = 0;     // tipe dari file yang berubah yang digambar lengkap
    int omittedTypes = 0;   // tipe yang tidak digambar karena batas ukuran diagram

    static ClassDiagram fromDiff(const WorkspaceDiff &diff);
};

#endif // CLASSDIAGRAM_H

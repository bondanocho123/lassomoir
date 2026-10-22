#ifndef TASKMATERIALS_H
#define TASKMATERIALS_H

#pragma once

#include <QList>
#include <QString>
#include <QStringList>

// Bahan run di luar TaskItem: lampiran task yang sudah dibaca dari disk, pedoman project, dan folder
// referensi project. Dibaca TaskAttachments::materials() tepat sebelum run; PromptComposer merakitnya
// ke prompt dan SwarmCoordinator meneruskan foto & izin baca foldernya ke AgentLaunch.
struct TaskMaterials {
    struct Document {
        QString fileName;
        QString path;                 // file asli di folder lampiran task
        QString text;                 // isi teks untuk prompt, bisa sudah dipotong
        qsizetype omittedChars = 0;   // panjang teks yang tidak ikut prompt
        QString fullTextPath;         // salinan teks lengkap bila dipotong, untuk dibaca dengan Read
        QString error;                // alasan tidak terbaca; text kosong
        bool readByAgent = false;     // PDF: isinya tidak ikut prompt, agent membacanya sendiri dengan Read
    };

    QString attachmentDirectory;        // folder lampiran task; kosong bila task tanpa lampiran
    QStringList imagePaths;             // foto, ikut sebagai blok gambar
    QList<Document> documents;
    QStringList referenceDirectories;   // folder referensi project yang masih ada
    // Pedoman project (batasan & guideline framework) untuk semua task di project itu, dibaca
    // ProjectGuidelines::addTo(); guidelineDirectory kosong bila project tanpa pedoman
    QString guidelineDirectory;
    QList<Document> guidelines;
    QStringList warnings;               // lampiran atau folder yang dilewati, untuk konsol

    // Folder di luar folder kerja yang boleh dibaca agent (izin Read, bukan Edit/Write)
    QStringList readableDirectories() const {
        QStringList directories = referenceDirectories;
        if (!guidelineDirectory.isEmpty()) {
            directories.append(guidelineDirectory);
        }
        if (!attachmentDirectory.isEmpty()) {
            directories.append(attachmentDirectory);
        }
        return directories;
    }
};

#endif // TASKMATERIALS_H

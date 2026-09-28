#ifndef PROMPTCOMPOSER_H
#define PROMPTCOMPOSER_H

#pragma once

#include "TaskMaterials.h"

#include <QString>

struct TaskItem;

// Perakit isi prompt task yang dikirim ke agent lewat stdin.
// Instruksi peran stage tidak di sini: itu bagian AgentDefinition (--append-system-prompt).
class PromptComposer {
public:
    virtual ~PromptComposer() = default;
    // materials: lampiran dan folder referensi yang sudah dibaca (TaskAttachments::materials)
    virtual QString compose(const TaskItem &task, const TaskMaterials &materials) const = 0;
    QString compose(const TaskItem &task) const { return compose(task, TaskMaterials()); }
};

// Prompt serah-terima antar stage, dirakit dari task.runs:
// - "# Task": judul / kategori / catatan satu baris (baris kosong dilewati);
// - "# Instruksi": catatan multi-baris dari kotak prompt form task;
// - "# Lampiran": nama foto (fotonya sendiri ikut sebagai blok gambar) dan isi teks dokumen;
// - "# Folder referensi (hanya dibaca)": folder di luar folder kerja yang boleh dibaca agent;
// - "# Spesifikasi yang disetujui (<STAGE>)": dokumen gate terakhir yang disetujui + catatannya;
// - "# Hasil stage sebelumnya (<STAGE>)": run terakhir dari stage lain (mis. hasil CODER untuk
//   CLEANER, atau laporan QA saat task dikembalikan ke CODER);
// - "# Dokumen sebelumnya (untuk direvisi)": bila keputusan terakhir di stage ini adalah revisi.
class TaskPromptComposer final : public PromptComposer {
public:
    using PromptComposer::compose;
    QString compose(const TaskItem &task, const TaskMaterials &materials) const override;
};

#endif // PROMPTCOMPOSER_H

#ifndef PROMPTCOMPOSER_H
#define PROMPTCOMPOSER_H

#pragma once

#include <QString>

struct TaskItem;

// Perakit isi prompt task yang dikirim ke agent lewat stdin.
// Instruksi peran stage tidak di sini: itu bagian AgentDefinition (--append-system-prompt).
class PromptComposer {
public:
    virtual ~PromptComposer() = default;
    virtual QString compose(const TaskItem &task) const = 0;
};

// Prompt serah-terima antar stage, dirakit dari task.runs:
// - "# Task": judul / kategori / catatan (baris kosong dilewati);
// - "# Spesifikasi yang disetujui (<STAGE>)": dokumen gate terakhir yang disetujui + catatannya;
// - "# Hasil stage sebelumnya (<STAGE>)": run terakhir dari stage lain (mis. hasil CODER untuk
//   CLEANER, atau laporan QA saat task dikembalikan ke CODER);
// - "# Dokumen sebelumnya (untuk direvisi)": bila keputusan terakhir di stage ini adalah revisi.
class TaskPromptComposer final : public PromptComposer {
public:
    QString compose(const TaskItem &task) const override;
};

#endif // PROMPTCOMPOSER_H

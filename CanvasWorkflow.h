#pragma once

#include "AgentDefinition.h"
#include "CanvasBoard.h"
#include "TaskItem.h"

#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

// Aturan alur kerja kanvas brainstorm, tanpa UI dan tanpa proses: isi kartu referensi dari task
// asalnya, prompt langkah AI, ringkasan untuk task baru, dan usulan task dari jawaban agent.
namespace CanvasWorkflow {

// Bahan yang ikut prompt langkah AI: per kartu dan total satu langkah
constexpr qsizetype kInputChars = 24000;
constexpr qsizetype kInputsChars = 90000;
// Salinan isi referensi yang ikut tersimpan di canvas.json
constexpr qsizetype kSnapshotChars = 60000;
constexpr int kMaxProposals = 12;

// Agent langkah AI: hanya membaca (Read, Grep, Glob), instruksi peran :/prompts/BRAINSTORM.md.
// model/effort kosong = bawaan (sonnet, medium).
AgentDefinition brainstormAgent(const QString &model = QString(), const QString &effort = QString());
// Agent chat tanya jawab kanvas: sama baca-sajanya, peran :/prompts/BRAINSTORM_CHAT.md
AgentDefinition chatAgent(const QString &model = QString());

// "SPECIFIER" -> "Spesifikasi"; stage tanpa label khusus -> "Hasil <STAGE>"
QString documentLabel(const QString &stage);

// Run yang mewakili dokumen satu stage: run sukses terakhir yang berisi jawaban, atau run terakhir
// stage itu bila belum ada; nullptr bila stage itu belum pernah dijalankan
const StageRun *documentRun(const TaskItem &task, const QString &stage);

// Artefak satu task untuk pustaka kanvas: dokumen tiap stage yang pernah dijalankan (urut
// stageOrder), lalu lampirannya
struct Artifact {
    CanvasSource source;
    QString label;     // "Spesifikasi", "mockup.png"
    QString detail;    // "SPECIFIER · disetujui", "foto"
};
QList<Artifact> artifactsOf(const TaskItem &task, const QStringList &stageOrder);

// Isi terbaru sumber kartu referensi. task kosong = sumbernya sudah tidak ada (available false;
// pemanggil mempertahankan salinan lamanya).
struct Reference {
    bool available = false;
    QString title;
    QString detail;
    QString text;
};
Reference resolve(const CanvasSource &source, const std::optional<TaskItem> &task,
                  const QString &attachmentDirectory);

// Kartu artefak berupa foto lampiran: ikut ke agent sebagai blok gambar, bukan teks
bool isImageReference(const CanvasNode &node);

// Baris berisi pertama dari teks Markdown, tanpa tanda judul/kutipan/daftar dan tanpa **
QString firstLine(const QString &text);

// Kosong bila langkah AI bisa dijalankan; selain itu alasannya untuk pengguna
QString stepProblem(const CanvasBoard &board, const QString &stepId);

// Isi prompt langkah AI (stdin agent): instruksi, bahan dari kartu yang tersambung, bentuk jawaban
QString stepPrompt(const CanvasBoard &board, const QString &stepId);

// Judul dan prompt task baru dari kartu-kartu kanvas beserta bahan yang tersambung ke sana
QString suggestedTitle(const CanvasNode &node);
QString taskBrief(const CanvasBoard &board, const QStringList &nodeIds);

// Nama pendek kartu untuk daftar bahan chat: judul referensi, atau baris pertama catatan/instruksi
QString cardName(const CanvasNode &node);
// Kartu bahan satu pertanyaan chat: contextIds yang masih ada, atau semua kartu bila contextIds
// kosong; urut posisi kartu (atas ke bawah, lalu kiri ke kanan)
QStringList chatContext(const CanvasBoard &board, const QStringList &contextIds);
// Ringkasan bahan untuk pengguna: "seluruh kanvas · 13 kartu" / "2 kartu: Spesifikasi · Login, Laporan QA"
QString chatContextLabel(const CanvasBoard &board, const QStringList &contextIds);
// Prompt satu pertanyaan chat: bahan dari kartu, percakapan sebelumnya (pesan terbaru didahulukan
// sampai batasnya), lalu pertanyaannya
struct ChatTurn {
    bool fromUser = true;
    QString text;
};
constexpr qsizetype kChatHistoryChars = 20000;
constexpr qsizetype kChatTurnChars = 4000;
constexpr int kChatHistoryTurns = 16;
QString chatPrompt(const CanvasBoard &board, const QStringList &contextIds, const QList<ChatTurn> &history,
                   const QString &question);

// Usulan task dari jawaban langkah AI berkeluaran "tasks": blok ```json terakhir yang berisi array
// objek {title, category, instructions} (atau {"tasks": [...]}). Kosong + *error bila tidak ada.
struct TaskProposal {
    QString title;
    QString category;
    QString instructions;
};
QList<TaskProposal> parseTaskProposals(const QString &answer, QString *error = nullptr);

}

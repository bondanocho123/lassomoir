#pragma once

#include "AgentTypes.h"

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QPointF>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QStringList>

#include <optional>

// Jenis kartu di kanvas brainstorm
enum class CanvasNodeKind { Note, Artifact, Task, Step };

QString canvasNodeKindKey(CanvasNodeKind kind);   // "note" | "artifact" | "task" | "step"
std::optional<CanvasNodeKind> canvasNodeKindFromKey(const QString &key);

// Keluaran langkah AI: dokumen Markdown, atau daftar usulan task untuk pipeline
enum class CanvasStepOutput { Document, Tasks };

// Asal kartu referensi: task di project mana pun. Kartu artefak menunjuk dokumen hasil run terakhir
// satu stage task itu, atau satu lampirannya.
struct CanvasSource {
    QString projectId;
    QString taskId;
    QString stage;
    QString attachment;

    bool isValid() const { return !projectId.isEmpty() && !taskId.isEmpty(); }
    bool isArtifact() const { return !stage.isEmpty() || !attachment.isEmpty(); }
    bool operator==(const CanvasSource &other) const {
        return projectId == other.projectId && taskId == other.taskId && stage == other.stage
               && attachment == other.attachment;
    }

    QJsonObject toJson() const;
    static CanvasSource fromJson(const QJsonObject &object);
};

struct CanvasNode {
    QString id;
    CanvasNodeKind kind = CanvasNodeKind::Note;
    QPointF pos;                   // pojok kiri atas, koordinat kanvas
    QSizeF size;

    // Catatan: isinya. Langkah AI: instruksinya. Referensi: salinan isi sumber yang terakhir terbaca,
    // tetap dipakai bila sumbernya sudah tidak ada (task dihapus, project ditutup).
    QString text;
    QString color;                 // Catatan: kunci warna kertas, lihat CanvasPalette

    CanvasSource source;           // Referensi (Artifact / Task)
    QString title;                 // Referensi: judul salinan terakhir
    QString detail;                // Referensi: keterangan singkat, mis. "CODER · review"
    bool available = true;         // Referensi: sumbernya masih ada; tidak disimpan ke disk

    QString model;                 // Langkah AI: kosong = bawaan langkah AI
    QString effort;
    CanvasStepOutput output = CanvasStepOutput::Document;
    AgentResult result;            // Langkah AI: hasil run terakhir; outcome kosong = belum pernah jalan
    QDateTime finishedAt;

    bool isReference() const { return kind == CanvasNodeKind::Artifact || kind == CanvasNodeKind::Task; }
    bool hasResult() const { return !result.outcome.isEmpty(); }
    QRectF rect() const { return QRectF(pos, size); }

    static QSizeF defaultSize(CanvasNodeKind kind);
    static QSizeF minimumSize(CanvasNodeKind kind);
};

// Garis berarah: isi kartu `from` menjadi bahan kartu `to` (dipakai langkah AI dan task baru)
struct CanvasEdge {
    QString id;
    QString from;
    QString to;
};

// Seluruh isi kanvas satu project. Grafnya selalu tanpa putaran (dijaga CanvasModel dan fromJson),
// jadi langkah AI selalu bisa diurutkan.
struct CanvasBoard {
    static constexpr int kSchemaVersion = 1;

    QList<CanvasNode> nodes;       // urutan gambar: yang terakhir paling atas
    QList<CanvasEdge> edges;
    QPointF viewCenter;            // tampilan terakhir, dipulihkan saat kanvas dibuka lagi
    qreal zoom = 1.0;

    qsizetype indexOf(const QString &nodeId) const;
    const CanvasNode *node(const QString &nodeId) const;
    CanvasNode *node(const QString &nodeId);
    const CanvasEdge *edge(const QString &edgeId) const;
    bool hasEdge(const QString &from, const QString &to) const;

    QStringList inputsOf(const QString &nodeId) const;    // urut sesuai garisnya dibuat
    QStringList outputsOf(const QString &nodeId) const;
    bool reaches(const QString &from, const QString &to) const;   // ada jalur from -> to (from == to: ya)

    QStringList stepIds() const;
    // Langkah AI yang hasilnya langsung menjadi bahan langkah ini
    QStringList upstreamSteps(const QString &stepId) const;
    // Langkah AI diurutkan supaya tiap langkah jalan sesudah langkah hulunya; seri dipecah posisi
    // kartu (atas ke bawah, lalu kiri ke kanan)
    QStringList stepOrder(const QStringList &stepIds) const;
    // stepId beserta semua langkah hulunya (transitif), urut dependensi
    QStringList withUpstream(const QString &stepId) const;

    QRectF bounds() const;         // gabungan semua kartu; kosong bila belum ada kartu
    // Pojok kiri atas terdekat dari want.topLeft() untuk kartu seukuran want yang tidak menimpa kartu
    // lain (berjarak gap). Untuk kartu baru yang posisinya tidak dipilih pengguna.
    QPointF openSpot(const QRectF &want, qreal gap = 24.0) const;

    QJsonObject toJson() const;
    // nullopt + *error bila bukan kanvas yang dikenal (schemaVersion lain). Kartu berjenis tidak
    // dikenal, id ganda, serta garis yang menunjuk kartu yang tidak ada atau membuat putaran dilewati
    // dan dicatat di *warnings.
    static std::optional<CanvasBoard> fromJson(const QJsonObject &root, QString *error,
                                               QStringList *warnings = nullptr);
};

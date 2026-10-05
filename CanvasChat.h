#pragma once

#include "AgentTypes.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>

class AgentRuntime;
class AgentSession;
class CanvasModel;

// Satu pesan chat kanvas: pertanyaan pengguna, atau jawaban agent atas satu pertanyaan
struct CanvasChatMessage {
    enum class Role { User, Agent };

    QString id;
    Role role = Role::User;
    QString text;                  // pertanyaan; jawaban Markdown, atau alasan gagal
    QDateTime at;                  // UTC

    // Pertanyaan: kartu bahan (kosong = seluruh kanvas) dan ringkasannya saat ditanyakan
    QStringList contextIds;
    QString contextLabel;

    // Jawaban: pertanyaan yang dijawab dan hasil run-nya
    QString replyTo;
    QString model;
    QString outcome;               // "success", "cancelled", "timeout", alasan gagal CLI, ...
    qint64 durationMs = 0;
    qint64 totalTokens = 0;
    double costUsd = 0.0;

    bool isUser() const { return role == Role::User; }
    bool succeeded() const { return role == Role::Agent && outcome == QLatin1String("success"); }

    QJsonObject toJson() const;
    static std::optional<CanvasChatMessage> fromJson(const QJsonObject &object);
};

// Percakapan tanya jawab satu kanvas brainstorm: menyimpan pesan dan menjalankan agent untuk satu
// pertanyaan pada satu waktu. Agent-nya baca-saja dan mulai dari konteks bersih setiap pertanyaan:
// bahan kartu, percakapan sebelumnya (dibatasi), dan pertanyaannya dirakit ulang ke prompt.
// Percakapan bukan bagian isi kanvas: tidak ikut undo/redo, tersimpan di canvas.json ("chat").
class CanvasChat : public QObject {
    Q_OBJECT

public:
    static constexpr int kMaxMessages = 200;

    // Lingkungan run untuk bahan itu (folder kerja, folder baca, foto); agent dan prompt diisi di sini.
    // nullopt + *reason bila tidak bisa dijalankan.
    using LaunchBuilder = std::function<std::optional<AgentLaunch>(const QStringList &contextIds, QString *reason)>;

    CanvasChat(CanvasModel &model, AgentRuntime &runtime, LaunchBuilder builder, QObject *parent = nullptr);

    const QList<CanvasChatMessage> &messages() const { return m_messages; }
    const CanvasChatMessage *message(const QString &id) const;
    // Isi dari disk; diabaikan selagi agent menjawab
    void load(const QList<CanvasChatMessage> &messages);
    QJsonArray toJson() const;
    static QList<CanvasChatMessage> fromJson(const QJsonArray &array);

    bool isBusy() const { return !m_session.isNull(); }
    QString liveText() const { return m_live; }
    QString activity() const { return m_activity; }

    // contextIds kosong = seluruh kanvas; model kosong = bawaan. false + *reason bila ditolak
    // (pertanyaan kosong, masih menjawab, kartu bahannya hilang, backend tidak siap).
    bool ask(const QString &question, const QStringList &contextIds, const QString &model, QString *reason = nullptr);
    // Jawaban yang sudah masuk dipertahankan sebagai jawaban "cancelled"
    void cancel();
    // false bila agent masih menjawab
    bool clear();

signals:
    void messageAdded(const CanvasChatMessage &message);
    // Daftar pesan diganti (load, clear, pesan terlama dibuang): tampilan dibangun ulang
    void messagesReset();
    void busyChanged(bool busy);
    // Jawaban yang sedang mengalir (Markdown) dan tool yang terakhir dipakai agent
    void liveChanged(const QString &markdown, const QString &activity);
    void started(const AgentLaunch &launch);
    void finished(const CanvasChatMessage &answer, const AgentResult &result);
    // Ada perubahan yang perlu disimpan
    void changed();

private:
    void finish(const QString &questionId, const QString &model, const AgentResult &result);
    void append(const CanvasChatMessage &message);

    CanvasModel &m_model;
    AgentRuntime &m_runtime;
    LaunchBuilder m_builder;
    QList<CanvasChatMessage> m_messages;
    QPointer<AgentSession> m_session;
    QString m_workingDirectory;
    QString m_live;
    QString m_activity;
};

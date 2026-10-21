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

// Satu sesi chat kanvas: satu percakapan yang berdiri sendiri, dikenali dari pertanyaan pertamanya
// atau dari judul yang diberikan pengguna
struct CanvasChatSession {
    QString id;
    QList<CanvasChatMessage> messages;
    QString name;                  // judul dari pengguna; kosong = pertanyaan pertamanya

    // Judul dari pengguna, atau pertanyaan pertamanya; satu baris
    QString title() const;
    int questionCount() const;
    // Waktu pesan terakhirnya (UTC)
    QDateTime lastAt() const;
};

// Tanya jawab satu kanvas brainstorm: menyimpan sesi-sesi percakapannya dan menjalankan agent untuk
// satu pertanyaan pada satu waktu, di sesi yang sedang dibuka. Agent-nya baca-saja dan mulai dari
// konteks bersih setiap pertanyaan: bahan kartu, percakapan sesi itu sebelumnya (dibatasi), dan
// pertanyaannya dirakit ulang ke prompt; sesi lain tidak ikut.
// Percakapan bukan bagian isi kanvas: tidak ikut undo/redo, tersimpan di canvas.json ("chat" dan
// "chatHistory").
class CanvasChat : public QObject {
    Q_OBJECT

public:
    static constexpr int kMaxMessages = 200;   // per sesi
    static constexpr int kMaxSessions = 50;

    // Lingkungan run untuk bahan itu (folder kerja, folder baca, foto); agent dan prompt diisi di sini.
    // nullopt + *reason bila tidak bisa dijalankan.
    using LaunchBuilder = std::function<std::optional<AgentLaunch>(const QStringList &contextIds, QString *reason)>;

    CanvasChat(CanvasModel &model, AgentRuntime &runtime, LaunchBuilder builder, QObject *parent = nullptr);

    // Pesan sesi yang sedang dibuka
    const QList<CanvasChatMessage> &messages() const { return m_current.messages; }
    const CanvasChatMessage *message(const QString &id) const;

    // Riwayat: sesi yang punya isi, yang terakhir dipakai lebih dulu
    QList<CanvasChatSession> sessions() const;
    QString currentSessionId() const { return m_current.id; }
    // Sesi kosong baru; percakapan yang sedang dibuka tetap ada di riwayat. false bila agent masih menjawab
    bool newSession();
    // false bila agent masih menjawab atau sesinya tidak ada
    bool openSession(const QString &id);
    // Judul sesi dari pengguna; kosong = kembali ke pertanyaan pertamanya. false bila sesinya tidak ada
    bool renameSession(const QString &id, const QString &name);

    // canvas.json: "chat" = pesan sesi yang sedang dibuka (bentuknya tetap seperti sebelum ada sesi,
    // jadi file lama terbaca apa adanya), "chatTitle" = judul sesi itu dari pengguna, "chatHistory" =
    // sesi-sesi lain sebagai {id, title?, messages}. load diabaikan selagi agent menjawab.
    void load(const QJsonArray &messages, const QJsonArray &history = QJsonArray(), const QString &name = QString());
    QJsonArray toJson() const;
    QString currentName() const { return m_current.name; }
    QJsonArray historyToJson() const;

    bool isBusy() const { return !m_session.isNull(); }
    QString liveText() const { return m_live; }
    QString activity() const { return m_activity; }

    // contextIds kosong = seluruh kanvas; model kosong = bawaan. false + *reason bila ditolak
    // (pertanyaan kosong, masih menjawab, kartu bahannya hilang, backend tidak siap).
    bool ask(const QString &question, const QStringList &contextIds, const QString &model, QString *reason = nullptr);
    // Jawaban yang sudah masuk dipertahankan sebagai jawaban "cancelled"
    void cancel();
    // Mengosongkan sesi yang sedang dibuka (sesi kosong tidak masuk riwayat); false bila agent masih menjawab
    bool clear();

signals:
    void messageAdded(const CanvasChatMessage &message);
    // Daftar pesan diganti (load, clear, pindah sesi, pesan terlama dibuang): tampilan dibangun ulang
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
    // Sesi yang sedang dibuka masuk riwayat bila ada isinya; di atas kMaxSessions, sesi yang pesan
    // terakhirnya paling lama dibuang
    void archiveCurrent();

    CanvasModel &m_model;
    AgentRuntime &m_runtime;
    LaunchBuilder m_builder;
    CanvasChatSession m_current;           // sesi yang sedang dibuka; boleh kosong
    QList<CanvasChatSession> m_archive;    // sesi lain, semuanya berisi; yang terakhir dibuka di belakang
    QPointer<AgentSession> m_session;      // run agent yang sedang menjawab
    QString m_workingDirectory;
    QString m_live;
    QString m_activity;
};

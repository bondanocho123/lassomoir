#pragma once

#include "AgentTypes.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>

class AgentRuntime;
class AgentSession;
class CanvasModel;

// Menjalankan langkah AI satu kanvas: satu proses agent per langkah (konteks bersih), paling banyak
// maxConcurrent bersamaan, sisanya antre. Langkah yang hulunya ikut antre menunggu hulunya selesai;
// bila hulunya gagal atau dibatalkan, langkah itu dilewati. Hasil run yang selesai dicatat ke
// CanvasModel; run yang dibatalkan, dilewati, atau gagal disiapkan tidak menimpa hasil sebelumnya.
class CanvasAutomation : public QObject {
    Q_OBJECT

public:
    // Dipanggil tepat sebelum langkah dijalankan supaya prompt-nya memakai hasil hulu yang terbaru.
    // nullopt + *reason bila langkah tidak bisa dijalankan.
    using LaunchBuilder = std::function<std::optional<AgentLaunch>(const QString &stepId, QString *reason)>;

    CanvasAutomation(CanvasModel &model, AgentRuntime &runtime, LaunchBuilder builder, QObject *parent = nullptr);

    // false + *reason bila ditolak: bukan langkah AI, sudah antre/berjalan, atau runtime tidak tersedia
    bool run(const QString &stepId, QString *reason = nullptr);
    // Langkah ini beserta semua langkah hulunya, urut dependensi
    bool runWithUpstream(const QString &stepId, QString *reason = nullptr);
    // Semua langkah AI di kanvas, urut dependensi
    bool runAll(QString *reason = nullptr);

    void cancel(const QString &stepId);
    void cancelAll();

    RunState state(const QString &stepId) const;
    bool isBusy() const { return !m_queue.isEmpty() || !m_active.isEmpty(); }
    void setMaxConcurrent(int count) { m_maxConcurrent = qMax(1, count); }

signals:
    void stateChanged(const QString &stepId, RunState state);
    void stepStarted(const QString &stepId, const AgentLaunch &launch);
    void stepEvent(const QString &stepId, const AgentEvent &event);
    // Juga untuk langkah yang dibatalkan, dilewati (outcome "skipped"), atau gagal disiapkan ("rejected")
    void stepFinished(const QString &stepId, const AgentResult &result);
    // Antrean kosong lagi: rekap semua langkah sejak antrean terakhir kosong
    void batchFinished(int succeeded, int failed, int skipped);

private:
    struct Pending {
        QString stepId;
        QStringList waitingFor;   // langkah hulu yang ikut antre/berjalan dan belum selesai
    };

    bool enqueue(const QStringList &order, QString *reason);
    void pump();
    void start(const QString &stepId);
    void settle(const QString &stepId, const AgentResult &result, bool record);
    void dropMissingSteps();

    CanvasModel &m_model;
    AgentRuntime &m_runtime;
    LaunchBuilder m_builder;
    int m_maxConcurrent = 2;
    QList<Pending> m_queue;
    QHash<QString, AgentSession *> m_active;
    bool m_pumping = false;
    bool m_pumpAgain = false;
    int m_succeeded = 0;
    int m_failed = 0;
    int m_skipped = 0;
};

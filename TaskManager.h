#ifndef TASKMANAGER_H
#define TASKMANAGER_H

#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QMap>
#include <QList>
#include <optional>
#include "TaskItem.h"

class StageCatalog;

// Pemilik state semua task: satu-satunya tempat stage/status task berubah.
// Widget hanya menampilkan data dari sinyalnya dan mengirim permintaan lewat fungsi di bawah.
class TaskManager : public QObject {
    Q_OBJECT

public:
    // Katalog dipakai untuk urutan stage dan aturan keluar (gate) tiap stage
    explicit TaskManager(const StageCatalog &catalog, QObject *parent = nullptr);
    ~TaskManager() override = default;

    // Mengambil satu data spesifik task berdasarkan taskId
    std::optional<TaskItem> task(const QString &taskId) const;

    // Mengambil daftar tugas berdasarkan ID proyek (misal "TTT" atau "spacewar")
    QList<TaskItem> tasksForProject(const QString &projectId) const;

    // Menambahkan item tugas baru ke dalam database/memori
    void addTask(const TaskItem &item);

    // Salin isi form edit (judul, kategori, subtext, pilihan model/effort per stage, lampiran)
    // ke task details.id; stage, status, dan riwayat run tidak tersentuh
    bool updateDetails(const TaskItem &details);

    // Pindah stage karena drag manual. Mundur selalu boleh; maju ditolak bila task sedang
    // menunggu review atau stage asalnya ber-gate dan belum disetujui (taskMoveRejected).
    bool moveTask(const QString &taskId, const QString &toStage, QString *reason = nullptr);

    // Buang semua task project dari memori (data di disk tidak disentuh)
    void removeProject(const QString &projectId);

    // Hasil satu run agent: dibatalkan -> Idle, gagal -> Failed,
    // sukses di stage ber-gate -> menunggu review, sukses tanpa gate -> maju ke stage berikutnya
    void recordRun(const QString &taskId, const StageRun &run);

    // Keputusan review untuk task yang menunggu review
    bool approve(const QString &taskId, const QString &note, QString *reason = nullptr);            // -> stage berikutnya
    bool requestRevision(const QString &taskId, const QString &note, QString *reason = nullptr);    // -> ulang di stage ini
    bool sendBack(const QString &taskId, const QString &toStage, const QString &note,
                  QString *reason = nullptr);                                                        // -> stage agent sebelumnya

    // Stage sesudah stageKey dalam pipeline; kosong bila sudah terakhir
    QString nextStage(const QString &stageKey) const;
    // Stage ber-agent sebelum stageKey (tujuan "Kembalikan ke"), urut pipeline
    QStringList sendBackTargets(const QString &stageKey) const;

signals:
    // Sinyal saat ada task baru yang ditambahkan
    void taskAdded(const TaskItem &item);

    // Isi atau status task berubah (termasuk run baru dan keputusan review)
    void taskChanged(const TaskItem &item);

    // Task pindah stage; widget kartu mengikuti ke kolom item.stage
    void taskMoved(const TaskItem &item, const QString &fromStage);

    // Dipancarkan saat pindah stage ditolak TransitionPolicy stage asal (mis. gate belum di-approve).
    // Widget kolom perlu dengar ini untuk mengembalikan kartu ke posisi semula.
    void taskMoveRejected(const QString &taskId, const QString &fromStage, const QString &reason);

private:
    const StageCatalog &m_catalog;

    // Penyimpanan internal state kartu (Key: taskId)
    QMap<QString, TaskItem> m_tasks;

    int stageIndex(const QString &stageKey) const;
    // Tandai run yang sedang direview; gagal bila task tidak sedang menunggu review
    StageRun *reviewedRun(TaskItem &task, QString *reason);
    // Pindahkan task ke stage lain dalam keadaan Idle, lalu pancarkan taskMoved
    void moveTo(TaskItem &task, const QString &toStage);
};

#endif // TASKMANAGER_H

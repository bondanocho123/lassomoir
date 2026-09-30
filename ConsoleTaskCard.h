#ifndef CONSOLETASKCARD_H
#define CONSOLETASKCARD_H

#pragma once

#include "AgentTypes.h"
#include "TaskItem.h"

#include <QPoint>
#include <QString>
#include <QWidget>

class QLabel;
class QPlainTextEdit;
class QToolButton;
class RunPulse;

// Satu kartu di daftar task panel konsol: semua notifikasi satu task dikumpulkan di sini.
// Kepala kartu: project, stage, status (antre/berjalan/review/gagal/selesai/dihapus), dan judul
// task; di bawahnya notifikasi terakhir, lalu log lengkap task yang bisa dibuka-tutup. Selama
// agent-nya berjalan kartu berkedip, sama seperti kartu kanban-nya.
// Kartu tanpa taskId adalah kartu "Sistem": notifikasi yang bukan milik task mana pun (project,
// folder kerja, pesan awal aplikasi).
class ConsoleTaskCard : public QWidget {
    Q_OBJECT

public:
    explicit ConsoleTaskCard(const QString &taskId, QWidget *parent = nullptr);

    QString taskId() const { return m_taskId; }
    bool isSystem() const { return m_taskId.isEmpty(); }

    // Project, stage, judul, dan status review/gagal mengikuti data task terbaru
    void setTask(const TaskItem &task);
    // Antre / berjalan (kartu berkedip) / selesai
    void setRunState(RunState state);
    RunState runState() const { return m_runState; }
    // Task sudah dihapus: kartu tetap ada sebagai riwayat, berstatus "Dihapus"
    void setRemoved();
    bool isRemoved() const { return m_removed; }

    // Satu notifikasi, boleh multi-baris (mis. [RUN] beserta isi prompt). Baris pertamanya jadi
    // ringkasan di kartu; seluruhnya masuk log, diawali jam kedatangannya.
    void appendEntry(const QString &text);
    int entryCount() const { return m_entryCount; }
    // Isi log kartu ini
    QString logText() const;

    // Log lengkap di bawah ringkasan; tertutup saat kartu dibuat
    void setLogVisible(bool visible);
    bool isLogVisible() const;

signals:
    // Kartu task yang masih ada diklik (di luar tombol dan isi log): tampilkan task-nya
    void activated(const QString &taskId);

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    // Kedip run di atas latar styles.qss
    void paintEvent(QPaintEvent *event) override;
    // Ringkasan dipotong ulang ("…") setiap lebar labelnya berubah
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    // Pil status + property "state" (garis kartu di styles.qss) dari status run/task/stage
    void refreshState();
    // Ringkasan satu baris yang muat di lebar labelnya
    void refreshSummary();

    QString m_taskId;
    QString m_taskLabel;     // "project/judul", sudah tampil di kepala kartu jadi dibuang dari ringkasan
    QString m_stage;
    TaskState m_taskState = TaskState::Idle;
    RunState m_runState = RunState::Idle;
    bool m_removed = false;
    int m_entryCount = 0;
    QString m_summaryText;   // ringkasan utuh; labelnya menampilkan versi yang muat
    QPoint m_pressPosition;
    bool m_pressed = false;

    QLabel *m_project;
    QLabel *m_stageLabel;
    QLabel *m_status;
    QLabel *m_title;
    QLabel *m_tag;
    QLabel *m_summary;
    QLabel *m_meta;
    QToolButton *m_toggle;
    QPlainTextEdit *m_log;
    RunPulse *m_pulse;
};

#endif // CONSOLETASKCARD_H

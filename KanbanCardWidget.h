#ifndef KANBANCARDWIDGET_H
#define KANBANCARDWIDGET_H


#pragma once

#include <QWidget>
#include <QPoint>
#include <QStringList>

#include "AgentTypes.h"
#include "TaskItem.h"

namespace Ui {
class KanbanCardWidget;
}

class RunPulse;


class KanbanCardWidget : public QWidget{
    Q_OBJECT;
public:
    explicit KanbanCardWidget(QWidget *parent = nullptr);
    ~KanbanCardWidget() override;

    void setCardData(const QString &id,
                     const QString &category,
                     const QString &title,
                     const QString &subtext,
                     const QString &badge);

    QString id() const { return m_id; }
    QString category() const { return m_category; }
    QString title() const { return m_title; }
    QString subtext() const { return m_subtext; }
    QString badge() const { return m_badge; }

    // Penanda lampiran di bawah subtext ("2 foto · 1 file"); tersembunyi bila keduanya 0.
    // names = nama file lampiran, untuk tooltip.
    void setAttachments(int images, int documents, const QStringList &names = QStringList());

    // Diatur kolom: tombol ▶ hanya aktif di stage yang punya agent
    void setRunEnabled(bool enabled);

    // Idle: ▶ · Queued/Running: ■ (batalkan). Selain Idle, kartu tidak bisa di-drag.
    // Selama Running kartu berkedip (RunPulse) supaya task yang sedang dikerjakan agent mudah dilihat.
    void setRunState(RunState state);
    RunState runState() const { return m_runState; }

    // Status task di stage-nya: Review -> tombol 📋 dan drag dikunci; Failed -> garis merah,
    // detail (alasan gagal) tampil di tooltip "Coba lagi"
    void setTaskState(TaskState state, const QString &detail = QString());
    TaskState taskState() const { return m_taskState; }

    // Stage yang pekerjaannya baru selesai (TaskItem::completedStage), kosong = tanpa penanda.
    // Penanda "✓ Selesai" disembunyikan selama run antre/berjalan; property "done" untuk styles.qss.
    void setCompletedStage(const QString &stage);

    // Stage tempat kartu sekarang berada; diatur kolom saat kartu masuk ke dalamnya
    void setStage(const QString &stage) { m_stage = stage; }
    QString stage() const { return m_stage; }

    // Semua stage pipeline (urut) untuk submenu "Pindah ke stage" di menu klik kanan
    void setMoveTargets(const QStringList &stages) { m_moveTargets = stages; }

signals:
    void cardClicked(const QString &cardId);
    // Tombol ▶ di pojok kanan atas kartu diklik saat Idle
    void runRequested(const QString &cardId);
    // Tombol ■ diklik saat run sedang antre atau berjalan
    void cancelRequested(const QString &cardId);
    // Tombol 📋 diklik saat task menunggu review
    void reviewRequested(const QString &cardId);
    // Kartu diklik sekali (tanpa drag): tampilkan hasil agent
    void detailsRequested(const QString &cardId);
    // Kartu diklik dua kali (atau "Edit task" di menu klik kanan): pengguna ingin mengubah isi task
    void editRequested(const QString &cardId);
    // "Hapus task" di menu klik kanan; konfirmasinya urusan penerima sinyal
    void deleteRequested(const QString &cardId);
    // Stage dipilih di submenu "Pindah ke stage"; boleh-tidaknya diputuskan penerima sinyal
    void moveRequested(const QString &cardId, const QString &targetStage);

protected:
    //Event penanganan drag and drop
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    // Klik kanan: menu Edit / Pindah ke stage / Hapus task
    void contextMenuEvent(QContextMenuEvent *event) override;
    // Kedip run di atas latar styles.qss
    void paintEvent(QPaintEvent *event) override;

private :
    Ui::KanbanCardWidget *ui;
    RunPulse *m_pulse;

    // Titik awal klik untuk mendeteksi ambang drag (drag threshold)
    QPoint m_dragStartPosition;
    // Drag sudah dimulai sejak tekan terakhir: lepas mouse bukan klik
    bool m_dragStarted = false;

    //Data internal kartu
    QString m_id;
    QString m_category;
    QString m_title;
    QString m_subtext;
    QString m_badge;
    QString m_stage;
    QStringList m_moveTargets;

    // Status tombol run
    bool m_runEnabled = false;
    RunState m_runState = RunState::Idle;
    TaskState m_taskState = TaskState::Idle;
    QString m_taskStateDetail;
    QString m_completedStage;

    void updateUI();
    void onRunButtonClicked();
    // Ikon, tooltip, dan enabled tombol run mengikuti m_runEnabled + m_runState + m_taskState
    void refreshRunButton();
    // Property "state" untuk styles.qss: queued/running mengalahkan review/failed
    void refreshStateProperty();
    // Penanda "✓ Selesai" mengikuti m_completedStage + m_runState
    void refreshDoneMarker();
};

#endif // KANBANCARDWIDGET_H

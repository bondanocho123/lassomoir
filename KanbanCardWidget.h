#ifndef KANBANCARDWIDGET_H
#define KANBANCARDWIDGET_H


#pragma once

#include <QWidget>
#include <QPoint>

#include "AgentTypes.h"
#include "TaskItem.h"

namespace Ui {
class KanbanCardWidget;
}


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

    // Diatur kolom: tombol ▶ hanya aktif di stage yang punya agent
    void setRunEnabled(bool enabled);

    // Idle: ▶ · Queued/Running: ■ (batalkan). Selain Idle, kartu tidak bisa di-drag.
    void setRunState(RunState state);
    RunState runState() const { return m_runState; }

    // Status task di stage-nya: Review -> tombol 📋 dan drag dikunci; Failed -> garis merah,
    // detail (alasan gagal) tampil di tooltip "Coba lagi"
    void setTaskState(TaskState state, const QString &detail = QString());
    TaskState taskState() const { return m_taskState; }

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
    // Kartu diklik dua kali: pengguna ingin mengubah isi task
    void editRequested(const QString &cardId);

protected:
    //Event penanganan drag and drop
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;

private :
    Ui::KanbanCardWidget *ui;

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

    // Status tombol run
    bool m_runEnabled = false;
    RunState m_runState = RunState::Idle;
    TaskState m_taskState = TaskState::Idle;
    QString m_taskStateDetail;

    void updateUI();
    void onRunButtonClicked();
    // Ikon, tooltip, dan enabled tombol run mengikuti m_runEnabled + m_runState + m_taskState
    void refreshRunButton();
    // Property "state" untuk styles.qss: queued/running mengalahkan review/failed
    void refreshStateProperty();
};

#endif // KANBANCARDWIDGET_H

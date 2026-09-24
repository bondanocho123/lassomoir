#ifndef RESPONSEDRAWER_H
#define RESPONSEDRAWER_H

#pragma once

#include "AgentTypes.h"
#include "TaskItem.h"

#include <QList>
#include <QString>
#include <QStringList>
#include <QWidget>

class MarkdownView;
class MermaidRenderer;
class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;

// Panel hasil agent untuk satu task: pemilih run (termasuk "Live" saat berjalan), metrik,
// dokumen Markdown/Mermaid, dan panel keputusan saat task menunggu review.
// Pasif: data datang dari MainWindow, keputusan dikirim balik lewat sinyal.
class ResponseDrawer : public QWidget {
    Q_OBJECT

public:
    explicit ResponseDrawer(MermaidRenderer *renderer, QWidget *parent = nullptr);

    QString taskId() const { return m_task.id; }

    // Tampilkan task, atau segarkan bila task yang sama. Run terbaru dipilih ("Live" bila sedang
    // berjalan); pilihan pengguna dipertahankan selama masih ada.
    void showTask(const TaskItem &task, RunState runState, const QString &nextStage,
                  const QStringList &sendBackStages);

    // Run baru dimulai untuk task yang sedang tampil: tampilan Live dikosongkan dan dipilih
    void startLive();
    // Satu event run yang sedang berjalan (teks, tool, stderr)
    void appendLive(const AgentEvent &event, const QString &workingDirectory);

    // Sinkronkan ikon/tooltip tombol expand dengan keadaan drawer (tanpa memancarkan sinyal)
    void setExpanded(bool expanded);
    bool isExpanded() const { return m_expanded; }

signals:
    void closeRequested();   // tombol ✕ atau Esc
    void expandToggled(bool expanded);   // tombol expand: true = lebarkan, false = kembalikan
    void approveRequested(const QString &taskId, const QString &note);
    void revisionRequested(const QString &taskId, const QString &note);
    void sendBackRequested(const QString &taskId, const QString &stage, const QString &note);

private:
    void rebuildRunSelector(int selected);   // selected: indeks run, atau kLive
    void showSelected();
    void updateReviewPanel();
    int selectedRun() const;
    int reviewedRunIndex() const;            // run yang sedang direview; -1 bila tidak ada
    bool requireNote();                      // false + peringatan bila catatan kosong

    TaskItem m_task;
    RunState m_runState = RunState::Idle;
    QString m_nextStage;
    QString m_liveMarkdown;
    bool m_expanded = false;

    QLabel *m_title;
    QLabel *m_status;
    QPushButton *m_expand;
    QComboBox *m_runSelector;
    QLabel *m_metrics;
    MarkdownView *m_view;
    QWidget *m_reviewPanel;
    QPlainTextEdit *m_note;
    QLabel *m_noteHint;
    QPushButton *m_approve;
    QPushButton *m_revise;
    QWidget *m_sendBackRow;
    QComboBox *m_sendBackTarget;
    QPushButton *m_sendBack;
};

#endif // RESPONSEDRAWER_H

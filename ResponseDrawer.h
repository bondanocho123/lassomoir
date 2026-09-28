#ifndef RESPONSEDRAWER_H
#define RESPONSEDRAWER_H

#pragma once

#include "AgentTypes.h"
#include "ClassDiagram.h"
#include "TaskItem.h"

#include <QList>
#include <QString>
#include <QStringList>
#include <QWidget>

struct WorkspaceDiff;
class DiffView;
class MaintainabilityView;
class MarkdownView;
class MermaidRenderer;
class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QStackedWidget;
class QTabBar;

// Panel hasil agent untuk satu task: pemilih run (termasuk "Live" saat berjalan), metrik,
// dokumen Markdown/Mermaid, dan panel keputusan saat task menunggu review. Di stage yang
// berurusan dengan kode ada tab tambahan: perubahan kode folder kerja (CODER dan ARCHITECT),
// serta di stage peninjauan (ARCHITECT) Maintainability Index-nya dan diagram UML.
// Pasif: data datang dari MainWindow, keputusan dan permintaan diff dikirim balik lewat sinyal.
class ResponseDrawer : public QWidget {
    Q_OBJECT

public:
    explicit ResponseDrawer(MermaidRenderer *renderer, QWidget *parent = nullptr);

    // Stage yang menampilkan tab perubahan kode: CODER menulisnya, ARCHITECT meninjaunya
    static bool showsCodeChanges(const QString &stageKey);
    // Stage peninjauan kode: selain diff ada tab Maintainability dan UML
    static bool showsCodeAnalysis(const QString &stageKey);

    QString taskId() const { return m_task.id; }

    // Tampilkan task, atau segarkan bila task yang sama. Run terbaru dipilih ("Live" bila sedang
    // berjalan); pilihan pengguna dipertahankan selama masih ada.
    void showTask(const TaskItem &task, RunState runState, const QString &nextStage,
                  const QStringList &sendBackStages);

    // Run baru dimulai untuk task yang sedang tampil: tampilan Live dikosongkan dan dipilih
    void startLive();
    // Satu event run yang sedang berjalan (teks, tool, stderr)
    void appendLive(const AgentEvent &event, const QString &workingDirectory);

    // Jawaban diffRequested; diabaikan bila drawer sudah beralih ke task atau stage lain
    void showDiff(const QString &taskId, const WorkspaceDiff &diff);

    // Status git task yang sedang tampil (mis. "Merge … ke development…" atau alasan merge gagal).
    // busy = tombol keputusan dinonaktifkan sampai git selesai. Teks kosong = sembunyikan.
    void setGitActivity(const QString &taskId, const QString &text, bool busy);

    // Sinkronkan ikon/tooltip tombol expand dengan keadaan drawer (tanpa memancarkan sinyal)
    void setExpanded(bool expanded);
    bool isExpanded() const { return m_expanded; }

signals:
    void closeRequested();   // tombol ✕ atau Esc
    void expandToggled(bool expanded);   // tombol expand: true = lebarkan, false = kembalikan
    void approveRequested(const QString &taskId, const QString &note);
    void revisionRequested(const QString &taskId, const QString &note);
    void sendBackRequested(const QString &taskId, const QString &stage, const QString &note);
    // Perubahan kode folder kerja task ini perlu dibaca (lagi); jawabannya lewat showDiff()
    void diffRequested(const QString &taskId);

private:
    void requestDiff();
    // Tab UML: diagram kelas dari kode + diagram Mermaid dari hasil agent stage ini
    void refreshUml();
    QStringList agentDiagrams() const;
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
    bool m_diffLoading = false;
    bool m_diffStale = false;   // agent menulis file sejak diff dibaca; dibaca ulang saat tabnya terlihat
    QString m_diffError;
    ClassDiagram m_classDiagram;

    QLabel *m_title;
    QLabel *m_status;
    QLabel *m_branch;          // branch git task; tersembunyi bila task tanpa branch
    QPushButton *m_expand;
    QTabBar *m_tabs;           // hanya tampil di stage yang menampilkan perubahan kode
    QStackedWidget *m_pages;   // [hasil agent | perubahan kode | maintainability | UML], mengikuti m_tabs
    QComboBox *m_runSelector;
    QLabel *m_metrics;
    MarkdownView *m_view;
    DiffView *m_diffView;
    MaintainabilityView *m_maintainabilityView;
    MarkdownView *m_umlView;
    QWidget *m_reviewPanel;
    QPlainTextEdit *m_note;
    QLabel *m_noteHint;
    QLabel *m_gitActivity;
    QPushButton *m_approve;
    QPushButton *m_revise;
    QWidget *m_sendBackRow;
    QComboBox *m_sendBackTarget;
    QPushButton *m_sendBack;
};

#endif // RESPONSEDRAWER_H

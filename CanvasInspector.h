#pragma once

#include "AgentTypes.h"
#include "CanvasBoard.h"

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
class QStackedWidget;
class QToolButton;

// Isi drawer kanan kanvas: isi lengkap kartu yang dipilih. Catatan dan instruksi langkah AI diedit di
// sini (tersimpan saat fokus pindah), dokumen artefak dan hasil langkah AI dibaca sebagai Markdown,
// dan aksi alur kerja (jalankan, jadikan catatan/task, buat task usulan) ada di sini. Status run
// langkah AI (selesai, gagal, durasi, token) tidak diulang di sini: sudah dilaporkan di Lieutenant.
class CanvasInspector : public QWidget {
    Q_OBJECT

public:
    explicit CanvasInspector(MermaidRenderer *renderer, QWidget *parent = nullptr);
    ~CanvasInspector() override;

    // ids: kartu terpilih. Teks yang sedang diketik di panel ini tidak ditimpa; bila pilihan pindah
    // ke kartu lain, ketikan itu disimpan dulu.
    void showSelection(const CanvasBoard &board, const QStringList &ids, RunState stepState, const QString &live);
    QString nodeId() const { return m_node.id; }   // kosong bila yang tampil bukan satu kartu
    // Keluaran agent yang sedang berjalan untuk langkah yang tampil
    void setLiveOutput(const QString &markdown);
    // Ketikan di editor catatan/instruksi yang belum tersimpan dikirim sekarang (textCommitted)
    void commitText();

signals:
    void textCommitted(const QString &id, const QString &text);
    void colorChosen(const QString &id, const QString &color);
    void stepOptionsChosen(const QString &id, const QString &model, const QString &effort, CanvasStepOutput output);
    void runRequested(const QString &id, bool withUpstream);
    void cancelRequested(const QString &id);
    void noteFromResultRequested(const QString &id);
    void createTaskRequested(const QStringList &ids);
    void proposalsRequested(const QString &id);
    void openTaskRequested(const QString &taskId);
    void removeRequested(const QStringList &ids);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void showEmpty(const CanvasBoard &board);
    void showMany(const CanvasBoard &board, const QStringList &ids);
    void showNote(const CanvasNode &node, bool sameNode);
    void showReference(const CanvasNode &node);
    void showStep(const CanvasNode &node, bool sameNode, RunState state, const QString &live);
    void setHeader(const QString &kind, const QString &title, const QString &meta);
    void emitStepOptions();

    MermaidRenderer *m_renderer;
    CanvasNode m_node;            // kartu yang tampil; id kosong = petunjuk atau banyak kartu
    QStringList m_ids;
    RunState m_state = RunState::Idle;

    QLabel *m_kind;
    QLabel *m_title;
    QLabel *m_meta;
    QStackedWidget *m_pages;

    QLabel *m_tips;
    QLabel *m_stats;

    QPlainTextEdit *m_noteText;
    QList<QToolButton *> m_swatches;

    QLabel *m_referenceMissing;
    MarkdownView *m_referenceView;
    QPushButton *m_openTask;

    QPlainTextEdit *m_stepText;
    QComboBox *m_stepModel;
    QComboBox *m_stepEffort;
    QComboBox *m_stepOutput;
    QPushButton *m_stepRun;
    QPushButton *m_stepRunUpstream;
    MarkdownView *m_stepResult;
    QPushButton *m_stepToNote;
    QPushButton *m_stepProposals;

    QLabel *m_manySummary;
};

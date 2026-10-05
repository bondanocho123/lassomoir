#pragma once

#include "AgentTypes.h"
#include "CanvasBoard.h"
#include "TaskItem.h"

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QWidget>

class CanvasAutomation;
class CanvasChat;
class CanvasChatPanel;
class CanvasInspector;
class CanvasLibrary;
class CanvasModel;
class CanvasView;
class MermaidRenderer;
class QLabel;
class QPushButton;
class QSplitter;
class QStackedWidget;
class QToolButton;

// Halaman kanvas satu project: toolbar, pustaka artefak (kiri), kanvas tak terbatas (tengah), dan
// panel kanan yang bergantian antara detail kartu terpilih dan chat tanya jawab. Perubahan kanvas
// lewat CanvasModel; aksi yang butuh data di luar kanvas (isi referensi task, folder kerja, task
// baru, pertanyaan chat) diteruskan lewat sinyal ke CanvasWorkspace.
class CanvasPage : public QWidget {
    Q_OBJECT

public:
    CanvasPage(CanvasModel &model, CanvasAutomation &automation, CanvasChat &chat, MermaidRenderer *renderer,
               QWidget *parent = nullptr);
    ~CanvasPage() override;

    QString projectId() const;
    CanvasView *view() const { return m_view; }
    CanvasLibrary *library() const { return m_library; }
    CanvasChatPanel *chatPanel() const { return m_chatPanel; }

    // Panel chat dibuka dengan kartu terpilih sebagai bahan, input siap diketik
    void openChat();
    bool isChatOpen() const;
    // Pertanyaan chat ditolak: alasannya tampil di panel chat, ketikannya dibiarkan
    void showChatError(const QString &reason);

    void setLibraryTasks(const QList<TaskItem> &tasks, const QStringList &stageOrder);
    // Keluaran agent langkah yang sedang berjalan; kosong = run selesai
    void setLiveOutput(const QString &stepId, const QString &markdown);
    void showMessage(const QString &text, bool error = false);
    // Posisi tampilan dan teks yang masih diketik ke CanvasModel, tersimpan bersama canvas.json berikutnya
    void storeView();

signals:
    void boardRequested();
    // exact: dijatuhkan pengguna tepat di scenePos. Selain itu (klik dua kali di pustaka) kartunya
    // dicarikan tempat lapang di dekat scenePos.
    void sourcesDropped(const QList<CanvasSource> &sources, const QPointF &scenePos, bool exact);
    void runRequested(const QStringList &stepIds, bool withUpstream);
    void runAllRequested();
    void createTaskRequested(const QStringList &nodeIds);
    void proposalsRequested(const QString &stepId);
    void openTaskRequested(const QString &taskId);
    void viewStored();
    // contextIds kosong = seluruh kanvas; model kosong = bawaan
    void chatAskRequested(const QString &question, const QStringList &contextIds, const QString &model);

protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    void scheduleInspectorRefresh();
    void refreshInspector();
    void refreshToolbar();
    void noteFromResult(const QString &stepId);
    // "Jadikan catatan" pada jawaban chat: pertanyaan + jawaban, tersambung dari kartu bahannya
    void noteFromChat(const QString &answerId);
    enum class SidePanel { Detail, Chat };
    void showPanel(SidePanel panel);
    void hidePanel();

    CanvasModel &m_model;
    CanvasAutomation &m_automation;
    CanvasChat &m_chat;
    CanvasView *m_view;
    CanvasLibrary *m_library;
    CanvasInspector *m_inspector;
    CanvasChatPanel *m_chatPanel;
    QStackedWidget *m_side;
    QSplitter *m_splitter;
    QLabel *m_status;
    QLabel *m_zoomLabel;
    QPushButton *m_runAll;
    QPushButton *m_stopAll;
    QToolButton *m_undo;
    QToolButton *m_redo;
    QPushButton *m_toggleLibrary;
    QPushButton *m_toggleInspector;
    QPushButton *m_toggleChat;
    QTimer m_inspectorTimer;
    QTimer m_statusTimer;
    QHash<QString, QString> m_live;   // stepId -> keluaran agent yang sedang mengalir
    bool m_viewRestored = false;
};

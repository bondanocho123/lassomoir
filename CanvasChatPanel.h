#pragma once

#include "CanvasBoard.h"

#include <QString>
#include <QStringList>
#include <QTimer>
#include <QWidget>

class CanvasChat;
struct CanvasChatMessage;
class ElidedLabel;
class MarkdownView;
class MermaidRenderer;
class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QScrollArea;
class QStackedWidget;
class QVBoxLayout;

// Panel chat kanvas brainstorm (kanan, bergantian dengan Detail): tanya jawab dengan agent tentang
// artefak di kanvas. Bahan pertanyaan berikutnya adalah kartu yang sedang dipilih, atau seluruh
// kanvas bila tidak ada yang dipilih. Isi percakapan dibaca dari CanvasChat; pertanyaan baru dikirim
// sebagai intent (askRequested) supaya folder kerja bisa dipastikan dulu.
// Kepala panel: Mulai ulang, + (sesi baru), dan riwayat. Riwayat sesi menggantikan ruang chat sampai
// sebuah sesi dipilih atau tombol kembali ditekan.
class CanvasChatPanel : public QWidget {
    Q_OBJECT

public:
    CanvasChatPanel(CanvasChat &chat, MermaidRenderer *renderer, QWidget *parent = nullptr);
    ~CanvasChatPanel() override;

    // Kartu yang dipilih di kanvas; kosong = seluruh kanvas
    void setContext(const CanvasBoard &board, const QStringList &selectedIds);
    QStringList contextIds() const { return m_contextIds; }
    // Ruang chat tampil (riwayat ditutup) dan input siap diketik
    void focusInput();
    // Pertanyaan ditolak (mis. folder kerja belum ada): ketikannya dibiarkan
    void showError(const QString &text);
    QString draft() const;
    bool isHistoryOpen() const;

signals:
    void askRequested(const QString &question, const QStringList &contextIds, const QString &model);
    void noteRequested(const QString &answerId);
    // "Perluas" pada jawaban: isinya dibuka di drawer pratinjau
    void expandRequested(const QString &answerId);
    // "Ke Notion" pada jawaban: pertanyaan dan jawabannya dikirim sebagai halaman Notion
    void exportRequested(const QString &answerId);
    // Klik bahan sebuah pertanyaan: kartunya dipilih di kanvas
    void contextActivated(const QStringList &ids);

private:
    void rebuild();
    void addMessage(const CanvasChatMessage &message);
    QWidget *userBubble(const CanvasChatMessage &message);
    QWidget *agentBubble(const CanvasChatMessage &message);
    MarkdownView *answerView(QWidget *parent);
    void setBusy(bool busy);
    void showLive();
    void send();
    void fitInput();
    void updateEmptyState();
    void showHistory(bool open);
    void rebuildHistory();

    CanvasChat &m_chat;
    MermaidRenderer *m_renderer;
    QStringList m_contextIds;

    ElidedLabel *m_title;
    QPushButton *m_back;
    QPushButton *m_clear;
    QPushButton *m_new;
    QPushButton *m_history;
    QStackedWidget *m_pages;       // ruang chat, atau riwayat sesi
    QWidget *m_room;
    QWidget *m_historyPage;
    QVBoxLayout *m_sessions;
    QLabel *m_noSessions;
    QScrollArea *m_scroll;
    QVBoxLayout *m_list;
    QWidget *m_empty;
    QWidget *m_live = nullptr;
    QLabel *m_liveActivity = nullptr;
    MarkdownView *m_liveView = nullptr;
    QLabel *m_error;
    QLabel *m_context;
    QLabel *m_contextHint;
    QPlainTextEdit *m_input;
    QComboBox *m_model;
    QPushButton *m_send;
    QString m_pendingQuestion;     // dikirim, menunggu diterima CanvasChat
    bool m_stickToEnd = true;      // daftar pesan ikut turun selama pengguna tidak menggulir ke atas
    QTimer m_liveTimer;            // keluaran live dilukis paling sering tiap 120 ms
};

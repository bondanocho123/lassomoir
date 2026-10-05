#pragma once

#include "CanvasBoard.h"

#include <QString>
#include <QStringList>
#include <QTimer>
#include <QWidget>

class CanvasChat;
struct CanvasChatMessage;
class MarkdownView;
class MermaidRenderer;
class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QScrollArea;
class QVBoxLayout;

// Panel chat kanvas brainstorm (kanan, bergantian dengan Detail): tanya jawab dengan agent tentang
// artefak di kanvas. Bahan pertanyaan berikutnya adalah kartu yang sedang dipilih, atau seluruh
// kanvas bila tidak ada yang dipilih. Isi percakapan dibaca dari CanvasChat; pertanyaan baru dikirim
// sebagai intent (askRequested) supaya folder kerja bisa dipastikan dulu.
class CanvasChatPanel : public QWidget {
    Q_OBJECT

public:
    CanvasChatPanel(CanvasChat &chat, MermaidRenderer *renderer, QWidget *parent = nullptr);
    ~CanvasChatPanel() override;

    // Kartu yang dipilih di kanvas; kosong = seluruh kanvas
    void setContext(const CanvasBoard &board, const QStringList &selectedIds);
    QStringList contextIds() const { return m_contextIds; }
    void focusInput();
    // Pertanyaan ditolak (mis. folder kerja belum ada): ketikannya dibiarkan
    void showError(const QString &text);
    QString draft() const;

signals:
    void askRequested(const QString &question, const QStringList &contextIds, const QString &model);
    void noteRequested(const QString &answerId);
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

    CanvasChat &m_chat;
    MermaidRenderer *m_renderer;
    QStringList m_contextIds;

    QPushButton *m_clear;
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

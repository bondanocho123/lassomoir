#ifndef CONSOLEPANELWIDGET_H
#define CONSOLEPANELWIDGET_H

#include <QHash>
#include <QList>
#include <QString>
#include <QWidget>

#include "AgentTypes.h"
#include "TaskItem.h"

namespace Ui {
class ConsolePanelWidget;
}

class ConsoleTaskCard;

// Panel kanan: notifikasi aplikasi sebagai daftar kartu, satu kartu per task (ConsoleTaskCard)
// ditambah satu kartu "Sistem" untuk notifikasi di luar task. Kartu yang terakhir mendapat kabar
// naik ke paling atas. Di bawah daftar ada kotak perintah.
class ConsolePanelWidget : public QWidget
{
    Q_OBJECT

public:
    explicit ConsolePanelWidget(QWidget *parent = nullptr);
    ~ConsolePanelWidget();

    // Notifikasi yang bukan milik task (project, folder kerja, pesan awal): kartu "Sistem"
    void appendLog(const QString &log);
    // Notifikasi milik task: masuk kartu task-nya (dibuat dari data task bila belum ada), lalu
    // kartu itu naik ke paling atas
    void appendTaskLog(const TaskItem &task, const QString &log);
    // Keluaran agent yang sedang mengalir (teks, tool, stderr): sama, tetapi urutan kartu tidak
    // berubah, supaya daftar tidak berlompatan selama beberapa agent berjalan bersamaan
    void appendTaskOutput(const TaskItem &task, const QString &log);
    // Kepala kartu (project, stage, judul, status review/gagal) mengikuti data task terbaru;
    // diabaikan bila task belum punya kartu
    void updateTask(const TaskItem &task);
    // Antre / berjalan (kartu berkedip) / selesai; kartu dibuat bila run-nya baru antre/berjalan
    void setRunState(const TaskItem &task, RunState state);
    // Task dihapus: kartunya tetap ada sebagai riwayat, berstatus "Dihapus"
    void markTaskRemoved(const TaskItem &task);

    // Kartu task; nullptr bila task belum pernah mendapat notifikasi
    ConsoleTaskCard *taskCard(const QString &taskId) const;
    // Semua kartu dari atas ke bawah, termasuk kartu "Sistem"
    QList<ConsoleTaskCard *> cards() const;
    // Isi log semua kartu, mulai dari kartu teratas
    QString logText() const;

signals:
    // Kartu task diklik: pengguna ingin melihat task itu
    void taskActivated(const QString &taskId);

private:
    // Kartu task; bila belum ada, dibuat di paling atas daftar
    ConsoleTaskCard *ensureCard(const TaskItem &task);
    // Taruh kartu di paling atas daftar
    void promote(ConsoleTaskCard *card);

    Ui::ConsolePanelWidget *ui;
    QHash<QString, ConsoleTaskCard *> m_cards;   // key: taskId
    ConsoleTaskCard *m_systemCard = nullptr;
};

#endif // CONSOLEPANELWIDGET_H

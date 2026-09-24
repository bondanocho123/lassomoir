#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#pragma once

#include <QList>
#include <QMainWindow>
#include <QMap>
#include <QPointer>
#include <QString>

// Tipe lengkap dibutuhkan moc untuk parameter slot run agent
#include "AgentTypes.h"
#include "TaskItem.h"

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

// Forward declaration widget anak agar kompilasi cepat
class SwimlaneWidget;
class KanbanCardWidget;
class FileManager;
class MermaidRenderer;
class ResponseDrawer;
class SplitterPaneAnimator;
class StageCatalog;
class SwarmCoordinator;
class TaskManager;
class QListWidgetItem;
class QSplitter;
class QVariantAnimation;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    // Katalog stage, pemilik data task, coordinator agent, dan renderer diagram
    // dirakit di main.cpp (composition root)
    MainWindow(const StageCatalog &catalog, TaskManager &tasks, SwarmCoordinator &swarm,
               MermaidRenderer &mermaid, QWidget *parent = nullptr);
    ~MainWindow() override;

    // Fungsi untuk menambah baris proyek/swimlane baru
    void addSwimlane(const QString &projectId);

    // Tampilkan satu project di board dan sinkronkan seleksi sidebar
    void setActiveProject(const QString &projectId);

private slots:
    // Slot saat kartu dipindahkan antar-kolom di swimlane mana pun
    void handleCardMoved(const QString &projectId, KanbanCardWidget *card, const QString &targetStage, int targetIndex);

    // Slot saat tombol "New Task" di klik pada swimlane tertentu
    void handleNewTaskRequested(const QString &projectId);

    // Slot saat kartu diklik dua kali: buka form task terisi data kartu, lalu terapkan perubahannya
    void handleEditTaskRequested(const QString &projectId, const QString &taskId);

    // Slot saat tombol "Close" di header swimlane diklik
    void handleCloseProjectRequested(const QString &projectId);

    // Slot saat konfirmasi "Ya" pada popup hapus project ditekan:
    // project dibuang dari UI sekaligus dihapus permanen dari disk
    void handleDeleteProjectRequested(const QString &projectId);

    // Slot saat baris project di sidebar dipilih
    void handleProjectSelected(QListWidgetItem *current, QListWidgetItem *previous);

    // Slot saat ada perintah atau pesan dikirim dari ConsolePanel
    void handleCommandSubmitted(const QString &command);

    // Ciutkan / lebarkan panel sidebar
    void toggleSidebar();

    // Kejadian run agent dari SwarmCoordinator: perbarui kartu, drawer, dan konsol
    void handleRunQueued(const TaskItem &task);
    void handleRunStarted(const TaskItem &task, const AgentLaunch &launch);
    void handleRunEvent(const TaskItem &task, const AgentEvent &event);
    void handleRunFinished(const TaskItem &task, const AgentResult &result);

    // Perubahan data dari TaskManager: kartu, drawer, dan file sesi mengikuti
    void handleTaskAdded(const TaskItem &task);
    void handleTaskChanged(const TaskItem &task);
    void handleTaskMoved(const TaskItem &task, const QString &fromStage);
    void handleTaskMoveRejected(const QString &taskId, const QString &fromStage, const QString &reason);

    // Keputusan review dari drawer
    void handleApproveRequested(const QString &taskId, const QString &note);
    void handleRevisionRequested(const QString &taskId, const QString &note);
    void handleSendBackRequested(const QString &taskId, const QString &stage, const QString &note);

    // Drawer minta perubahan kode folder kerja task (stage peninjauan kode)
    void handleDiffRequested(const QString &taskId);

private:
    Ui::MainWindow *ui;
    const StageCatalog &m_catalog;
    TaskManager &m_tasks;
    SwarmCoordinator &m_swarm;
    FileManager *m_fileManager;
    // Drawer hasil agent, di splitter [board | drawer] yang menggantikan boardStack di mainSplitter
    QSplitter *m_boardSplitter = nullptr;
    ResponseDrawer *m_drawer = nullptr;
    SplitterPaneAnimator *m_drawerAnimator = nullptr;
    // Nomor permintaan diff terbaru; hanya jawabannya yang ditampilkan di drawer
    int m_diffRequest = 0;
    // Saat memuat dari disk tidak perlu menulis ulang session.json per task
    bool m_loading = false;
    // Perpindahan yang dipicu pengguna (drag / keputusan review) sudah dicatat pemanggilnya
    bool m_userMoveInProgress = false;
    // Daftar swimlane yang aktif (Key: projectId, misal "TTT", "spacewar")
    QMap<QString, SwimlaneWidget*> m_swimlanes;
    // Project yang sedang ditampilkan di board; kosong bila belum ada
    QString m_activeProjectId;
    // Lebar sidebar terakhir sebelum diciutkan, dipakai saat dibuka kembali
    int m_savedSidebarWidth = 0;
    // Animasi geser lebar sidebar saat dibuka/ditutup
    QPointer<QVariantAnimation> m_sidebarAnimation;
    // Lebar minimum asli sidebarPanel (dari file .ui), disimpan karena
    // animasi sempat menurunkannya ke 0 agar splitter bisa menciutkannya penuh
    int m_sidebarMinWidth = 180;
    // Batas maksimum asli sidebarPanel; maximumWidth dipakai untuk menggiring
    // lebar selama animasi sehingga nilainya harus disimpan dulu
    int m_sidebarMaxWidth = 240;

    // Helper untuk memuat data awal saat aplikasi baru dibuka
    void loadInitialMockData();
    // Muat semua project dari <AppData>/projects; return jumlah project yang dimuat
    int loadProjectsFromDisk();
    // Jadwalkan penulisan session.json project dari data TaskManager
    void saveProject(const QString &projectId);
    // Cari baris sidebar milik projectId; -1 bila tidak ada
    int findProjectRow(const QString &projectId) const;
    // Jalankan animasi geser: opening=true melebarkan sidebar, false menciutkannya
    void animateSidebar(bool opening);
    // Baris item sidebar kustom: label nama project (kiri) + tombol hapus & "+" New Task (kanan)
    QWidget *createProjectRowWidget(const QString &projectId);
    // Samakan warna label + icon tombol tiap baris sidebar dengan status seleksinya
    // (putih saat aktif/terpilih, warna default saat tidak)
    void refreshProjectRowStyles();
    // Popup kecil berisi pertanyaan konfirmasi + tombol "Ya" / "Batal",
    // ditempelkan tepat di bawah tombol hapus yang diklik
    void showDeleteConfirmPopup(const QString &projectId, QWidget *anchor);
    // Buang project dari sidebar + board (dipakai "Close" maupun "Hapus")
    void removeProjectFromUi(const QString &projectId);

    // Buat kartu, sambungkan tombol & klik-nya, lalu taruh di kolom stage task
    KanbanCardWidget *createCard(SwimlaneWidget *swimlane, const TaskItem &task);
    // Samakan isi kartu (teks, badge ✓ N, status Review/Failed) dengan data task
    void applyTaskToCard(KanbanCardWidget *card, const TaskItem &task);
    // Kartu milik task; nullptr bila project/kartunya sudah tidak ada
    KanbanCardWidget *cardFor(const TaskItem &task) const;
    // Buka drawer untuk task (hanya bila sudah ada hasil run atau sedang berjalan)
    void openDrawer(const QString &taskId);
    // Segarkan drawer bila sedang menampilkan task ini
    void refreshDrawer(const TaskItem &task);
    // Tombol ▶ diklik: pastikan folder kerja ada, lalu serahkan ke SwarmCoordinator
    void handleRunRequested(const QString &projectId, const QString &taskId);
    // Folder kerja tersimpan yang masih ada; kalau tidak ada, tanya pengguna
    QString ensureWorkingDirectory(const QString &projectId);
    // Pemilih folder -> FileManager -> tombol header swimlane; kosong bila dibatalkan
    QString chooseWorkingDirectory(const QString &projectId);

protected:
    // Tukar icon tombol baris sidebar jadi putih selama kursor berada di atasnya,
    // supaya tetap terbaca di atas background hover yang gelap
    bool eventFilter(QObject *watched, QEvent *event) override;
};

#endif // MAINWINDOW_H

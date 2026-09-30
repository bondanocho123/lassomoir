#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#pragma once

#include <QList>
#include <QMainWindow>
#include <QMap>
#include <QPointer>
#include <QSet>
#include <QString>

#include <functional>

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
namespace TaskAttachments {
struct Draft;
}
namespace TaskGit {
struct Result;
}

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

    // Slot saat konfirmasi "Ya" pada popup hapus task ditekan: agent-nya dihentikan,
    // task dibuang dari board dan session.json, folder lampirannya ikut dihapus
    void handleDeleteTaskRequested(const QString &taskId);

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
    void handleTaskRemoved(const TaskItem &task);

    // Keputusan review dari drawer
    void handleApproveRequested(const QString &taskId, const QString &note);
    void handleRevisionRequested(const QString &taskId, const QString &note);
    void handleSendBackRequested(const QString &taskId, const QString &stage, const QString &note);

    // Drawer minta perubahan kode folder kerja task (stage peninjauan kode)
    void handleDiffRequested(const QString &taskId);

    // Tombol × di popup folder referensi swimlane
    void removeReferenceDirectory(const QString &projectId, const QString &dir);

    // Jendela branch & commit project: dari tombol branch di header swimlane (branch aktif folder
    // kerja), atau dari "Lihat commit" di drawer task (branch task dibanding branch dasarnya)
    void showBranchViewer(const QString &projectId, const QString &branch = QString(),
                          const QString &compareWith = QString());

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
    // Task yang sedang menunggu git (pull, worktree, commit + push ke QA); run berikutnya ditolak
    // sampai selesai
    QSet<QString> m_gitBusy;
    // Tombol stop ditekan selagi git menyiapkan run: agent tidak dijalankan sesudahnya
    QSet<QString> m_gitCancelled;
    // Saat memuat dari disk tidak perlu menulis ulang session.json per task
    bool m_loading = false;
    // Perpindahan karena keputusan review sudah dicatat pemanggilnya ([GATE] ...)
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
    // Popup kecil berisi pertanyaan konfirmasi + tombol "Ya" / "Batal", ditempelkan tepat di
    // bawah anchor (tombol hapus project atau kartu task). onConfirm dijalankan di siklus event
    // berikutnya, jadi boleh membuang anchor-nya.
    void showDeleteConfirmPopup(QWidget *anchor, const QString &question, std::function<void()> onConfirm);
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
    // Tombol ▶ diklik: pastikan folder kerja ada, pull dulu (folder kerja di repository git), siapkan
    // branch/worktree task (dan di QA: commit + push) bila perlu, lalu serahkan ke SwarmCoordinator
    void handleRunRequested(const QString &projectId, const QString &taskId);
    // Task baru (atau yang branch dasarnya diganti) langsung pull branch dasarnya di thread pool;
    // hasilnya dicatat di kartu konsol task. Gagal tidak menghalangi apa pun: run tetap pull lagi.
    void pullBaseLater(const TaskItem &task);
    // Baca lampiran & folder referensi lalu jalankan agent di folder itu; false bila ditolak
    bool startAgentRun(const TaskItem &task, const QString &workingDirectory);
    // Git selesai menyiapkan run task (requested = data saat ▶ diklik)
    void finishRunPreparation(const TaskItem &requested, const TaskGit::Result &result);
    // Folder kerja agent task: worktree-nya bila ada, selain itu folder kerja project
    QString taskDirectory(const TaskItem &task) const;
    // Baris [GIT] untuk yang sudah dikerjakan dan peringatannya
    void logGitResult(const TaskItem &task, const TaskGit::Result &result);
    // Notifikasi milik task ke kartu task-nya di panel konsol
    void logTask(const TaskItem &task, const QString &line);
    // Status run (antre / berjalan = berkedip / selesai) di kartu kanban dan kartu konsol task
    void showRunState(const TaskItem &task, RunState state);
    // Buang worktree task (dihapus) di thread pool; branch-nya dibiarkan
    void removeWorktreeLater(const TaskItem &task);
    // Folder kerja tersimpan yang masih ada; kalau tidak ada, tanya pengguna
    QString ensureWorkingDirectory(const QString &projectId);
    // Pemilih folder -> FileManager -> tombol header swimlane; kosong bila dibatalkan
    QString chooseWorkingDirectory(const QString &projectId);
    // Pemilih folder referensi (dibaca agent, tidak diubah) -> addReferenceDirectory
    void chooseReferenceDirectory(const QString &projectId);
    // Tolak folder di dalam folder kerja atau yang sudah tercakup referensi lain; false bila ditolak
    bool addReferenceDirectory(const QString &projectId, const QString &dir);
    // Simpan foto & dokumen dari form task ke folder lampiran task; kembalikan nama yang tersimpan
    QStringList saveAttachments(const TaskItem &task, const QList<TaskAttachments::Draft> &drafts);
    // Baca branch aktif folder kerja project (git di thread pool) untuk tombol branch di header
    // swimlane; folder yang jelas bukan repository langsung menyembunyikan tombolnya
    void refreshGitHead(const QString &projectId);

protected:
    // Tukar icon tombol baris sidebar jadi putih selama kursor berada di atasnya,
    // supaya tetap terbaca di atas background hover yang gelap
    bool eventFilter(QObject *watched, QEvent *event) override;
    // Jendela kembali aktif: branch folder kerja project yang tampil bisa sudah diganti di luar aplikasi
    void changeEvent(QEvent *event) override;
};

#endif // MAINWINDOW_H

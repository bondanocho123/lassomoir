#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#pragma once

#include <QList>
#include <QMainWindow>
#include <QMap>
#include <QPointer>
#include <QString>

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

// Forward declaration widget anak agar kompilasi cepat
class SwimlaneWidget;
class KanbanCardWidget;
class FileManager;
class QListWidgetItem;
class QVariantAnimation;
struct TaskItem;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
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

private:
    Ui::MainWindow *ui;
    FileManager *m_fileManager;
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
    QMap<QString, TaskItem> collectTasksForProject(const QString &projectId) const;
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

protected:
    // Tukar icon tombol baris sidebar jadi putih selama kursor berada di atasnya,
    // supaya tetap terbaca di atas background hover yang gelap
    bool eventFilter(QObject *watched, QEvent *event) override;
};

#endif // MAINWINDOW_H

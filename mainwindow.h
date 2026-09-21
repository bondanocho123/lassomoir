#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#pragma once

#include <QMainWindow>
#include <QMap>
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
class TaskItem;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    // Fungsi untuk menambah baris proyek/swimlane baru
    void addSwimlane(const QString &projectId);

private slots:
    // Slot saat kartu dipindahkan antar-kolom di swimlane mana pun
    void handleCardMoved(const QString &projectId, KanbanCardWidget *card, const QString &targetStage, int targetIndex);

    // Slot saat tombol "New Task" di klik pada swimlane tertentu
    void handleNewTaskRequested(const QString &projectId);

    // Slot saat ada perintah atau pesan dikirim dari ConsolePanel
    void handleCommandSubmitted(const QString &command);

private:
    Ui::MainWindow *ui;
    FileManager *m_fileManager;
    // Daftar swimlane yang aktif (Key: projectId, misal "TTT", "spacewar")
    QMap<QString, SwimlaneWidget*> m_swimlanes;

    // Helper untuk memuat data awal saat aplikasi baru dibuka
    void loadInitialMockData();
    QMap<QString, TaskItem> collectTasksForProject(const QString &projectId) const;
};

#endif // MAINWINDOW_H
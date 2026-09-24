#include <QApplication>
#include <QFile>
#include <QDebug>
#include <QIcon>

#include "mainwindow.h" // Gunakan "mainwindow.h" jika tanpa subfolder
#include "ClaudeCodeRuntime.h"
#include "EdgeMermaidRenderer.h"
#include "PromptComposer.h"
#include "StageCatalog.h"
#include "SwarmCoordinator.h"
#include "TaskManager.h"

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    QCoreApplication::setApplicationName("Lassomoir");
    a.setWindowIcon(QIcon(":/app.ico"));

    // 1. Muat Stylesheet Global
    QFile styleFile(":/styles.qss");
    if (styleFile.open(QFile::ReadOnly | QFile::Text)) {
        a.setStyleSheet(QString::fromUtf8(styleFile.readAll()));
        styleFile.close();
    } else {
        qWarning() << "Peringatan: Gagal memuat :/styles.qss. Tampilan akan menggunakan default Qt.";
    }

    // 2. Rakit dependensi. Urutan deklarasi penting: objek dihancurkan terbalik, jadi jendela
    //    tutup lebih dulu, lalu browser render diagram dan proses agent dimatikan, baru
    //    runtime dan katalog yang dipakainya ikut hilang.
    const StageCatalog catalog = StageCatalog::standard();
    ClaudeCodeRuntime runtime;
    TaskPromptComposer composer;
    SwarmCoordinator swarm(catalog, runtime, composer);
    TaskManager tasks(catalog);
    EdgeMermaidRenderer mermaid;

    // Hasil setiap run dicatat di riwayat task; TaskManager yang memutuskan task maju,
    // menunggu review, atau gagal
    QObject::connect(&swarm, &SwarmCoordinator::runFinished, &tasks,
                     [&tasks](const TaskItem &task, const AgentResult &result) {
        tasks.recordRun(task.id, StageRun::finished(task.stage, result));
    });

    // 3. Jalankan MainWindow Utama
    MainWindow window(catalog, tasks, swarm, mermaid);
    window.setWindowTitle("L'Assommoir - Workflow Orchestrator");
    window.resize(1440, 850);
    window.show();

    return QApplication::exec();
}
#include <QApplication>
#include <QIcon>
#include <QThreadPool>

#include "mainwindow.h" // Gunakan "mainwindow.h" jika tanpa subfolder
#include "AppFonts.h"
#include "ClaudeCodeRuntime.h"
#include "EdgeMermaidRenderer.h"
#include "PromptComposer.h"
#include "StageCatalog.h"
#include "SwarmCoordinator.h"
#include "TaskManager.h"
#include "Theme.h"

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    QCoreApplication::setApplicationName("Lassomoir");
    a.setWindowIcon(QIcon(":/app.ico"));

    // 1. Font tertanam + font pilihan pengguna (bawaan: Garamond dari styles.qss), lalu stylesheet
    //    global; mode terang/gelap mengikuti Windows, juga saat aplikasi berjalan
    AppFonts::registerBundled();
    Theme::setUiFontFamily(AppFonts::saved());
    Theme::install(a);

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
    MainWindow window(catalog, tasks, swarm, runtime, mermaid);
    window.setWindowTitle("L'Assommoir - Workflow Orchestrator");
    window.resize(1440, 850);
    window.show();

    // Claude Code terpasang, cukup baru, dan sudah login? Diperiksa di belakang layar;
    // bila tidak, notice berisi cara memperbaikinya muncul di atas jendela
    window.checkRuntime();

    const int exitCode = QApplication::exec();
    // Pemeriksaan Claude Code (checkRuntime) yang masih berjalan memakai runtime & swarm di atas:
    // tunggu sampai selesai sebelum keduanya dihancurkan
    QThreadPool::globalInstance()->waitForDone();
    return exitCode;
}
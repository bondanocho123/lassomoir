#include <QApplication>
#include <QFile>
#include <QDebug>
#include <QIcon>

#include "mainwindow.h" // Gunakan "mainwindow.h" jika tanpa subfolder

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

    // 2. Jalankan MainWindow Utama
    MainWindow window;
    window.setWindowTitle("L'Assommoir - Workflow Orchestrator");
    window.resize(1440, 850);
    window.show();

    return QApplication::exec();
}
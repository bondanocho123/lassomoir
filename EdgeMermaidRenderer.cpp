#include "EdgeMermaidRenderer.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QUrl>

namespace {

constexpr int kTimeoutMs = 30000;
constexpr qreal kScale = 2.0;   // sama dengan --force-device-scale-factor

// Tema "base" Mermaid dengan palet aplikasi (lihat styles.qss)
constexpr char kMermaidConfig[] =
    "{startOnLoad: true, securityLevel: 'strict', theme: 'base', themeVariables: {"
    "primaryColor: '#f2e3d1', primaryBorderColor: '#a9743f', primaryTextColor: '#3d3730',"
    "secondaryColor: '#e3ebf6', tertiaryColor: '#fbf8f3', lineColor: '#33517a',"
    "noteBkgColor: '#fbeccf', noteBorderColor: '#b7791f',"
    "fontFamily: 'Segoe UI, sans-serif', fontSize: '15px'}}";

}

EdgeMermaidRenderer::EdgeMermaidRenderer(QString browser, QString cacheDir, QObject *parent)
    : MermaidRenderer(parent),
      m_browser(std::move(browser)),
      m_cacheDir(cacheDir.isEmpty()
                     ? QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/mermaid")
                     : std::move(cacheDir)) {
    m_timeout.setSingleShot(true);
    connect(&m_timeout, &QTimer::timeout, this, [this]() {
        if (!m_process) {
            return;
        }
        m_process->disconnect(this);
        m_process->kill();
        m_process->waitForFinished(2000);
        finishCurrent(QImage(), QStringLiteral("melewati batas %1 detik").arg(kTimeoutMs / 1000));
    });
}

EdgeMermaidRenderer::~EdgeMermaidRenderer() {
    // Browser harus berhenti sebelum folder kerja sementara dihapus
    if (m_process && m_process->state() != QProcess::NotRunning) {
        m_process->disconnect(this);
        m_process->kill();
        m_process->waitForFinished(2000);
    }
}

QString EdgeMermaidRenderer::findBrowser() {
    QStringList candidates;
    for (const char *variable : {"ProgramFiles(x86)", "ProgramFiles"}) {
        const QString root = qEnvironmentVariable(variable);
        if (!root.isEmpty()) {
            candidates << root + QStringLiteral("/Microsoft/Edge/Application/msedge.exe");
        }
    }
    for (const char *variable : {"ProgramFiles", "ProgramFiles(x86)"}) {
        const QString root = qEnvironmentVariable(variable);
        if (!root.isEmpty()) {
            candidates << root + QStringLiteral("/Google/Chrome/Application/chrome.exe");
        }
    }
    for (const QString &path : std::as_const(candidates)) {
        if (QFileInfo::exists(path)) {
            return QDir::cleanPath(path);
        }
    }
    for (const char *name : {"msedge", "chrome", "google-chrome", "chromium"}) {
        const QString found = QStandardPaths::findExecutable(QLatin1String(name));
        if (!found.isEmpty()) {
            return found;
        }
    }
    return QString();
}

bool EdgeMermaidRenderer::isAvailable(QString *reason) const {
    if (m_browser.isEmpty() || !QFileInfo::exists(m_browser)) {
        if (reason) {
            *reason = QStringLiteral("Microsoft Edge / Chrome tidak ditemukan");
        }
        return false;
    }
    return true;
}

void EdgeMermaidRenderer::render(const QString &code) {
    const QString key = keyFor(code);

    const QString cached = cachePath(key);
    if (QFileInfo::exists(cached)) {
        QImage image(cached);
        if (!image.isNull()) {
            image.setDevicePixelRatio(kScale);
            QMetaObject::invokeMethod(this, [this, key, image]() { emit rendered(key, image); },
                                      Qt::QueuedConnection);
            return;
        }
    }

    if (m_currentKey == key) {
        return;
    }
    for (const auto &pending : std::as_const(m_queue)) {
        if (pending.first == key) {
            return;
        }
    }
    m_queue.append({key, code});
    // Selalu lewat event loop: hasil (termasuk gagal) tidak pernah dipancarkan di dalam render()
    QMetaObject::invokeMethod(this, &EdgeMermaidRenderer::startNext, Qt::QueuedConnection);
}

QImage EdgeMermaidRenderer::trimTransparent(const QImage &source, int margin) {
    const QImage image = source.convertToFormat(QImage::Format_ARGB32);
    int left = image.width();
    int right = -1;
    int top = image.height();
    int bottom = -1;
    for (int y = 0; y < image.height(); ++y) {
        const auto *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(line[x]) > 0) {
                left = qMin(left, x);
                right = qMax(right, x);
                top = qMin(top, y);
                bottom = qMax(bottom, y);
            }
        }
    }
    if (right < 0) {
        return QImage();
    }
    const QRect box = QRect(QPoint(left, top), QPoint(right, bottom))
                          .adjusted(-margin, -margin, margin, margin)
                          .intersected(image.rect());
    return image.copy(box);
}

QString EdgeMermaidRenderer::pageHtml(const QString &code) {
    // Satu kali arg() dengan dua nilai: isi kode tidak dipindai ulang sebagai penanda %1/%2.
    // Wadah selebar jendela (bukan inline-block): SVG Mermaid memakai width 100% + max-width
    // ukuran aslinya, dan di wadah shrink-to-fit lebarnya jatuh ke default 300px sehingga
    // diagram lebar ikut mengecil. Tepi kosong dipotong trimTransparent().
    return QStringLiteral(
               "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
               "<style>html,body{margin:0;padding:0;background:transparent}"
               "#diagram{display:block;width:1380px;padding:8px;box-sizing:border-box}</style>"
               "<script src=\"mermaid.min.js\"></script></head><body>"
               "<div id=\"diagram\"><pre class=\"mermaid\">%1</pre></div>"
               "<script>mermaid.initialize(%2);</script></body></html>")
        .arg(code.trimmed().toHtmlEscaped(), QString::fromLatin1(kMermaidConfig));
}

void EdgeMermaidRenderer::startNext() {
    if (m_process || m_queue.isEmpty()) {
        return;
    }
    const QPair<QString, QString> next = m_queue.takeFirst();
    m_currentKey = next.first;

    QString reason;
    if (!isAvailable(&reason)) {
        finishCurrent(QImage(), reason);
        return;
    }
    if (!ensureRuntimeFiles()) {
        finishCurrent(QImage(), QStringLiteral("mermaid.min.js tidak bisa disiapkan"));
        return;
    }

    const QString page = m_workDir.filePath(m_currentKey + QStringLiteral(".html"));
    QFile file(page);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        finishCurrent(QImage(), QStringLiteral("halaman render tidak bisa ditulis"));
        return;
    }
    file.write(pageHtml(next.second).toUtf8());
    file.close();

    const QString screenshot = m_workDir.filePath(m_currentKey + QStringLiteral(".png"));
    m_process = new QProcess(this);
    m_process->setProgram(m_browser);
    m_process->setArguments({
        QStringLiteral("--headless=new"),
        QStringLiteral("--disable-gpu"),
        QStringLiteral("--hide-scrollbars"),
        QStringLiteral("--no-first-run"),
        QStringLiteral("--no-default-browser-check"),
        QStringLiteral("--disable-extensions"),
        QStringLiteral("--user-data-dir=%1").arg(QDir::toNativeSeparators(m_workDir.filePath(QStringLiteral("profile")))),
        QStringLiteral("--force-device-scale-factor=2"),
        // Latar transparan: tepi kosong dipotong trimTransparent(), diagram menyatu dengan panel
        QStringLiteral("--default-background-color=00000000"),
        QStringLiteral("--window-size=1400,2400"),
        // Menunggu render Mermaid (asinkron) selesai sebelum screenshot diambil
        QStringLiteral("--virtual-time-budget=10000"),
        QStringLiteral("--screenshot=%1").arg(QDir::toNativeSeparators(screenshot)),
        QUrl::fromLocalFile(page).toString(),
    });
    connect(m_process, &QProcess::finished, this, &EdgeMermaidRenderer::onFinished);
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            finishCurrent(QImage(), QStringLiteral("browser gagal dijalankan: %1").arg(m_process->errorString()));
        }
    });
    m_timeout.start(kTimeoutMs);
    m_process->start();
}

void EdgeMermaidRenderer::onFinished() {
    const QString screenshot = m_workDir.filePath(m_currentKey + QStringLiteral(".png"));
    QImage image(screenshot);
    QString error;
    if (image.isNull()) {
        error = QStringLiteral("browser tidak menghasilkan gambar");
    } else {
        image = trimTransparent(image);
        if (image.isNull()) {
            error = QStringLiteral("diagram kosong; periksa sintaks Mermaid");
        }
    }

    if (error.isEmpty()) {
        QDir().mkpath(m_cacheDir);
        image.save(cachePath(m_currentKey), "PNG");
        image.setDevicePixelRatio(kScale);
    }
    QFile::remove(screenshot);
    QFile::remove(m_workDir.filePath(m_currentKey + QStringLiteral(".html")));
    finishCurrent(image, error);
}

void EdgeMermaidRenderer::finishCurrent(const QImage &image, const QString &error) {
    m_timeout.stop();
    const QString key = m_currentKey;
    m_currentKey.clear();
    if (m_process) {
        m_process->disconnect(this);
        m_process->deleteLater();
        m_process = nullptr;
    }

    if (error.isEmpty()) {
        emit rendered(key, image);
    } else {
        emit failed(key, error);
    }
    startNext();
}

QString EdgeMermaidRenderer::cachePath(const QString &key) const {
    return QStringLiteral("%1/%2.png").arg(m_cacheDir, key);
}

bool EdgeMermaidRenderer::ensureRuntimeFiles() {
    if (!m_workDir.isValid()) {
        return false;
    }
    const QString target = m_workDir.filePath(QStringLiteral("mermaid.min.js"));
    return QFileInfo::exists(target) || QFile::copy(QStringLiteral(":/vendor/mermaid.min.js"), target);
}

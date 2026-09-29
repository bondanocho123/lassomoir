#include "EdgeMermaidRenderer.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStringList>
#include <QUrl>
#include <QtMath>

#include <algorithm>
#include <cmath>

namespace {

constexpr int kTimeoutMs = 30000;
constexpr qreal kScale = 2.0;               // skala perangkat lintasan pertama, sekaligus batas atas
constexpr int kCanvasWidth = 1400;          // kanvas lintasan pertama (piksel CSS)
constexpr int kCanvasHeight = 2400;
constexpr int kPadding = 8;                 // tepi #diagram di halaman render (piksel CSS)
constexpr qreal kMaxPixels = 10000000.0;    // lintasan kedua: ±40 MB per diagram di memori
constexpr qreal kMaxSide = 16000.0;         // sisi screenshot terbesar (piksel perangkat)
constexpr int kMaxCanvas = 30000;           // diagram yang lebih besar tidak dirender ulang
constexpr char kRatioText[] = "LassomoirRatio";   // teks PNG di cache: devicePixelRatio gambar

// Tema "base" Mermaid dengan palet aplikasi (lihat styles.qss). startOnLoad mati: halaman
// menjalankan mermaid.run() sendiri supaya bisa mengukur hasilnya dan melaporkan gagal render.
constexpr char kMermaidConfig[] =
    "{startOnLoad: false, securityLevel: 'strict', theme: 'base', themeVariables: {"
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
            bool known = false;
            const qreal ratio = image.text(QLatin1String(kRatioText)).toDouble(&known);
            image.convertTo(QImage::Format_ARGB32_Premultiplied);
            image.setDevicePixelRatio(known && ratio > 0 ? ratio : kScale);
            QMetaObject::invokeMethod(this, [this, key, image]() { emit rendered(key, image); },
                                      Qt::QueuedConnection);
            return;
        }
    }

    if (m_current.key == key) {
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

QString EdgeMermaidRenderer::pageHtml(const QString &code, const QSize &canvas) {
    // Satu kali arg() dengan semua nilai: isi kode tidak dipindai ulang sebagai penanda %1..%5.
    // Wadah selebar kanvas (bukan inline-block): gantt memakai lebar wadahnya sebagai lebar diagram.
    // Skrip membaca ukuran asli SVG (viewBox), memperkecilnya bila tidak muat kanvas, lalu melapor
    // di #lassomoir-render untuk --dump-dom. Kanvas ditulis eksplisit karena viewport yang dilihat
    // skrip di headless lebih kecil dari --window-size (ruang UI browser), padahal screenshot
    // memotret seluas --window-size.
    return QStringLiteral(
               "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
               "<style>html,body{margin:0;padding:0;background:transparent}"
               "#diagram{display:block;width:%3px;padding:%5px;box-sizing:border-box}"
               "#diagram svg{display:block}</style>"
               "<script src=\"mermaid.min.js\"></script></head><body>"
               "<div id=\"diagram\"><pre class=\"mermaid\">%1</pre></div>"
               "<pre id=\"lassomoir-render\" hidden></pre>"
               "<script>"
               "function report(text){document.getElementById('lassomoir-render').textContent=text;}"
               "mermaid.initialize(%2);"
               "mermaid.run({querySelector:'.mermaid'}).then(function(){"
               "var svg=document.querySelector('#diagram svg');"
               "var box=svg.viewBox&&svg.viewBox.baseVal;var rect=svg.getBoundingClientRect();"
               "var w=box&&box.width?box.width:rect.width;var h=box&&box.height?box.height:rect.height;"
               "var fit=Math.min(1,(%3-2*%5)/w,(%4-2*%5)/h);"
               "svg.style.maxWidth='none';svg.setAttribute('width',w*fit);svg.setAttribute('height',h*fit);"
               "report('ok '+w+' '+h+' '+fit);"
               "}).catch(function(error){"
               "report('error '+encodeURIComponent(String(error&&error.message||error)));});"
               "</script></body></html>")
        .arg(code.trimmed().toHtmlEscaped(), QString::fromLatin1(kMermaidConfig), QString::number(canvas.width()),
             QString::number(canvas.height()), QString::number(kPadding));
}

EdgeMermaidRenderer::Report EdgeMermaidRenderer::readReport(const QByteArray &dom) {
    static const QRegularExpression marker(QStringLiteral(R"(<pre id="lassomoir-render"[^>]*>([^<]*)</pre>)"));
    static const QRegularExpression pointer(QStringLiteral(R"(^-*\^$)"));

    Report report;
    const QString text = marker.match(QString::fromUtf8(dom)).captured(1).trimmed();
    if (text.startsWith(QLatin1String("error "))) {
        // Pesan parse Mermaid berbaris-baris: posisi, potongan kode, penunjuk "----^", lalu token
        // yang diharapkan. Penunjuk tidak berarti di satu baris, jadi dibuang.
        QStringList parts;
        const QStringList lines = QUrl::fromPercentEncoding(text.mid(6).toUtf8()).split(QLatin1Char('\n'));
        for (const QString &line : lines) {
            const QString trimmed = line.trimmed();
            if (!trimmed.isEmpty() && !pointer.match(trimmed).hasMatch()) {
                parts << trimmed;
            }
        }
        report.error = parts.isEmpty() ? QStringLiteral("kode diagram tidak valid") : parts.join(QLatin1Char(' '));
        if (report.error.size() > 300) {
            report.error = report.error.left(299) + QChar(u'…');
        }
        return report;
    }

    const QStringList values = text.split(QLatin1Char(' '));
    if (values.size() == 4 && values.first() == QLatin1String("ok")) {
        bool widthOk = false;
        bool heightOk = false;
        bool fitOk = false;
        const qreal width = values.at(1).toDouble(&widthOk);
        const qreal height = values.at(2).toDouble(&heightOk);
        const qreal fit = values.at(3).toDouble(&fitOk);
        if (widthOk && heightOk && fitOk && width > 0 && height > 0 && fit > 0) {
            report.ok = true;
            report.natural = QSizeF(width, height);
            report.fit = qMin(fit, 1.0);
        }
    }
    return report;
}

EdgeMermaidRenderer::Pass EdgeMermaidRenderer::firstPass() {
    return Pass{QSize(kCanvasWidth, kCanvasHeight), kScale};
}

EdgeMermaidRenderer::Pass EdgeMermaidRenderer::detailPass(const QSizeF &natural) {
    Pass pass;
    // +2: cadangan pembulatan, supaya diagram muat tanpa diperkecil lagi
    pass.canvas = QSize(qCeil(natural.width()) + 2 * kPadding + 2, qCeil(natural.height()) + 2 * kPadding + 2);
    if (pass.canvas.width() > kMaxCanvas || pass.canvas.height() > kMaxCanvas) {
        return pass;
    }
    const qreal width = pass.canvas.width();
    const qreal height = pass.canvas.height();
    const qreal scale = std::min({kScale, std::sqrt(kMaxPixels / (width * height)), kMaxSide / width, kMaxSide / height});
    // Dua desimal: angka yang sama dipakai --force-device-scale-factor dan devicePixelRatio gambar
    pass.scale = std::floor(scale * 100.0) / 100.0;
    return pass;
}

void EdgeMermaidRenderer::startNext() {
    if (m_process || m_queue.isEmpty()) {
        return;
    }
    const QPair<QString, QString> next = m_queue.takeFirst();
    m_current = Job{next.first, next.second, firstPass(), QImage()};

    QString reason;
    if (!isAvailable(&reason)) {
        finishCurrent(QImage(), reason);
        return;
    }
    if (!ensureRuntimeFiles()) {
        finishCurrent(QImage(), QStringLiteral("mermaid.min.js tidak bisa disiapkan"));
        return;
    }
    launch();
}

void EdgeMermaidRenderer::launch() {
    const QString page = m_workDir.filePath(m_current.key + QStringLiteral(".html"));
    QFile file(page);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        finishCurrent(QImage(), QStringLiteral("halaman render tidak bisa ditulis"));
        return;
    }
    file.write(pageHtml(m_current.code, m_current.pass.canvas).toUtf8());
    file.close();

    const QString screenshot = m_workDir.filePath(m_current.key + QStringLiteral(".png"));
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
        QStringLiteral("--force-device-scale-factor=%1").arg(m_current.pass.scale),
        // Latar transparan: tepi kosong dipotong trimTransparent(), diagram menyatu dengan panel
        QStringLiteral("--default-background-color=00000000"),
        QStringLiteral("--window-size=%1,%2").arg(m_current.pass.canvas.width()).arg(m_current.pass.canvas.height()),
        // Menunggu render Mermaid (asinkron) selesai sebelum DOM dan screenshot diambil
        QStringLiteral("--virtual-time-budget=10000"),
        // Laporan ukuran diagram (stdout) dan screenshot dari satu proses
        QStringLiteral("--dump-dom"),
        QStringLiteral("--screenshot=%1").arg(QDir::toNativeSeparators(screenshot)),
        QUrl::fromLocalFile(page).toString(),
    });
    m_process->setStandardErrorFile(QProcess::nullDevice());   // log browser tidak dipakai
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
    const Report report = readReport(m_process->readAllStandardOutput());
    const QString screenshot = m_workDir.filePath(m_current.key + QStringLiteral(".png"));
    QImage image(screenshot);
    QFile::remove(screenshot);
    QFile::remove(m_workDir.filePath(m_current.key + QStringLiteral(".html")));

    QString error;
    if (!report.error.isEmpty()) {
        error = QStringLiteral("Mermaid: %1").arg(report.error);
    } else if (image.isNull()) {
        error = QStringLiteral("browser tidak menghasilkan gambar");
    } else {
        image = trimTransparent(image);
        if (image.isNull()) {
            error = QStringLiteral("diagram kosong; periksa sintaks Mermaid");
        }
    }
    if (!error.isEmpty()) {
        finishCurrent(QImage(), error);
        return;
    }

    // Kerapatan piksel = skala perangkat × skala diagram di kanvas, jadi ukuran logis gambar sama
    // dengan ukuran asli diagram (100% di penampil zoom). Tanpa laporan: fit 1, seperti dulu.
    const qreal ratio = m_current.pass.scale * report.fit;
    image.setDevicePixelRatio(ratio);
    if (report.ok && report.fit < 1.0 && m_current.fallback.isNull()) {
        // Diagram lebih besar dari kanvas pertama sehingga diperkecil: render lagi seukuran aslinya,
        // kecuali hasilnya nyaris tidak lebih tajam
        const Pass detail = detailPass(report.natural);
        if (detail.scale > ratio * 1.1) {
            m_current.fallback = image;
            m_current.pass = detail;
            m_timeout.stop();
            releaseProcess();
            launch();
            return;
        }
    }
    finishCurrent(image, QString());
}

void EdgeMermaidRenderer::finishCurrent(QImage image, const QString &error) {
    m_timeout.stop();
    releaseProcess();
    // Lintasan kedua gagal: pakai hasil lintasan pertama (diagram diperkecil agar muat kanvas)
    const bool useFallback = !error.isEmpty() && !m_current.fallback.isNull();
    if (useFallback) {
        image = m_current.fallback;
    }
    const QString key = m_current.key;
    m_current = Job();

    if (error.isEmpty() || useFallback) {
        // Kerapatan piksel ikut disimpan: gambar seukuran asli tidak selalu berskala 2
        const qreal ratio = image.devicePixelRatio();
        image.setText(QLatin1String(kRatioText), QString::number(ratio));
        QDir().mkpath(m_cacheDir);
        image.save(cachePath(key), "PNG");
        image.convertTo(QImage::Format_ARGB32_Premultiplied);
        image.setDevicePixelRatio(ratio);
        emit rendered(key, image);
    } else {
        emit failed(key, error);
    }
    startNext();
}

void EdgeMermaidRenderer::releaseProcess() {
    if (m_process) {
        m_process->disconnect(this);
        m_process->deleteLater();
        m_process = nullptr;
    }
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

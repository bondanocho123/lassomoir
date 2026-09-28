#include "TaskAttachments.h"
#include "DocumentText.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QPainter>
#include <QSaveFile>

namespace {

const QStringList kImageSuffixes = {
    QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"),
    QStringLiteral("gif"), QStringLiteral("bmp"), QStringLiteral("webp"),
};

// Nama lampiran dari session.json dipakai sebagai nama file di folder lampiran; jangan sampai
// menunjuk ke luar folder itu
bool isPlainName(const QString &name) {
    return !name.isEmpty() && name != QLatin1String(".") && name != QLatin1String("..")
           && !name.contains(QLatin1Char('/')) && !name.contains(QLatin1Char('\\'));
}

QByteArray encode(const QImage &image, const char *format, int quality) {
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    return image.save(&buffer, format, quality) ? bytes : QByteArray();
}

// JPEG tidak punya transparansi: latar transparan jadi putih, bukan hitam
QImage opaque(const QImage &image) {
    if (!image.hasAlphaChannel()) {
        return image.convertToFormat(QImage::Format_RGB32);
    }
    QImage result(image.size(), QImage::Format_RGB32);
    result.fill(Qt::white);
    QPainter painter(&result);
    painter.drawImage(0, 0, image);
    painter.end();
    return result;
}

QString sanitizedName(const QString &fileName) {
    // Karakter yang dilarang di nama file Windows, termasuk karakter kontrol
    static const QString forbidden = QStringLiteral("\\/:*?\"<>|");
    QString name = fileName;
    for (QChar &c : name) {
        if (c.unicode() < 32 || forbidden.contains(c)) {
            c = QLatin1Char('_');
        }
    }
    name = name.trimmed();
    while (name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' '))) {
        name.chop(1);
    }

    QFileInfo info(name);
    QString base = info.completeBaseName();
    const QString suffix = info.suffix().isEmpty() ? QString() : QLatin1Char('.') + info.suffix();
    static const QStringList reserved = {
        QStringLiteral("CON"), QStringLiteral("PRN"), QStringLiteral("AUX"), QStringLiteral("NUL"),
        QStringLiteral("COM1"), QStringLiteral("COM2"), QStringLiteral("COM3"), QStringLiteral("COM4"),
        QStringLiteral("LPT1"), QStringLiteral("LPT2"), QStringLiteral("LPT3"),
    };
    // Nama tanpa bagian dasar (mis. ".teks", nama folder teks lengkap dokumen) dan nama perangkat
    // Windows diberi awalan supaya tetap jadi file biasa
    if (base.isEmpty() || reserved.contains(base, Qt::CaseInsensitive)) {
        base.prepend(QStringLiteral("lampiran"));
    }
    // Path lengkap di AppData tetap di bawah batas 260 karakter Windows
    return base.left(100) + suffix.left(12);
}

bool writeText(const QString &path, const QString &text) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        return false;
    }
    QSaveFile file(path);
    const QByteArray data = text.toUtf8();
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size() && file.commit();
}

}

bool TaskAttachments::isImage(const QString &fileName) {
    return kImageSuffixes.contains(QFileInfo(fileName).suffix().toLower());
}

bool TaskAttachments::isDocument(const QString &fileName) {
    return DocumentText::isSupported(fileName);
}

QString TaskAttachments::imageFilter() {
    // Hanya format yang benar-benar bisa dibuka plugin gambar Qt yang terpasang (webp opsional)
    const QList<QByteArray> readable = QImageReader::supportedImageFormats();
    QStringList patterns;
    for (const QString &suffix : kImageSuffixes) {
        if (readable.contains(suffix.toLatin1())) {
            patterns.append(QStringLiteral("*.") + suffix);
        }
    }
    return QStringLiteral("Foto (%1)").arg(patterns.join(QLatin1Char(' ')));
}

QString TaskAttachments::documentFilter() {
    QStringList patterns;
    for (const QString &suffix : DocumentText::supportedSuffixes()) {
        patterns.append(QStringLiteral("*.") + suffix);
    }
    return QStringLiteral("Excel, Word, CSV (%1)").arg(patterns.join(QLatin1Char(' ')));
}

qint64 TaskAttachments::Draft::size() const {
    if (!imageData.isEmpty()) {
        return imageData.size();
    }
    return QFileInfo(isStored() ? storedPath : sourcePath).size();
}

TaskAttachments::NormalizedImage TaskAttachments::normalizeImage(const QImage &image, bool photo) {
    NormalizedImage result;
    if (image.isNull()) {
        result.error = QStringLiteral("Gambar kosong atau tidak terbaca");
        return result;
    }
    QImage scaled = image;
    if (qMax(image.width(), image.height()) > kMaxImageSide) {
        scaled = image.scaled(kMaxImageSide, kMaxImageSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }

    // Tangkapan layar & diagram tetap tajam sebagai PNG; foto kamera lebih kecil sebagai JPEG
    if (!photo) {
        const QByteArray png = encode(scaled, "PNG", -1);
        if (!png.isEmpty() && png.size() <= kMaxImageBytes) {
            result.data = png;
            result.suffix = QStringLiteral("png");
            result.size = scaled.size();
            return result;
        }
    }

    QImage flat = opaque(scaled);
    int quality = 90;
    for (int attempt = 0; attempt < 16; ++attempt) {
        const QByteArray jpeg = encode(flat, "JPEG", quality);
        if (jpeg.isEmpty()) {
            result.error = QStringLiteral("Gambar tidak bisa disimpan (plugin JPEG tidak tersedia)");
            return result;
        }
        if (jpeg.size() <= kMaxImageBytes) {
            result.data = jpeg;
            result.suffix = QStringLiteral("jpg");
            result.size = flat.size();
            return result;
        }
        // Turunkan kualitas dulu, baru resolusinya
        if (quality > 70) {
            quality -= 10;
        } else {
            flat = flat.scaled(flat.size() * 0.75, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
    }
    result.error = QStringLiteral("Gambar terlalu besar");
    return result;
}

TaskAttachments::NormalizedImage TaskAttachments::normalizeImageFile(const QString &path) {
    QImageReader reader(path);
    reader.setAutoTransform(true);   // foto ponsel: rotasi dari EXIF
    const QByteArray format = reader.format().toLower();
    const QSize size = reader.size();

    // PNG/JPEG yang sudah memenuhi batas disimpan apa adanya, tanpa kompresi ulang
    const bool keep = (format == "png" || format == "jpeg")
                      && reader.transformation() == QImageIOHandler::TransformationNone
                      && size.isValid() && qMax(size.width(), size.height()) <= kMaxImageSide
                      && QFileInfo(path).size() <= kMaxImageBytes;
    if (keep) {
        QFile file(path);
        if (file.open(QIODevice::ReadOnly)) {
            NormalizedImage result;
            result.data = file.readAll();
            result.suffix = format == "png" ? QStringLiteral("png") : QStringLiteral("jpg");
            result.size = size;
            return result;
        }
    }

    const QImage image = reader.read();
    if (image.isNull()) {
        NormalizedImage result;
        result.error = QStringLiteral("Bukan gambar yang bisa dibuka (%1)").arg(reader.errorString());
        return result;
    }
    return normalizeImage(image, format == "jpeg");
}

QString TaskAttachments::uniqueName(const QString &fileName, const QStringList &taken) {
    const QString clean = sanitizedName(fileName);
    if (!taken.contains(clean, Qt::CaseInsensitive)) {
        return clean;
    }
    const QFileInfo info(clean);
    const QString suffix = info.suffix().isEmpty() ? QString() : QLatin1Char('.') + info.suffix();
    for (int n = 2;; ++n) {
        const QString candidate = QStringLiteral("%1 (%2)%3").arg(info.completeBaseName()).arg(n).arg(suffix);
        if (!taken.contains(candidate, Qt::CaseInsensitive)) {
            return candidate;
        }
    }
}

QStringList TaskAttachments::save(const QString &directory, const QList<Draft> &drafts, const QStringList &previous,
                                  QStringList *errors) {
    // QDir("") adalah folder kerja proses: jangan pernah menghapus atau menulis di sana
    if (directory.isEmpty()) {
        if (errors && !drafts.isEmpty()) {
            errors->append(QStringLiteral("folder lampiran tidak diketahui"));
        }
        return QStringList();
    }
    const QDir dir(directory);

    QStringList kept;
    for (const Draft &draft : drafts) {
        if (draft.isStored()) {
            kept.append(QFileInfo(draft.storedPath).fileName());
        }
    }
    for (const QString &name : previous) {
        if (isPlainName(name) && !kept.contains(name, Qt::CaseInsensitive)) {
            QFile::remove(dir.filePath(name));
        }
    }
    // Teks lengkap dokumen dibuat ulang pada run berikutnya
    QDir(dir.filePath(QStringLiteral(".teks"))).removeRecursively();

    QStringList saved;
    QStringList taken = kept + dir.entryList(QDir::Files);
    for (const Draft &draft : drafts) {
        if (draft.isStored()) {
            saved.append(QFileInfo(draft.storedPath).fileName());
            continue;
        }
        if (!dir.mkpath(QStringLiteral("."))) {
            if (errors) {
                errors->append(QStringLiteral("folder lampiran tidak bisa dibuat: %1").arg(directory));
            }
            break;
        }
        const QString name = uniqueName(draft.fileName, taken);
        const QString target = dir.filePath(name);
        bool written = false;
        if (!draft.imageData.isEmpty()) {
            QSaveFile file(target);
            written = file.open(QIODevice::WriteOnly) && file.write(draft.imageData) == draft.imageData.size()
                      && file.commit();
        } else {
            written = QFile::copy(draft.sourcePath, target);
        }
        if (!written) {
            if (errors) {
                errors->append(QStringLiteral("%1 gagal disimpan").arg(draft.fileName));
            }
            continue;
        }
        taken.append(name);
        saved.append(name);
    }

    // Task tanpa lampiran tidak meninggalkan folder kosong (rmdir gagal bila masih ada isinya)
    if (saved.isEmpty()) {
        QDir().rmdir(dir.absolutePath());
    }
    return saved;
}

TaskMaterials TaskAttachments::materials(const TaskItem &task, const QString &attachmentDirectory,
                                         const QStringList &referenceDirectories) {
    TaskMaterials result;
    for (const QString &directory : referenceDirectories) {
        if (QFileInfo(directory).isDir()) {
            result.referenceDirectories.append(QDir::toNativeSeparators(QDir::cleanPath(directory)));
        } else {
            result.warnings.append(QStringLiteral("folder referensi tidak ditemukan, dilewati: %1")
                                       .arg(QDir::toNativeSeparators(directory)));
        }
    }
    if (task.attachments.isEmpty() || attachmentDirectory.isEmpty()) {
        return result;
    }

    const QDir dir(attachmentDirectory);
    qsizetype budget = kDocumentsPromptChars;
    for (const QString &name : task.attachments) {
        const QString path = dir.filePath(name);
        if (!isPlainName(name) || !QFileInfo(path).isFile()) {
            result.warnings.append(QStringLiteral("lampiran %1 tidak ditemukan, dilewati").arg(name));
            continue;
        }
        if (isImage(name)) {
            result.imagePaths.append(QDir::toNativeSeparators(path));
            continue;
        }

        TaskMaterials::Document document;
        document.fileName = name;
        document.path = QDir::toNativeSeparators(path);
        QString error;
        const QString text = DocumentText::extract(path, &error);
        if (!error.isEmpty()) {
            document.error = error;
            result.warnings.append(QStringLiteral("lampiran %1 tidak bisa dibaca: %2").arg(name, error));
        } else {
            document.text = text;
            const qsizetype limit = qMin(kDocumentPromptChars, budget);
            if (text.size() > limit) {
                // Dipotong di akhir baris supaya baris CSV atau tabel tidak terbelah
                const qsizetype lineEnd = text.lastIndexOf(QLatin1Char('\n'), limit);
                document.text = text.left(lineEnd > 0 ? lineEnd : limit);
                document.omittedChars = text.size() - document.text.size();
                const QString fullPath = dir.filePath(QStringLiteral(".teks/%1.txt").arg(name));
                if (writeText(fullPath, text)) {
                    document.fullTextPath = QDir::toNativeSeparators(fullPath);
                }
            }
            budget -= document.text.size();
        }
        result.documents.append(document);
    }
    if (!result.imagePaths.isEmpty() || !result.documents.isEmpty()) {
        result.attachmentDirectory = QDir::toNativeSeparators(dir.absolutePath());
    }
    return result;
}

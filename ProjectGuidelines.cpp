#include "ProjectGuidelines.h"
#include "DocumentText.h"
#include "TaskAttachments.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>

namespace {

bool fail(QString *error, const QString &why) {
    if (error) {
        *error = why;
    }
    return false;
}

// Nama dari pemanggil dipakai sebagai nama file di folder pedoman; jangan sampai menunjuk ke luar
bool isPlainName(const QString &name) {
    return !name.isEmpty() && name != QLatin1String(".") && name != QLatin1String("..")
           && !name.contains(QLatin1Char('/')) && !name.contains(QLatin1Char('\\'));
}

// PDF asli diawali "%PDF-" (sebagian pembuat PDF menaruh sampah sebelum itu, masih dalam 1 KB pertama)
bool checkPdf(const QString &path, QString *error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return fail(error, QStringLiteral("File tidak bisa dibuka: %1").arg(file.errorString()));
    }
    if (!file.read(1024).contains("%PDF-")) {
        return fail(error, QStringLiteral("Bukan file PDF yang valid"));
    }
    return true;
}

bool check(const QString &path, QString *error) {
    const QFileInfo info(path);
    if (!ProjectGuidelines::isSupported(info.fileName())) {
        // Alasan untuk .doc/.xls lama datang dari DocumentText
        QString reason;
        DocumentText::check(path, &reason);
        if (reason.startsWith(QStringLiteral("Format lama"))) {
            return fail(error, reason);
        }
        return fail(error, QStringLiteral("Hanya Markdown (.md), teks (.txt), Word (.docx), PDF, Excel (.xlsx), "
                                          "dan CSV yang bisa jadi pedoman"));
    }
    if (!info.isFile()) {
        return fail(error, QStringLiteral("File tidak ditemukan"));
    }
    if (info.size() > ProjectGuidelines::kMaxFileBytes) {
        return fail(error, QStringLiteral("File lebih besar dari %1")
                               .arg(QLocale().formattedDataSize(ProjectGuidelines::kMaxFileBytes)));
    }
    return ProjectGuidelines::isPdf(path) ? checkPdf(path, error) : DocumentText::check(path, error);
}

}

bool ProjectGuidelines::isPdf(const QString &fileName) {
    return QFileInfo(fileName).suffix().compare(QLatin1String("pdf"), Qt::CaseInsensitive) == 0;
}

bool ProjectGuidelines::isSupported(const QString &fileName) {
    return isPdf(fileName) || DocumentText::isSupported(fileName);
}

QString ProjectGuidelines::fileFilter() {
    QStringList patterns = {QStringLiteral("*.pdf")};
    for (const QString &suffix : DocumentText::supportedSuffixes()) {
        patterns.append(QStringLiteral("*.") + suffix);
    }
    return QStringLiteral("Dokumen pedoman (%1)").arg(patterns.join(QLatin1Char(' ')));
}

QStringList ProjectGuidelines::list(const QString &directory) {
    // QDir("") adalah folder kerja proses
    if (directory.isEmpty()) {
        return QStringList();
    }
    QStringList names;
    const QStringList entries = QDir(directory).entryList(QDir::Files, QDir::Name | QDir::IgnoreCase);
    for (const QString &name : entries) {
        if (isSupported(name)) {
            names.append(name);
        }
    }
    return names;
}

QString ProjectGuidelines::add(const QString &directory, const QString &sourcePath, QString *error) {
    if (directory.isEmpty()) {
        fail(error, QStringLiteral("Folder pedoman tidak diketahui"));
        return QString();
    }
    if (!check(sourcePath, error)) {
        return QString();
    }
    const QDir dir(directory);
    if (!dir.mkpath(QStringLiteral("."))) {
        fail(error, QStringLiteral("Folder pedoman tidak bisa dibuat: %1").arg(QDir::toNativeSeparators(directory)));
        return QString();
    }

    // Nama yang sama (tanpa beda huruf besar/kecil) menggantikan pedoman lama
    const QString name = TaskAttachments::uniqueName(QFileInfo(sourcePath).fileName(), QStringList());
    QString target = dir.filePath(name);
    for (const QString &existing : list(directory)) {
        if (existing.compare(name, Qt::CaseInsensitive) == 0) {
            target = dir.filePath(existing);
        }
    }
    if (QFileInfo(target).canonicalFilePath() == QFileInfo(sourcePath).canonicalFilePath()) {
        return QFileInfo(target).fileName();   // file di folder pedoman itu sendiri
    }

    // Salin dulu ke file sementara supaya pedoman lama tetap utuh bila penyalinan gagal
    const QString partial = target + QStringLiteral(".baru");
    QFile::remove(partial);
    if (!QFile::copy(sourcePath, partial)) {
        fail(error, QStringLiteral("File gagal disalin ke folder pedoman"));
        return QString();
    }
    QFile::setPermissions(partial, QFile::permissions(partial) | QFileDevice::WriteOwner);
    if (QFileInfo::exists(target) && !QFile::remove(target)) {
        QFile::remove(partial);
        fail(error, QStringLiteral("Pedoman lama %1 tidak bisa diganti (mungkin sedang dibuka)")
                        .arg(QFileInfo(target).fileName()));
        return QString();
    }
    if (!QFile::rename(partial, target)) {
        QFile::remove(partial);
        fail(error, QStringLiteral("File gagal disimpan di folder pedoman"));
        return QString();
    }
    // Teks lengkap lama (bila dulu dipotong) dibuat ulang pada run berikutnya
    QFile::remove(dir.filePath(QStringLiteral(".teks/%1.txt").arg(QFileInfo(target).fileName())));
    return QFileInfo(target).fileName();
}

bool ProjectGuidelines::remove(const QString &directory, const QString &fileName, QString *error) {
    if (directory.isEmpty() || !isPlainName(fileName)) {
        return fail(error, QStringLiteral("Nama pedoman tidak valid"));
    }
    const QDir dir(directory);
    const QString path = dir.filePath(fileName);
    if (QFileInfo::exists(path) && !QFile::remove(path)) {
        return fail(error, QStringLiteral("%1 tidak bisa dihapus (mungkin sedang dibuka)").arg(fileName));
    }
    QFile::remove(dir.filePath(QStringLiteral(".teks/%1.txt").arg(fileName)));
    // Project tanpa pedoman tidak meninggalkan folder kosong (rmdir gagal bila masih ada isinya)
    if (list(directory).isEmpty()) {
        QDir(dir.filePath(QStringLiteral(".teks"))).removeRecursively();
        QDir().rmdir(dir.absolutePath());
    }
    return true;
}

void ProjectGuidelines::addTo(TaskMaterials *materials, const QString &directory) {
    const QStringList names = list(directory);
    if (names.isEmpty()) {
        return;
    }
    const QDir dir(directory);
    qsizetype budget = kTotalPromptChars;
    for (const QString &name : names) {
        if (isPdf(name)) {
            TaskMaterials::Document document;
            document.fileName = name;
            document.path = QDir::toNativeSeparators(dir.filePath(name));
            document.readByAgent = true;
            materials->guidelines.append(document);
            continue;
        }
        const TaskMaterials::Document document =
            TaskAttachments::readDocument(dir, name, qMin(kDocumentPromptChars, budget));
        if (!document.error.isEmpty()) {
            materials->warnings.append(QStringLiteral("pedoman %1 tidak bisa dibaca: %2").arg(name, document.error));
        }
        budget -= document.text.size();
        materials->guidelines.append(document);
    }
    materials->guidelineDirectory = QDir::toNativeSeparators(dir.absolutePath());
}

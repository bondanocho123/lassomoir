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

// Kata untuk pesan ke pengguna: pedoman project dan konteks project memakai aturan file yang sama
struct Kind {
    QString noun;          // "pedoman" / "konteks", di tengah kalimat
    QString capitalNoun;   // awal kalimat
    QString filterLabel;   // QFileDialog
};

const Kind &guidelineKind() {
    static const Kind kind{QStringLiteral("pedoman"), QStringLiteral("Pedoman"), QStringLiteral("Dokumen pedoman")};
    return kind;
}

const Kind &contextKind() {
    static const Kind kind{QStringLiteral("konteks"), QStringLiteral("Konteks"), QStringLiteral("Dokumen konteks")};
    return kind;
}

// Nama dari pemanggil dipakai sebagai nama file di folder dokumen; jangan sampai menunjuk ke luar
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

bool check(const Kind &kind, const QString &path, QString *error) {
    const QFileInfo info(path);
    if (!ProjectGuidelines::isSupported(info.fileName())) {
        // Alasan untuk .doc/.xls lama datang dari DocumentText
        QString reason;
        DocumentText::check(path, &reason);
        if (reason.startsWith(QStringLiteral("Format lama"))) {
            return fail(error, reason);
        }
        return fail(error, QStringLiteral("Hanya Markdown (.md), teks (.txt), Word (.docx), PDF, Excel (.xlsx), "
                                          "dan CSV yang bisa jadi %1").arg(kind.noun));
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

QString filterFor(const Kind &kind) {
    QStringList patterns = {QStringLiteral("*.pdf")};
    for (const QString &suffix : DocumentText::supportedSuffixes()) {
        patterns.append(QStringLiteral("*.") + suffix);
    }
    return QStringLiteral("%1 (%2)").arg(kind.filterLabel, patterns.join(QLatin1Char(' ')));
}

QString addDocument(const Kind &kind, const QString &directory, const QString &sourcePath, QString *error) {
    if (directory.isEmpty()) {
        fail(error, QStringLiteral("Folder %1 tidak diketahui").arg(kind.noun));
        return QString();
    }
    if (!check(kind, sourcePath, error)) {
        return QString();
    }
    const QDir dir(directory);
    if (!dir.mkpath(QStringLiteral("."))) {
        fail(error, QStringLiteral("Folder %1 tidak bisa dibuat: %2").arg(kind.noun, QDir::toNativeSeparators(directory)));
        return QString();
    }

    // Nama yang sama (tanpa beda huruf besar/kecil) menggantikan dokumen lama
    const QString name = TaskAttachments::uniqueName(QFileInfo(sourcePath).fileName(), QStringList());
    QString target = dir.filePath(name);
    for (const QString &existing : ProjectGuidelines::list(directory)) {
        if (existing.compare(name, Qt::CaseInsensitive) == 0) {
            target = dir.filePath(existing);
        }
    }
    if (QFileInfo(target).canonicalFilePath() == QFileInfo(sourcePath).canonicalFilePath()) {
        return QFileInfo(target).fileName();   // file di folder dokumen itu sendiri
    }

    // Salin dulu ke file sementara supaya dokumen lama tetap utuh bila penyalinan gagal
    const QString partial = target + QStringLiteral(".baru");
    QFile::remove(partial);
    if (!QFile::copy(sourcePath, partial)) {
        fail(error, QStringLiteral("File gagal disalin ke folder %1").arg(kind.noun));
        return QString();
    }
    QFile::setPermissions(partial, QFile::permissions(partial) | QFileDevice::WriteOwner);
    if (QFileInfo::exists(target) && !QFile::remove(target)) {
        QFile::remove(partial);
        fail(error, QStringLiteral("%1 lama %2 tidak bisa diganti (mungkin sedang dibuka)")
                        .arg(kind.capitalNoun, QFileInfo(target).fileName()));
        return QString();
    }
    if (!QFile::rename(partial, target)) {
        QFile::remove(partial);
        fail(error, QStringLiteral("File gagal disimpan di folder %1").arg(kind.noun));
        return QString();
    }
    // Teks lengkap lama (bila dulu dipotong) dibuat ulang pada run berikutnya
    QFile::remove(dir.filePath(QStringLiteral(".teks/%1.txt").arg(QFileInfo(target).fileName())));
    return QFileInfo(target).fileName();
}

bool removeDocument(const Kind &kind, const QString &directory, const QString &fileName, QString *error) {
    if (directory.isEmpty() || !isPlainName(fileName)) {
        return fail(error, QStringLiteral("Nama %1 tidak valid").arg(kind.noun));
    }
    const QDir dir(directory);
    const QString path = dir.filePath(fileName);
    if (QFileInfo::exists(path) && !QFile::remove(path)) {
        return fail(error, QStringLiteral("%1 tidak bisa dihapus (mungkin sedang dibuka)").arg(fileName));
    }
    QFile::remove(dir.filePath(QStringLiteral(".teks/%1.txt").arg(fileName)));
    // Project tanpa dokumen tidak meninggalkan folder kosong (rmdir gagal bila masih ada isinya)
    if (ProjectGuidelines::list(directory).isEmpty()) {
        QDir(dir.filePath(QStringLiteral(".teks"))).removeRecursively();
        QDir().rmdir(dir.absolutePath());
    }
    return true;
}

// Baca dokumen di folder ke documents (PDF hanya disebut path-nya); kembalikan folder itu (native)
// bila ada dokumennya, atau kosong
QString readDocuments(const Kind &kind, TaskMaterials *materials, QList<TaskMaterials::Document> *documents,
                      const QString &directory, qsizetype documentChars, qsizetype totalChars) {
    const QStringList names = ProjectGuidelines::list(directory);
    if (names.isEmpty()) {
        return QString();
    }
    const QDir dir(directory);
    qsizetype budget = totalChars;
    for (const QString &name : names) {
        if (ProjectGuidelines::isPdf(name)) {
            TaskMaterials::Document document;
            document.fileName = name;
            document.path = QDir::toNativeSeparators(dir.filePath(name));
            document.readByAgent = true;
            documents->append(document);
            continue;
        }
        const TaskMaterials::Document document =
            TaskAttachments::readDocument(dir, name, qMin(documentChars, budget));
        if (!document.error.isEmpty()) {
            materials->warnings.append(QStringLiteral("%1 %2 tidak bisa dibaca: %3").arg(kind.noun, name, document.error));
        }
        budget -= document.text.size();
        documents->append(document);
    }
    return QDir::toNativeSeparators(dir.absolutePath());
}

}

bool ProjectGuidelines::isPdf(const QString &fileName) {
    return QFileInfo(fileName).suffix().compare(QLatin1String("pdf"), Qt::CaseInsensitive) == 0;
}

bool ProjectGuidelines::isSupported(const QString &fileName) {
    return isPdf(fileName) || DocumentText::isSupported(fileName);
}

QString ProjectGuidelines::fileFilter() {
    return filterFor(guidelineKind());
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
    return addDocument(guidelineKind(), directory, sourcePath, error);
}

bool ProjectGuidelines::remove(const QString &directory, const QString &fileName, QString *error) {
    return removeDocument(guidelineKind(), directory, fileName, error);
}

void ProjectGuidelines::addTo(TaskMaterials *materials, const QString &directory) {
    const QString dir = readDocuments(guidelineKind(), materials, &materials->guidelines, directory,
                                      kDocumentPromptChars, kTotalPromptChars);
    if (!dir.isEmpty()) {
        materials->guidelineDirectory = dir;
    }
}

QString ProjectContext::fileFilter() {
    return filterFor(contextKind());
}

QString ProjectContext::add(const QString &directory, const QString &sourcePath, QString *error) {
    return addDocument(contextKind(), directory, sourcePath, error);
}

bool ProjectContext::remove(const QString &directory, const QString &fileName, QString *error) {
    return removeDocument(contextKind(), directory, fileName, error);
}

void ProjectContext::addTo(TaskMaterials *materials, const QString &directory) {
    const QString dir = readDocuments(contextKind(), materials, &materials->contextDocuments, directory,
                                      kDocumentPromptChars, kTotalPromptChars);
    if (!dir.isEmpty()) {
        materials->contextDirectory = dir;
    }
}

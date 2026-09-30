#include "WorkspaceDiff.h"
#include "GitProcess.h"

#include <QDir>
#include <QFile>
#include <QHash>
#include <QLocale>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>

namespace {

constexpr qint64 kMaxUntrackedFileBytes = 256 * 1024;
constexpr qint64 kMaxUntrackedTotalBytes = 2 * 1024 * 1024;
constexpr int kMaxUntrackedFiles = 300;
constexpr qint64 kMaxMeasuredFileBytes = 512 * 1024;
constexpr int kMaxScannedSourceFiles = 5000;

// Pohon kosong git: pembanding saat repository belum punya commit (semua file tampil sebagai baru)
constexpr char kEmptyTree[] = "4b825dc642cb6eb9a060e54bf8d69288fbee4904";

// `git diff` untuk GitDiff::parse. --relative: hanya isi folder kerja (bisa subfolder repository)
// dengan path relatif terhadapnya. Prefix, warna, dan diff eksternal dipaksa agar konfigurasi git
// pengguna tidak mengubah format.
QStringList diffArguments(const QStringList &revisions) {
    return QStringList{
        QStringLiteral("-c"), QStringLiteral("core.quotepath=off"),
        QStringLiteral("diff"), QStringLiteral("--relative"), QStringLiteral("--find-renames"),
        QStringLiteral("--no-color"), QStringLiteral("--no-ext-diff"),
        QStringLiteral("--src-prefix=a/"), QStringLiteral("--dst-prefix=b/"),
    } + revisions + QStringList{QStringLiteral("--")};
}

void sortByPath(QList<FileDiff> *files) {
    std::sort(files->begin(), files->end(), [](const FileDiff &a, const FileDiff &b) {
        return QString::compare(a.path, b.path, Qt::CaseInsensitive) < 0;
    });
}

// Hash lengkap commit yang ditunjuk revisi; kosong + *error bila tidak ada atau git gagal
QString resolveCommit(const QString &git, const QString &directory, const QString &revision, QString *error) {
    const GitOutput commit = GitProcess::run(git, directory, {QStringLiteral("rev-parse"), QStringLiteral("-q"),
                                                              QStringLiteral("--verify"), revision + QStringLiteral("^{commit}")});
    if (commit.ok()) {
        return commit.text();
    }
    *error = commit.finished && commit.exitCode == 1
                 ? QStringLiteral("Commit %1 tidak ada di repository.").arg(revision)
                 : GitProcess::describeFailure(commit);
    return QString();
}

// "a/<path> b/<path>" dari baris "diff --git". Kedua nama sama kecuali rename (namanya diambil
// dari "rename from/to"), jadi path dibelah di tengah dan spasi di dalam path tetap aman.
QString pathFromGitHeader(const QString &names) {
    const qsizetype length = (names.size() - 5) / 2;
    if (length > 0 && names.startsWith(QLatin1String("a/"))
        && names.mid(2 + length, 3) == QLatin1String(" b/")
        && names.mid(2, length) == names.mid(5 + length)) {
        return names.mid(2, length);
    }
    const qsizetype split = names.lastIndexOf(QLatin1String(" b/"));
    return split >= 0 ? names.mid(split + 3) : names;
}

FileDiff untrackedFile(const QDir &root, const QString &path, qint64 *budget) {
    FileDiff file;
    file.status = FileDiff::Status::Added;
    file.untracked = true;
    file.path = path;

    QFile source(root.filePath(path));
    const qint64 size = source.size();
    if (size == 0) {
        file.note = QStringLiteral("File kosong");
        return file;
    }
    if (size > kMaxUntrackedFileBytes || size > *budget) {
        file.note = QStringLiteral("File terlalu besar untuk ditampilkan (%1)")
                        .arg(QLocale().formattedDataSize(size, 1, QLocale::DataSizeTraditionalFormat));
        return file;
    }
    if (!source.open(QIODevice::ReadOnly)) {
        file.note = QStringLiteral("File tidak bisa dibaca: %1").arg(source.errorString());
        return file;
    }
    const QByteArray bytes = source.readAll();
    *budget -= bytes.size();
    // Heuristik yang sama dengan git: ada byte NUL di awal file = biner
    if (bytes.left(8000).contains('\0')) {
        file.note = QStringLiteral("File biner — isi tidak ditampilkan");
        return file;
    }

    QStringList lines = QString::fromUtf8(bytes).split(QLatin1Char('\n'));
    if (lines.last().isEmpty()) {
        lines.removeLast();   // newline penutup file bukan baris baru
    }
    for (QString &line : lines) {
        if (line.endsWith(QLatin1Char('\r'))) {
            line.chop(1);
        }
        file.lines.append({DiffLine::Kind::Added, line, 0, ++file.added});
    }
    return file;
}

QString basePath(const FileDiff &file) {
    return file.status == FileDiff::Status::Renamed ? file.oldPath : file.path;
}

// Isi file di commit dasar lewat satu proses `git cat-file --batch`. Kunci = path relatif
// folder kerja ("<commit>:./path" dibaca git relatif terhadap folder kerja).
QHash<QString, QByteArray> readBaseFiles(const QString &git, const QString &directory, const QString &base,
                                         const QStringList &paths) {
    QHash<QString, QByteArray> files;
    if (paths.isEmpty()) {
        return files;
    }
    QByteArray request;
    for (const QString &path : paths) {
        request += (base + QStringLiteral(":./") + path).toUtf8() + '\n';
    }
    // Gagal di sini hanya menghilangkan metrik "sebelum"; diff tetap tampil
    const GitOutput output = GitProcess::run(git, directory, {QStringLiteral("cat-file"), QStringLiteral("--batch")}, request);
    if (!output.finished || output.exitCode != 0) {
        return files;
    }

    // Per permintaan: "<oid> <tipe> <ukuran>\n<isi>\n", atau "<nama> missing\n"
    qsizetype pos = 0;
    for (const QString &path : paths) {
        const qsizetype headerEnd = output.out.indexOf('\n', pos);
        if (headerEnd < 0) {
            break;
        }
        const QByteArray header = output.out.mid(pos, headerEnd - pos);
        pos = headerEnd + 1;
        if (header.endsWith(" missing") || header.endsWith(" ambiguous")) {
            continue;
        }
        const QList<QByteArray> parts = header.split(' ');
        if (parts.size() != 3) {
            break;
        }
        const qsizetype size = parts.at(2).toLongLong();
        if (parts.at(1) == "blob") {
            files.insert(path, output.out.mid(pos, size));
        }
        pos += size + 1;
    }
    return files;
}

std::optional<CodeMetrics> measureSource(const QString &path, const QByteArray &bytes) {
    if (bytes.size() > kMaxMeasuredFileBytes || bytes.left(8000).contains('\0')) {
        return std::nullopt;
    }
    return CodeMetrics::measure(path, QString::fromUtf8(bytes));
}

// Metrik sebelum (commit dasar) dan sesudah (folder kerja) untuk tiap file kode sumber.
// base kosong = repository belum punya commit, jadi tidak ada sisi "sebelum".
void measureFiles(const QString &git, const QString &directory, const QString &base, QList<FileDiff> *files) {
    QStringList basePaths;
    for (const FileDiff &file : std::as_const(*files)) {
        if (!base.isEmpty() && file.status != FileDiff::Status::Added && CodeMetrics::supports(file.path)) {
            basePaths.append(basePath(file));
        }
    }
    const QHash<QString, QByteArray> baseFiles = readBaseFiles(git, directory, base, basePaths);

    const QDir root(directory);
    for (FileDiff &file : *files) {
        if (!CodeMetrics::supports(file.path)) {
            continue;
        }
        const auto before = baseFiles.constFind(basePath(file));
        if (before != baseFiles.cend()) {
            file.before = measureSource(file.path, *before);
        }
        if (file.status != FileDiff::Status::Deleted) {
            QFile source(root.filePath(file.path));
            if (source.size() <= kMaxMeasuredFileBytes && source.open(QIODevice::ReadOnly)) {
                file.after = measureSource(file.path, source.readAll());
            }
        }
    }
}

// DIT C# butuh kelas dasar dari seluruh project, bukan hanya file yang berubah. File tracked
// dibaca dari folder kerja (sudah versi sesudah); file baru yang belum di-git add dari hasil ukurnya.
QHash<QString, QString> resolveCSharpInheritance(const QString &git, const QString &directory,
                                                 QList<FileDiff> *files) {
    const bool anyTypes = std::any_of(files->cbegin(), files->cend(), [](const FileDiff &file) {
        return file.after && !file.after->types.isEmpty();
    });
    if (!anyTypes) {
        return {};
    }

    QHash<QString, QString> bases;
    auto merge = [&bases](const QString &name, const QString &base) {
        // Kelas partial: base list cukup ada di salah satu bagiannya
        if (bases.value(name).isEmpty()) {
            bases.insert(name, base);
        }
    };
    const GitOutput tracked = GitProcess::run(git, directory, {QStringLiteral("ls-files"), QStringLiteral("-z"),
                                                               QStringLiteral("--"), QStringLiteral("*.cs")});
    if (tracked.finished && tracked.exitCode == 0) {
        const QDir root(directory);
        int scanned = 0;
        for (const QByteArray &path : tracked.out.split('\0')) {
            if (path.isEmpty()) {
                continue;
            }
            if (scanned == kMaxScannedSourceFiles) {
                break;
            }
            QFile source(root.filePath(QString::fromUtf8(path)));
            if (source.size() > kMaxMeasuredFileBytes || !source.open(QIODevice::ReadOnly)) {
                continue;
            }
            ++scanned;
            const QHash<QString, QString> declared = CSharpMetrics::declaredBases(QString::fromUtf8(source.readAll()));
            for (auto it = declared.cbegin(); it != declared.cend(); ++it) {
                merge(it.key(), it.value());
            }
        }
    }
    for (const FileDiff &file : std::as_const(*files)) {
        if (file.after) {
            for (const TypeMetrics &type : file.after->types) {
                merge(type.name.section(QLatin1Char('.'), -1), type.baseClass);
            }
        }
    }
    for (FileDiff &file : *files) {
        if (file.after) {
            CSharpMetrics::resolveInheritance(&file.after->types, bases);
        }
    }
    return bases;
}

std::optional<int> weightedMaintainability(const QList<FileDiff> &files, std::optional<CodeMetrics> FileDiff::*side) {
    double weighted = 0.0;
    qint64 lines = 0;
    for (const FileDiff &file : files) {
        const std::optional<CodeMetrics> &metrics = file.*side;
        if (metrics && metrics->sloc > 0) {
            weighted += double(metrics->maintainability) * metrics->sloc;
            lines += metrics->sloc;
        }
    }
    if (lines == 0) {
        return std::nullopt;
    }
    return int(std::lround(weighted / double(lines)));
}

}

int WorkspaceDiff::added() const {
    int total = 0;
    for (const FileDiff &file : files) {
        total += file.added;
    }
    return total;
}

int WorkspaceDiff::removed() const {
    int total = 0;
    for (const FileDiff &file : files) {
        total += file.removed;
    }
    return total;
}

std::optional<int> WorkspaceDiff::maintainabilityBefore() const {
    return weightedMaintainability(files, &FileDiff::before);
}

std::optional<int> WorkspaceDiff::maintainabilityAfter() const {
    return weightedMaintainability(files, &FileDiff::after);
}

WorkspaceDiff WorkspaceDiff::failure(const QString &error) {
    WorkspaceDiff diff;
    diff.error = error;
    return diff;
}

QList<FileDiff> GitDiff::parse(const QString &patch) {
    static const QRegularExpression hunkHeader(
        QStringLiteral(R"(^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@)"));

    QList<FileDiff> files;
    int oldLine = 0;
    int newLine = 0;
    int oldLeft = 0;   // sisa baris hunk yang belum terbaca, per sisi
    int newLeft = 0;
    QString oldMode;

    const QStringList lines = patch.split(QLatin1Char('\n'));
    for (QString line : lines) {
        if (line.endsWith(QLatin1Char('\r'))) {
            line.chop(1);
        }

        if (line.startsWith(QLatin1String("diff --git "))) {
            FileDiff file;
            file.path = pathFromGitHeader(line.mid(11));
            files.append(file);
            oldLeft = 0;
            newLeft = 0;
            oldMode.clear();
            continue;
        }
        if (files.isEmpty()) {
            continue;
        }
        FileDiff &file = files.last();

        // Di dalam hunk semua baris adalah isi, termasuk yang mirip header
        // ("--- x" = baris "-- x" yang dihapus)
        if (oldLeft > 0 || newLeft > 0) {
            const QChar marker = line.isEmpty() ? QLatin1Char(' ') : line.at(0);
            const QString text = line.mid(1);
            if (marker == QLatin1Char('+')) {
                file.lines.append({DiffLine::Kind::Added, text, 0, newLine++});
                ++file.added;
                --newLeft;
            } else if (marker == QLatin1Char('-')) {
                file.lines.append({DiffLine::Kind::Removed, text, oldLine++, 0});
                ++file.removed;
                --oldLeft;
            } else if (marker == QLatin1Char('\\')) {
                file.lines.append({DiffLine::Kind::Note, text.trimmed()});
            } else {
                // Konteks; tampil sebagai baris kosong tanpa spasi bila diff.suppressBlankEmpty aktif
                file.lines.append({DiffLine::Kind::Context, text, oldLine++, newLine++});
                --oldLeft;
                --newLeft;
            }
            continue;
        }

        const QRegularExpressionMatch hunk = hunkHeader.match(line);
        if (hunk.hasMatch()) {
            oldLine = hunk.captured(1).toInt();
            oldLeft = hunk.captured(2).isEmpty() ? 1 : hunk.captured(2).toInt();
            newLine = hunk.captured(3).toInt();
            newLeft = hunk.captured(4).isEmpty() ? 1 : hunk.captured(4).toInt();
            file.lines.append({DiffLine::Kind::Hunk, line});
        } else if (line.startsWith(QLatin1Char('\\'))) {
            // "\ No newline at end of file" setelah baris terakhir hunk
            file.lines.append({DiffLine::Kind::Note, line.mid(1).trimmed()});
        } else if (line.startsWith(QLatin1String("new file mode"))) {
            file.status = FileDiff::Status::Added;
        } else if (line.startsWith(QLatin1String("deleted file mode"))) {
            file.status = FileDiff::Status::Deleted;
        } else if (line.startsWith(QLatin1String("rename from "))) {
            file.status = FileDiff::Status::Renamed;
            file.oldPath = line.mid(12);
        } else if (line.startsWith(QLatin1String("rename to "))) {
            file.path = line.mid(10);
        } else if (line.startsWith(QLatin1String("Binary files "))) {
            file.note = QStringLiteral("File biner — isi tidak ditampilkan");
        } else if (line.startsWith(QLatin1String("old mode "))) {
            oldMode = line.mid(9);
        } else if (line.startsWith(QLatin1String("new mode "))) {
            file.note = QStringLiteral("Mode file berubah (%1 → %2)").arg(oldMode, line.mid(9));
        }
    }
    return files;
}

WorkspaceDiff GitDiff::collect(const QString &workingDirectory, bool withMetrics, const QString &baseRevision) {
    const QString git = GitProcess::executable();
    if (git.isEmpty()) {
        return WorkspaceDiff::failure(QStringLiteral("git tidak ditemukan di PATH, jadi perubahan kode tidak bisa dibaca."));
    }

    WorkspaceDiff result;
    const QString revision = baseRevision.isEmpty() ? QStringLiteral("HEAD")
                                                    : baseRevision + QStringLiteral("^{commit}");
    const GitOutput head = GitProcess::run(git, workingDirectory,
                                           {QStringLiteral("rev-parse"), QStringLiteral("-q"), QStringLiteral("--verify"),
                                            revision});
    QString base;
    if (head.ok()) {
        base = head.text();
        result.baseCommit = base.left(7);
    } else if (head.finished && head.exitCode == 1 && baseRevision.isEmpty()) {
        // Repository baru: HEAD belum menunjuk commit apa pun
        base = QString::fromLatin1(kEmptyTree);
    } else if (head.finished && head.exitCode == 1) {
        return WorkspaceDiff::failure(QStringLiteral("Commit dasar %1 tidak ada di repository.").arg(baseRevision.left(7)));
    } else {
        return WorkspaceDiff::failure(GitProcess::describeFailure(head));
    }

    const GitOutput patch = GitProcess::run(git, workingDirectory, diffArguments({base}));
    if (!patch.finished || patch.exitCode != 0) {
        return WorkspaceDiff::failure(GitProcess::describeFailure(patch));
    }
    result.files = parse(QString::fromUtf8(patch.out));

    // File baru yang belum di-git add tidak muncul di git diff; isinya dibaca langsung
    const GitOutput others = GitProcess::run(git, workingDirectory, {
        QStringLiteral("ls-files"), QStringLiteral("--others"), QStringLiteral("--exclude-standard"),
        QStringLiteral("-z"),
    });
    if (!others.finished || others.exitCode != 0) {
        return WorkspaceDiff::failure(GitProcess::describeFailure(others));
    }
    const QDir root(workingDirectory);
    qint64 budget = kMaxUntrackedTotalBytes;
    int listed = 0;
    for (const QByteArray &path : others.out.split('\0')) {
        if (path.isEmpty()) {
            continue;
        }
        if (listed == kMaxUntrackedFiles) {
            ++result.omittedUntracked;
            continue;
        }
        result.files.append(untrackedFile(root, QString::fromUtf8(path), &budget));
        ++listed;
    }

    if (withMetrics) {
        measureFiles(git, workingDirectory, result.baseCommit.isEmpty() ? QString() : base, &result.files);
        result.csharpTypes = resolveCSharpInheritance(git, workingDirectory, &result.files);
    }

    sortByPath(&result.files);
    return result;
}

WorkspaceDiff GitDiff::between(const QString &workingDirectory, const QString &from, const QString &to,
                               bool fromMergeBase) {
    const QString git = GitProcess::executable();
    if (git.isEmpty()) {
        return WorkspaceDiff::failure(QStringLiteral("git tidak ditemukan di PATH, jadi perubahan kode tidak bisa dibaca."));
    }

    QString error;
    const QString target = resolveCommit(git, workingDirectory, to, &error);
    if (target.isEmpty()) {
        return WorkspaceDiff::failure(error);
    }
    QString base;
    if (fromMergeBase) {
        const GitOutput mergeBase = GitProcess::run(git, workingDirectory, {QStringLiteral("merge-base"), from, to});
        if (!mergeBase.ok()) {
            return WorkspaceDiff::failure(
                mergeBase.finished && mergeBase.exitCode == 1
                    ? QStringLiteral("%1 dan %2 tidak punya titik cabang bersama, jadi perubahannya tidak bisa "
                                     "dibandingkan.").arg(from, to)
                    : GitProcess::describeFailure(mergeBase));
        }
        base = mergeBase.text();
    } else if (!from.isEmpty()) {
        base = resolveCommit(git, workingDirectory, from, &error);
        if (base.isEmpty()) {
            return WorkspaceDiff::failure(error);
        }
    }

    const GitOutput patch = GitProcess::run(
        git, workingDirectory, diffArguments({base.isEmpty() ? QString::fromLatin1(kEmptyTree) : base, target}));
    if (!patch.ok()) {
        return WorkspaceDiff::failure(GitProcess::describeFailure(patch));
    }
    WorkspaceDiff result;
    result.baseCommit = base.left(7);
    result.targetCommit = target.left(7);
    result.files = parse(QString::fromUtf8(patch.out));
    sortByPath(&result.files);
    return result;
}

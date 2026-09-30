#include "GitHistory.h"
#include "GitProcess.h"

#include <algorithm>

namespace {

// Satu commit per record (-z: diakhiri NUL), field dipisah US; pesan lengkap paling akhir
QString logFormat() {
    return QStringLiteral("--format=%H%x1f%P%x1f%an%x1f%ae%x1f%at%x1f%D%x1f%s%x1f%b");
}

// Field dipisah NUL, record diakhiri RS: judul commit ujung branch tidak dijamin satu baris
QString branchFormat() {
    return QStringLiteral("--format=%(refname)%00%(refname:lstrip=2)%00%(objectname)%00%(committerdate:unix)%00"
                          "%(HEAD)%00%(symref)%00%(upstream:short)%00%(upstream:track,nobracket)%00"
                          "%(worktreepath)%00%(subject)%1e");
}

QString gitMissing() {
    return QStringLiteral("git tidak ditemukan di PATH, jadi riwayat branch tidak bisa dibaca.");
}

// Folder kerja di subfolder repository: riwayat dibatasi ke commit yang mengubah subfolder itu
// (pathspec "." relatif terhadap folder kerja). Di akar repository tanpa pathspec, supaya commit
// merge dan commit kosong tidak ikut disaring git.
QStringList pathScope(const QString &git, const QString &directory, QString *error) {
    const GitOutput prefix = GitProcess::run(git, directory, {QStringLiteral("rev-parse"), QStringLiteral("--show-prefix")});
    if (!prefix.ok()) {
        *error = GitProcess::describeFailure(prefix);
        return {};
    }
    return prefix.text().isEmpty() ? QStringList{QStringLiteral("--")}
                                   : QStringList{QStringLiteral("--"), QStringLiteral(".")};
}

GitLog readLog(const QString &git, const QString &directory, const QStringList &scope, const QString &revision,
               int skip, int limit) {
    GitLog log;
    // Konfigurasi pengguna yang mengubah keluaran (warna, tanda tangan, dekorasi lengkap, --follow)
    // dimatikan; satu commit lebih dari batas diminta untuk tahu masih ada halaman berikutnya
    QStringList arguments = {
        QStringLiteral("-c"), QStringLiteral("log.follow=false"),
        QStringLiteral("log"), QStringLiteral("-z"), logFormat(), QStringLiteral("--no-color"),
        QStringLiteral("--no-show-signature"), QStringLiteral("--decorate=short"), QStringLiteral("--encoding=UTF-8"),
        QStringLiteral("--skip=%1").arg(skip), QStringLiteral("-n"), QString::number(limit + 1), revision,
    };
    arguments += scope;
    const GitOutput output = GitProcess::run(git, directory, arguments);
    if (!output.ok()) {
        log.error = GitProcess::describeFailure(output);
        return log;
    }
    log.commits = GitHistory::parseLog(output.out);
    if (log.commits.size() > limit) {
        log.hasMore = true;
        log.commits.resize(limit);
    }
    return log;
}

}

const GitBranch *GitBranchList::find(const QString &name) const {
    for (const GitBranch &branch : branches) {
        if (branch.name == name) {
            return &branch;
        }
    }
    return nullptr;
}

QList<GitBranch> GitHistory::parseBranches(const QByteArray &output) {
    QList<GitBranch> branches;
    for (QByteArray record : output.split('\x1e')) {
        // Setiap record sesudah yang pertama diawali baris baru penutup record sebelumnya
        while (record.startsWith('\n') || record.startsWith('\r')) {
            record.remove(0, 1);
        }
        const QList<QByteArray> fields = record.split('\0');
        if (fields.size() < 10) {
            continue;
        }
        GitBranch branch;
        branch.ref = QString::fromUtf8(fields.at(0));
        branch.remote = branch.ref.startsWith(QLatin1String("refs/remotes/"));
        // origin/HEAD hanya penunjuk ke branch remote lain
        if (!fields.at(5).isEmpty() || (!branch.remote && !branch.ref.startsWith(QLatin1String("refs/heads/")))) {
            continue;
        }
        branch.name = QString::fromUtf8(fields.at(1));
        branch.commit = QString::fromLatin1(fields.at(2));
        bool ok = false;
        const qint64 seconds = fields.at(3).toLongLong(&ok);
        if (ok) {
            branch.date = QDateTime::fromSecsSinceEpoch(seconds);
        }
        branch.current = fields.at(4).trimmed() == "*";
        branch.upstream = QString::fromUtf8(fields.at(6));
        branch.track = QString::fromUtf8(fields.at(7));
        branch.worktree = QString::fromUtf8(fields.at(8));
        branch.subject = QString::fromUtf8(fields.at(9)).trimmed();
        branches.append(branch);
    }
    return branches;
}

QList<GitCommit> GitHistory::parseLog(const QByteArray &output) {
    QList<GitCommit> commits;
    for (const QByteArray &record : output.split('\0')) {
        const QList<QByteArray> fields = record.split('\x1f');
        if (fields.size() < 8) {
            continue;
        }
        GitCommit commit;
        commit.hash = QString::fromLatin1(fields.at(0)).trimmed();
        commit.parents = QString::fromLatin1(fields.at(1)).split(QLatin1Char(' '), Qt::SkipEmptyParts);
        commit.author = QString::fromUtf8(fields.at(2));
        commit.email = QString::fromUtf8(fields.at(3));
        commit.date = QDateTime::fromSecsSinceEpoch(fields.at(4).toLongLong());
        commit.refs = parseRefs(QString::fromUtf8(fields.at(5)));
        commit.subject = QString::fromUtf8(fields.at(6));
        // Pesan commit paling akhir; pemisah field di dalamnya (hampir mustahil) disambung lagi
        QByteArray body = fields.at(7);
        for (qsizetype i = 8; i < fields.size(); ++i) {
            body += '\x1f' + fields.at(i);
        }
        commit.body = QString::fromUtf8(body).replace(QStringLiteral("\r\n"), QStringLiteral("\n")).trimmed();
        if (!commit.hash.isEmpty()) {
            commits.append(commit);
        }
    }
    return commits;
}

QStringList GitHistory::parseRefs(const QString &decoration) {
    QStringList refs;
    for (QString ref : decoration.split(QStringLiteral(", "), Qt::SkipEmptyParts)) {
        ref = ref.trimmed();
        if (ref.startsWith(QLatin1String("HEAD -> "))) {
            ref = ref.mid(8);
        }
        if (ref.isEmpty() || ref == QLatin1String("HEAD") || ref.endsWith(QLatin1String("/HEAD"))) {
            continue;
        }
        refs.append(ref);
    }
    return refs;
}

QString GitHistory::relativeTime(const QDateTime &when, const QDateTime &now) {
    if (!when.isValid()) {
        return QString();
    }
    const qint64 seconds = when.secsTo(now);
    if (seconds < 60) {
        return QStringLiteral("baru saja");   // termasuk jam komputer lain yang sedikit lebih cepat
    }
    if (seconds < 60 * 60) {
        return QStringLiteral("%1 menit lalu").arg(seconds / 60);
    }
    if (seconds < 24 * 60 * 60) {
        return QStringLiteral("%1 jam lalu").arg(seconds / (60 * 60));
    }
    const qint64 days = when.toLocalTime().date().daysTo(now.toLocalTime().date());
    if (days <= 1) {
        return QStringLiteral("kemarin");
    }
    if (days < 7) {
        return QStringLiteral("%1 hari lalu").arg(days);
    }
    return when.toLocalTime().toString(QStringLiteral("dd/MM/yyyy"));
}

GitHead GitHistory::head(const QString &workingDirectory) {
    GitHead head;
    const QString git = GitProcess::executable();
    if (git.isEmpty() || workingDirectory.isEmpty()) {
        return head;
    }
    // exit 0 = nama branch (juga branch yang belum punya commit), 1 = detached HEAD, lainnya = bukan repository
    const GitOutput symbolic = GitProcess::run(git, workingDirectory, {QStringLiteral("symbolic-ref"), QStringLiteral("-q"),
                                                                       QStringLiteral("--short"), QStringLiteral("HEAD")});
    if (!symbolic.finished || (symbolic.exitCode != 0 && symbolic.exitCode != 1)) {
        return head;
    }
    head.repository = true;
    head.branch = symbolic.ok() ? symbolic.text() : QString();
    const GitOutput commit = GitProcess::run(git, workingDirectory, {QStringLiteral("rev-parse"), QStringLiteral("-q"),
                                                                     QStringLiteral("--verify"), QStringLiteral("--short"),
                                                                     QStringLiteral("HEAD")});
    head.commit = commit.ok() ? commit.text() : QString();
    return head;
}

GitBranchList GitHistory::branches(const QString &workingDirectory) {
    GitBranchList list;
    const QString git = GitProcess::executable();
    if (git.isEmpty()) {
        list.error = gitMissing();
        return list;
    }
    const GitOutput top = GitProcess::run(git, workingDirectory, {QStringLiteral("rev-parse"), QStringLiteral("--show-toplevel"),
                                                                  QStringLiteral("--show-prefix")});
    if (!top.ok()) {
        list.error = GitProcess::describeFailure(top);
        return list;
    }
    const QStringList lines = QString::fromUtf8(top.out).split(QLatin1Char('\n'));
    list.root = lines.value(0).trimmed();
    list.subdir = lines.value(1).trimmed();
    while (list.subdir.endsWith(QLatin1Char('/'))) {
        list.subdir.chop(1);
    }
    list.head = head(workingDirectory);

    const GitOutput refs = GitProcess::run(git, workingDirectory, {
        QStringLiteral("for-each-ref"), QStringLiteral("--sort=-committerdate"), branchFormat(),
        QStringLiteral("refs/heads"), QStringLiteral("refs/remotes"),
    });
    if (!refs.ok()) {
        list.error = GitProcess::describeFailure(refs);
        return list;
    }
    // Urutan git (terbaru dulu) dipertahankan di dalam tiap kelompok: aktif, lokal, remote
    list.branches = parseBranches(refs.out);
    std::stable_sort(list.branches.begin(), list.branches.end(), [](const GitBranch &a, const GitBranch &b) {
        auto rank = [](const GitBranch &branch) { return branch.current ? 0 : branch.remote ? 2 : 1; };
        return rank(a) < rank(b);
    });
    return list;
}

GitLog GitHistory::log(const QString &workingDirectory, const QString &revision, int skip, int limit) {
    GitLog log;
    const QString git = GitProcess::executable();
    if (git.isEmpty()) {
        log.error = gitMissing();
        return log;
    }
    const QStringList scope = pathScope(git, workingDirectory, &log.error);
    if (!log.error.isEmpty()) {
        return log;
    }
    log = readLog(git, workingDirectory, scope, revision, skip, limit);
    if (!log.error.isEmpty() || skip > 0) {
        return log;
    }
    if (!log.hasMore) {
        log.total = int(log.commits.size());
        return log;
    }
    const GitOutput count = GitProcess::run(git, workingDirectory,
                                            QStringList{QStringLiteral("rev-list"), QStringLiteral("--count"), revision} + scope);
    bool ok = false;
    const int total = count.ok() ? count.text().toInt(&ok) : -1;
    log.total = ok ? total : -1;
    return log;
}

GitComparison GitHistory::compare(const QString &workingDirectory, const QString &target, const QString &source,
                                  int limit) {
    GitComparison comparison;
    const QString git = GitProcess::executable();
    if (git.isEmpty()) {
        comparison.error = gitMissing();
        return comparison;
    }
    const QStringList scope = pathScope(git, workingDirectory, &comparison.error);
    if (!comparison.error.isEmpty()) {
        return comparison;
    }

    // exit 1 = tidak ada titik cabang bersama (riwayat terpisah); jumlah & daftar commit tetap berarti
    const GitOutput base = GitProcess::run(git, workingDirectory, {QStringLiteral("merge-base"), target, source});
    if (base.ok()) {
        comparison.mergeBase = base.text();
    } else if (!base.finished || base.exitCode != 1) {
        comparison.error = GitProcess::describeFailure(base);
        return comparison;
    }

    // "<di belakang>\t<di depan>": sisi kiri rentang simetris = target
    const GitOutput counts = GitProcess::run(git, workingDirectory,
                                             QStringList{QStringLiteral("rev-list"), QStringLiteral("--left-right"),
                                                         QStringLiteral("--count"), target + QStringLiteral("...") + source}
                                                 + scope);
    if (!counts.ok()) {
        comparison.error = GitProcess::describeFailure(counts);
        return comparison;
    }
    const QStringList numbers = counts.text().split(QLatin1Char('\t'));
    comparison.behind = numbers.value(0).toInt();
    comparison.ahead = numbers.value(1).toInt();

    const GitLog ahead = readLog(git, workingDirectory, scope, target + QStringLiteral("..") + source, 0, limit);
    comparison.error = ahead.error;
    comparison.commits = ahead.commits;
    comparison.hasMore = ahead.hasMore;
    return comparison;
}

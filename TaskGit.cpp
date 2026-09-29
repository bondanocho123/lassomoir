#include "TaskGit.h"
#include "GitProcess.h"
#include "StageCatalog.h"

#include <QDir>
#include <QFileInfo>

namespace {

constexpr int kNetworkTimeoutMs = 120000;   // push lewat jaringan; perintah lokal memakai batas bawaan
constexpr qsizetype kMaxSlugLength = 40;
constexpr qsizetype kMaxSubjectLength = 72;

class Git {
public:
    explicit Git(QString program) : m_program(std::move(program)) {}

    bool isAvailable() const { return !m_program.isEmpty(); }

    GitOutput operator()(const QString &directory, const QStringList &arguments,
                         int timeoutMs = GitProcess::kDefaultTimeoutMs) const {
        return GitProcess::run(m_program, directory, arguments, QByteArray(), timeoutMs);
    }

private:
    QString m_program;
};

// Beberapa baris pertama keluaran git (stdout lalu stderr), untuk pesan error yang ringkas
QString summary(const GitOutput &output, int maxLines = 4) {
    const QString text = (QString::fromUtf8(output.out) + QLatin1Char('\n') + output.err).trimmed();
    QStringList lines;
    for (const QString &line : text.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        lines.append(line.trimmed());
        if (lines.size() == maxLines) {
            break;
        }
    }
    return lines.isEmpty() ? GitProcess::describeFailure(output) : lines.join(QStringLiteral("; "));
}

// Huruf/angka ASCII kecil; huruf beraksen jadi huruf dasarnya (é -> e), sisanya jadi satu "-"
QString slug(const QString &text, qsizetype limit) {
    QString result;
    const QString decomposed = text.normalized(QString::NormalizationForm_KD).toLower();
    for (const QChar c : decomposed) {
        if ((c >= QLatin1Char('a') && c <= QLatin1Char('z')) || (c >= QLatin1Char('0') && c <= QLatin1Char('9'))) {
            result += c;
        } else if (c.category() != QChar::Mark_NonSpacing && !result.isEmpty() && !result.endsWith(QLatin1Char('-'))) {
            result += QLatin1Char('-');
        }
        if (result.size() >= limit) {
            break;
        }
    }
    while (result.endsWith(QLatin1Char('-'))) {
        result.chop(1);
    }
    return result;
}

QString subjectFor(const TaskItem &task) {
    QString subject = task.title.simplified();
    if (subject.isEmpty()) {
        subject = QStringLiteral("Task %1").arg(task.id);
    }
    if (subject.size() > kMaxSubjectLength) {
        subject = subject.left(kMaxSubjectLength - 1) + QChar(0x2026);   // …
    }
    return subject;
}

// Putaran serah-terima: 1 + berapa kali stage serah-terima sudah mengembalikan task
int handoffRound(const TaskItem &task) {
    int round = 1;
    for (const StageRun &run : task.runs) {
        if (TaskGit::isHandoffStage(run.stage) && run.decision == ReviewDecision::SentBack) {
            ++round;
        }
    }
    return round;
}

QString currentBranch(const Git &git, const QString &directory) {
    const GitOutput head = git(directory, {QStringLiteral("symbolic-ref"), QStringLiteral("-q"),
                                           QStringLiteral("--short"), QStringLiteral("HEAD")});
    return head.ok() ? head.text() : QString();
}

// Worktree tambahan punya file .git (bukan folder) yang menunjuk ke repository utama
bool isLinkedWorktree(const QString &path) {
    return !path.isEmpty() && QFileInfo(path + QStringLiteral("/.git")).isFile();
}

// Push branch ke origin (dengan -u) bila remote itu ada. Gagal hanya jadi peringatan: commit lokal tetap sah.
void pushToOrigin(const Git &git, const QString &directory, const QString &ref, TaskGit::Result *result) {
    if (!git(directory, {QStringLiteral("remote"), QStringLiteral("get-url"), QStringLiteral("origin")}).ok()) {
        result->log.append(QStringLiteral("tidak ada remote origin: %1 hanya ada di lokal").arg(ref));
        return;
    }
    const GitOutput push = git(directory, {QStringLiteral("push"), QStringLiteral("-u"), QStringLiteral("origin"), ref},
                               kNetworkTimeoutMs);
    if (!push.ok()) {
        result->warnings.append(QStringLiteral("push %1 ke origin gagal: %2").arg(ref, summary(push)));
        return;
    }
    result->log.append(QStringLiteral("push %1 → origin").arg(ref));
}

}

bool TaskGit::isHandoffStage(const QString &stageKey) {
    return stageKey == QLatin1String("QA");
}

QString TaskGit::branchName(const TaskItem &task) {
    QStringList parts;
    const QString id = slug(task.id, 64);
    const QString title = slug(task.title, kMaxSlugLength);
    if (!id.isEmpty()) {
        parts.append(id);
    }
    if (!title.isEmpty()) {
        parts.append(title);
    }
    return QStringLiteral("lassomoir/") + (parts.isEmpty() ? QStringLiteral("task") : parts.join(QLatin1Char('-')));
}

bool TaskGit::looksLikeRepository(const QString &directory) {
    if (directory.isEmpty()) {
        return false;
    }
    QDir dir(QFileInfo(directory).absoluteFilePath());
    do {
        if (QFileInfo::exists(dir.filePath(QStringLiteral(".git")))) {
            return true;
        }
    } while (dir.cdUp());
    return false;
}

bool TaskGit::startsBranch(const TaskItem &task, const StageCatalog &catalog) {
    if (!task.branch.isEmpty()) {
        return false;
    }
    auto writes = [&catalog](const QString &stageKey) {
        const StageProfile *profile = catalog.profile(stageKey);
        return profile && profile->agent() && profile->agent()->writesWorkspace();
    };
    if (!writes(task.stage)) {
        return false;
    }
    for (const StageRun &run : task.runs) {
        if (writes(run.stage)) {
            return false;
        }
    }
    return true;
}

TaskGit::Result TaskGit::ensureWorktree(const QString &projectDir, const QString &path, const TaskItem &task) {
    Result result;
    result.branch = task.branch;
    if (isLinkedWorktree(task.branch.worktree)) {
        return result;
    }
    const Git git(GitProcess::executable());
    if (!git.isAvailable()) {
        result.error = QStringLiteral("git tidak ditemukan di PATH");
        return result;
    }

    // Task baru di folder yang bukan repository (atau belum punya commit) jalan tanpa branch seperti dulu
    const GitOutput top = git(projectDir, {QStringLiteral("rev-parse"), QStringLiteral("--show-toplevel")});
    if (!top.ok() && !top.err.contains(QLatin1String("not a git repository"))) {
        result.error = summary(top);
        return result;
    }
    const bool hasCommit = top.ok()
                           && git(projectDir, {QStringLiteral("rev-parse"), QStringLiteral("-q"),
                                               QStringLiteral("--verify"), QStringLiteral("HEAD")}).ok();
    if (!hasCommit) {
        const QString why = top.ok() ? QStringLiteral("repository di folder kerja belum punya commit")
                                     : QStringLiteral("folder kerja bukan repository git");
        if (task.branch.isEmpty()) {
            result.skipped = true;
            result.log.append(why + QStringLiteral(": task jalan tanpa branch"));
        } else {
            result.error = QStringLiteral("%1, padahal task ini punya branch %2").arg(why, task.branch.name);
        }
        return result;
    }

    const QString base = task.branch.base.isEmpty() ? currentBranch(git, projectDir) : task.branch.base;
    if (base.isEmpty()) {
        result.error = QStringLiteral("folder kerja sedang tidak di branch mana pun (detached HEAD); "
                                      "checkout dulu branch yang akan jadi dasar task ini");
        return result;
    }
    QString subdir = git(projectDir, {QStringLiteral("rev-parse"), QStringLiteral("--show-prefix")}).text();
    while (subdir.endsWith(QLatin1Char('/'))) {
        subdir.chop(1);
    }

    // Folder sisa di path milik aplikasi ini dibuang, lalu catatan worktree yang foldernya sudah
    // hilang dibersihkan: keduanya menghalangi worktree add
    if (QFileInfo::exists(path) && !QDir(path).removeRecursively()) {
        result.error = QStringLiteral("folder worktree lama %1 tidak bisa dibersihkan").arg(QDir::toNativeSeparators(path));
        return result;
    }
    git(projectDir, {QStringLiteral("worktree"), QStringLiteral("prune")});
    QDir().mkpath(QFileInfo(path).absolutePath());

    const QString name = task.branch.isEmpty() ? branchName(task) : task.branch.name;
    const bool existing = git(projectDir, {QStringLiteral("show-ref"), QStringLiteral("--verify"), QStringLiteral("--quiet"),
                                           QStringLiteral("refs/heads/") + name}).ok();
    const GitOutput added = existing
        ? git(projectDir, {QStringLiteral("worktree"), QStringLiteral("add"), path, name})
        : git(projectDir, {QStringLiteral("worktree"), QStringLiteral("add"), QStringLiteral("-b"), name, path, base});
    if (!added.ok()) {
        result.error = QStringLiteral("worktree untuk %1 gagal dibuat: %2").arg(name, summary(added));
        return result;
    }

    TaskBranch branch = task.branch;
    branch.name = name;
    branch.base = base;
    branch.worktree = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    branch.subdir = subdir;
    branch.mergedCommit.clear();
    if (!existing) {
        branch.baseCommit = git(branch.worktree, {QStringLiteral("rev-parse"), QStringLiteral("HEAD")}).text();
    } else if (branch.baseCommit.isEmpty()) {
        branch.baseCommit = git(projectDir, {QStringLiteral("merge-base"), base, name}).text();
    }
    // Folder kerja project bisa subfolder yang belum ada di commit mana pun (mis. masih kosong)
    QDir().mkpath(branch.directory());

    result.branch = branch;
    const QString shownPath = QDir::toNativeSeparators(branch.worktree);
    result.log.append(existing ? QStringLiteral("worktree dipasang lagi untuk branch %1 · %2").arg(name, shownPath)
                               : QStringLiteral("branch %1 dari %2 (%3) · worktree %4")
                                     .arg(name, base, branch.baseCommit.left(7), shownPath));
    return result;
}

TaskGit::Result TaskGit::handoff(const TaskItem &task) {
    Result result;
    result.branch = task.branch;
    const TaskBranch &branch = task.branch;
    if (!isLinkedWorktree(branch.worktree)) {
        result.error = QStringLiteral("worktree task ini tidak ada");
        return result;
    }
    const Git git(GitProcess::executable());
    if (!git.isAvailable()) {
        result.error = QStringLiteral("git tidak ditemukan di PATH");
        return result;
    }

    const QString head = currentBranch(git, branch.worktree);
    if (head != branch.name) {
        result.error = QStringLiteral("worktree tidak lagi di branch %1 (sekarang %2); kembalikan dengan git switch %1")
                           .arg(branch.name, head.isEmpty() ? QStringLiteral("detached HEAD") : head);
        return result;
    }

    const GitOutput add = git(branch.worktree, {QStringLiteral("add"), QStringLiteral("-A")});
    if (!add.ok()) {
        result.error = QStringLiteral("git add gagal: %1").arg(summary(add));
        return result;
    }
    const GitOutput staged = git(branch.worktree, {QStringLiteral("diff"), QStringLiteral("--cached"), QStringLiteral("--quiet")});
    const int round = handoffRound(task);
    if (staged.finished && staged.exitCode == 1) {
        const QString body = QStringLiteral("Diserahkan ke QA oleh L'Assommoir, putaran %1.\n\nTask: %2/%3")
                                 .arg(QString::number(round), task.projectId, task.id);
        const GitOutput commit = git(branch.worktree, {QStringLiteral("commit"), QStringLiteral("-q"),
                                                       QStringLiteral("-m"), subjectFor(task), QStringLiteral("-m"), body});
        if (!commit.ok()) {
            const bool noIdentity = commit.err.contains(QLatin1String("Please tell me who you are"));
            result.error = noIdentity ? QStringLiteral("identitas git belum diatur: jalankan git config --global user.name "
                                                       "\"Nama\" dan git config --global user.email \"email\"")
                                      : QStringLiteral("commit gagal: %1").arg(summary(commit));
            return result;
        }
        const QString hash = git(branch.worktree, {QStringLiteral("rev-parse"), QStringLiteral("--short"), QStringLiteral("HEAD")}).text();
        result.log.append(QStringLiteral("commit %1 di %2 (putaran %3)").arg(hash, branch.name, QString::number(round)));
    } else if (staged.ok()) {
        result.log.append(QStringLiteral("tidak ada perubahan baru untuk di-commit di %1").arg(branch.name));
    } else {
        result.error = summary(staged);
        return result;
    }

    pushToOrigin(git, branch.worktree, branch.name, &result);
    return result;
}

TaskGit::Result TaskGit::removeWorktree(const QString &projectDir, const TaskBranch &branch) {
    Result result;
    result.branch = branch;
    if (!branch.hasWorktree()) {
        return result;
    }
    const Git git(GitProcess::executable());
    if (git.isAvailable() && isLinkedWorktree(branch.worktree)) {
        git(projectDir, {QStringLiteral("worktree"), QStringLiteral("remove"), QStringLiteral("--force"), branch.worktree});
    }
    // Git bisa gagal (mis. folder kerja project sudah dipindah); worktree tetap dibuang, tapi hanya
    // bila memang worktree tambahan, bukan folder lain yang kebetulan tercatat di sesi
    if (isLinkedWorktree(branch.worktree) && !QDir(branch.worktree).removeRecursively()) {
        result.error = QStringLiteral("worktree %1 tidak bisa dihapus").arg(QDir::toNativeSeparators(branch.worktree));
        return result;
    }
    if (git.isAvailable()) {
        git(projectDir, {QStringLiteral("worktree"), QStringLiteral("prune")});
    }
    result.branch.worktree.clear();
    result.log.append(QStringLiteral("worktree %1 dihapus; branch %2 tetap ada")
                          .arg(QDir::toNativeSeparators(branch.worktree), branch.name));
    return result;
}

void TaskGit::prune(const QString &projectDir) {
    const Git git(GitProcess::executable());
    if (git.isAvailable() && looksLikeRepository(projectDir)) {
        git(projectDir, {QStringLiteral("worktree"), QStringLiteral("prune")});
    }
}

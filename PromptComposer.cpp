#include "PromptComposer.h"
#include "TaskItem.h"

#include <QStringList>

namespace {

// Indeks run terakhir yang memenuhi syarat; -1 bila tidak ada
template <typename Predicate>
qsizetype lastRunIndex(const QList<StageRun> &runs, Predicate predicate) {
    for (qsizetype i = runs.size() - 1; i >= 0; --i) {
        if (predicate(runs.at(i))) {
            return i;
        }
    }
    return -1;
}

QString section(const QString &heading, const StageRun &run, const QString &noteLabel) {
    QString text = QStringLiteral("# %1\n\n%2").arg(heading, run.result.message.trimmed());
    if (!run.reviewNote.isEmpty()) {
        text += QStringLiteral("\n\n%1:\n%2").arg(noteLabel, run.reviewNote);
    }
    return text;
}

}

QString TaskPromptComposer::compose(const TaskItem &task) const {
    QStringList lines = {QStringLiteral("# Task")};

    auto addField = [&lines](const QString &label, const QString &value) {
        const QString trimmed = value.trimmed();
        if (!trimmed.isEmpty()) {
            lines.append(QStringLiteral("%1: %2").arg(label, trimmed));
        }
    };
    addField(QStringLiteral("Judul"), task.title);
    addField(QStringLiteral("Kategori"), task.category);
    addField(QStringLiteral("Catatan"), task.subtext);

    QStringList sections = {lines.join('\n')};

    // Kontrak: dokumen gate terakhir yang disetujui, dari stage lain
    const qsizetype contract = lastRunIndex(task.runs, [&task](const StageRun &run) {
        return run.stage != task.stage && run.decision == ReviewDecision::Approved;
    });
    if (contract >= 0) {
        const StageRun &run = task.runs.at(contract);
        sections.append(section(QStringLiteral("Spesifikasi yang disetujui (%1)").arg(run.stage), run,
                                QStringLiteral("Catatan saat disetujui")));
    }

    // Hasil terbaru dari stage lain yang belum tampil sebagai kontrak
    const qsizetype previous = lastRunIndex(task.runs, [&task](const StageRun &run) {
        return run.stage != task.stage;
    });
    if (previous >= 0 && previous != contract) {
        const StageRun &run = task.runs.at(previous);
        sections.append(section(QStringLiteral("Hasil stage sebelumnya (%1)").arg(run.stage), run,
                                QStringLiteral("Catatan saat dikembalikan")));
    }

    // Putaran revisi di stage yang sama
    const StageRun *own = task.latestRun(task.stage);
    if (own && own->decision == ReviewDecision::Revise) {
        sections.append(section(QStringLiteral("Dokumen sebelumnya (untuk direvisi)"), *own,
                                QStringLiteral("Catatan revisi")));
    }

    return sections.join(QStringLiteral("\n\n")) + '\n';
}

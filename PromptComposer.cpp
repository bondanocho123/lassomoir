#include "PromptComposer.h"
#include "TaskItem.h"

#include <QFileInfo>
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

// Pagar blok kode yang lebih panjang dari deretan backtick mana pun di dalam isinya
QString fenceFor(const QString &text) {
    int longest = 0;
    int run = 0;
    for (const QChar c : text) {
        run = c == QLatin1Char('`') ? run + 1 : 0;
        longest = qMax(longest, run);
    }
    return QString(qMax(3, longest + 1), QLatin1Char('`'));
}

// Path dalam kode sebaris: tanpa ini agent bisa membaca "C:\data (lama)" sebagai "C:\data" + keterangan
QString codeSpan(const QString &text) {
    return text.contains(QLatin1Char('`')) ? QStringLiteral("`` %1 ``").arg(text) : QStringLiteral("`%1`").arg(text);
}

// Bahasa blok kode sesuai bentuk teks hasil DocumentText
QString fenceLanguage(const QString &fileName) {
    const QString suffix = QFileInfo(fileName).suffix().toLower();
    if (suffix == QLatin1String("docx")) {
        return QStringLiteral("markdown");
    }
    return suffix == QLatin1String("tsv") ? QStringLiteral("tsv") : QStringLiteral("csv");
}

QString attachmentSection(const TaskMaterials &materials) {
    QStringList lines = {QStringLiteral("# Lampiran")};
    if (!materials.imagePaths.isEmpty()) {
        QStringList names;
        for (const QString &path : materials.imagePaths) {
            names.append(QFileInfo(path).fileName());
        }
        lines.append(QStringLiteral("Foto (terlampir sebagai gambar di pesan ini): %1")
                         .arg(names.join(QStringLiteral(", "))));
    }
    if (materials.documents.isEmpty()) {
        return lines.join(QLatin1Char('\n'));
    }

    lines.append(QStringLiteral("Dokumen (isi teksnya di bawah):"));
    for (const TaskMaterials::Document &document : materials.documents) {
        lines.append(QStringLiteral("- %1: %2").arg(document.fileName, codeSpan(document.path)));
    }
    for (const TaskMaterials::Document &document : materials.documents) {
        lines.append(QString());
        lines.append(QStringLiteral("## %1").arg(document.fileName));
        if (!document.error.isEmpty()) {
            lines.append(QStringLiteral("(Tidak bisa dibaca: %1)").arg(document.error));
            continue;
        }
        if (document.omittedChars > 0) {
            lines.append(document.fullTextPath.isEmpty()
                             ? QStringLiteral("(Dipotong: %1 karakter terakhir tidak ikut di sini.)")
                                   .arg(document.omittedChars)
                             : QStringLiteral("(Dipotong: %1 karakter terakhir tidak ikut di sini; teks lengkapnya "
                                              "bisa dibaca dengan Read: %2)")
                                   .arg(document.omittedChars)
                                   .arg(codeSpan(document.fullTextPath)));
        }
        const QString fence = fenceFor(document.text);
        lines.append(fence + fenceLanguage(document.fileName));
        lines.append(document.text);
        lines.append(fence);
    }
    return lines.join(QLatin1Char('\n'));
}

QString referenceSection(const QStringList &directories) {
    QStringList lines = {
        QStringLiteral("# Folder referensi (hanya dibaca)"),
        QStringLiteral("Folder di luar folder kerja yang boleh kamu baca dengan Read, Grep, dan Glob sebagai acuan. "
                       "Jangan mengubah isinya."),
    };
    for (const QString &directory : directories) {
        lines.append(QStringLiteral("- %1").arg(codeSpan(directory)));
    }
    return lines.join(QLatin1Char('\n'));
}

}

QString TaskPromptComposer::compose(const TaskItem &task, const TaskMaterials &materials) const {
    QStringList lines = {QStringLiteral("# Task")};

    auto addField = [&lines](const QString &label, const QString &value) {
        const QString trimmed = value.trimmed();
        if (!trimmed.isEmpty()) {
            lines.append(QStringLiteral("%1: %2").arg(label, trimmed));
        }
    };
    addField(QStringLiteral("Judul"), task.title);
    addField(QStringLiteral("Kategori"), task.category);
    // Catatan pendek tetap sebaris; prompt multi-baris dari form task jadi bagian sendiri
    const QString note = task.subtext.trimmed();
    const bool longNote = note.contains(QLatin1Char('\n'));
    if (!longNote) {
        addField(QStringLiteral("Catatan"), note);
    }

    QStringList sections = {lines.join('\n')};
    if (longNote) {
        sections.append(QStringLiteral("# Instruksi\n\n%1").arg(note));
    }
    if (!materials.imagePaths.isEmpty() || !materials.documents.isEmpty()) {
        sections.append(attachmentSection(materials));
    }
    if (!materials.referenceDirectories.isEmpty()) {
        sections.append(referenceSection(materials.referenceDirectories));
    }

    // Kontrak: dokumen terakhir yang disetujui dari tiap stage lain, urut sesuai kemunculannya.
    // Yang pertama (spesifikasi) tetap dibawa ke stage-stage setelah review berikutnya.
    QList<qsizetype> contracts;
    QStringList seenStages;
    for (qsizetype i = task.runs.size() - 1; i >= 0; --i) {
        const StageRun &run = task.runs.at(i);
        if (run.stage != task.stage && run.decision == ReviewDecision::Approved && !seenStages.contains(run.stage)) {
            seenStages.append(run.stage);
            contracts.prepend(i);
        }
    }
    for (qsizetype i = 0; i < contracts.size(); ++i) {
        const StageRun &run = task.runs.at(contracts.at(i));
        const QString heading = i == 0 ? QStringLiteral("Spesifikasi yang disetujui (%1)")
                                       : QStringLiteral("Hasil review yang disetujui (%1)");
        sections.append(section(heading.arg(run.stage), run,
                                QStringLiteral("Catatan saat disetujui")));
    }

    // Hasil terbaru dari stage lain yang belum tampil sebagai kontrak
    const qsizetype previous = lastRunIndex(task.runs, [&task](const StageRun &run) {
        return run.stage != task.stage;
    });
    if (previous >= 0 && !contracts.contains(previous)) {
        const StageRun &run = task.runs.at(previous);
        sections.append(section(QStringLiteral("Hasil stage sebelumnya (%1)").arg(run.stage), run,
                                QStringLiteral("Catatan saat dikembalikan")));
    }

    // Putaran revisi di stage yang sama
    const StageRun *own = task.latestRun(task.stage);
    if (own && own->decision == ReviewDecision::Revise) {
        sections.append(section(QStringLiteral("Dokumen sebelumnya (untuk direvisi)"), *own,
                                QStringLiteral("Catatan revisi")));
    } else if (own && own->decision == ReviewDecision::SentBack) {
        // Stage ini yang dulu mengembalikan task: hasil lamanya jadi acuan memeriksa perbaikan
        sections.append(section(QStringLiteral("Hasil sebelumnya di stage ini (sebelum dikembalikan)"), *own,
                                QStringLiteral("Catatan saat dikembalikan")));
    }

    return sections.join(QStringLiteral("\n\n")) + '\n';
}

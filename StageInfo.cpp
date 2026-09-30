#include "StageInfo.h"

#include "StageCatalog.h"
#include "TaskGit.h"

#include <QHash>
#include <QStringList>

namespace {

// Warna skin (styles.qss): teks kaya tidak bisa diwarnai dari stylesheet widget-nya
constexpr char kNavy[] = "#33517a";    // nama stage, seperti judul task
constexpr char kBrown[] = "#a9743f";   // subjudul dan penanda, seperti judul kolom stage

// Bagian yang tidak bisa dibaca dari konfigurasi stage: peran, tugas, dan hal yang terlihat pengguna
struct StageCopy {
    QString role;
    QStringList duties;
    QStringList extras;
};

const StageCopy *copyOf(const QString &key) {
    static const QHash<QString, StageCopy> copies = {
        {QStringLiteral("WAITING"), {
            QStringLiteral("Antrean: task menunggu di sini sebelum dikerjakan agent."),
            {QStringLiteral("Menampung task baru beserta prompt dan lampirannya."),
             QStringLiteral("Tempat Anda memilih task mana yang mulai dikerjakan lebih dulu.")},
            {QStringLiteral("Seret kartu ke SPECIFIER untuk mulai dikerjakan agent.")}}},
        {QStringLiteral("SPECIFIER"), {
            QStringLiteral("Penyusun spesifikasi: mengubah permintaan menjadi spesifikasi yang siap dikerjakan."),
            {QStringLiteral("Merumuskan tujuan dan ruang lingkup (apa yang termasuk dan tidak)."),
             QStringLiteral("Menulis kriteria penerimaan yang bisa diuji, dalam daftar bernomor."),
             QStringLiteral("Mencatat pertanyaan terbuka bila ada hal yang ambigu.")},
            {QStringLiteral("Hasilnya spesifikasi (Markdown); yang Anda setujui jadi acuan utama semua stage berikutnya.")}}},
        {QStringLiteral("CODER"), {
            QStringLiteral("Pelaksana: menulis kode sesuai spesifikasi yang disetujui."),
            {QStringLiteral("Membaca kode terkait, lalu mengikuti gaya, penamaan, dan pola yang sudah ada."),
             QStringLiteral("Membuat perubahan sekecil mungkin yang menyelesaikan task."),
             QStringLiteral("Memperbaiki temuan bila task dikembalikan dari stage sesudahnya.")},
            {QStringLiteral("Bekerja di branch dan worktree git khusus task (bila folder kerja repository git)."),
             QStringLiteral("Tab Perubahan kode di panel hasil menampilkan diff-nya.")}}},
        {QStringLiteral("CLEANER"), {
            QStringLiteral("Perapi: merapikan kode tanpa mengubah perilakunya."),
            {QStringLiteral("Menghapus duplikasi dan kode mati, menyederhanakan logika yang berbelit."),
             QStringLiteral("Memperbaiki penamaan yang menyesatkan."),
             QStringLiteral("Menambah atau membenahi test bila proyek sudah punya test.")},
            {}}},
        {QStringLiteral("ARCHITECT"), {
            QStringLiteral("Peninjau arsitektur: menilai struktur dan dependensi kode."),
            {QStringLiteral("Menilai tanggung jawab tiap kelas atau modul dan batas antar lapisan."),
             QStringLiteral("Mencari ketergantungan berlebihan atau melingkar, serta risiko untuk perubahan berikutnya."),
             QStringLiteral("Menggambar diagram UML (class dan sequence) dalam Mermaid.")},
            {QStringLiteral("Hasilnya catatan arsitektur (Markdown): temuan, diagram, lalu saran berurutan."),
             QStringLiteral("Panel hasil menambah tab Perubahan kode, Maintainability, dan UML.")}}},
        {QStringLiteral("HARDENER"), {
            QStringLiteral("Penguat: membuat implementasi tahan terhadap kasus sulit."),
            {QStringLiteral("Menangani kasus tepi dan input yang tidak valid."),
             QStringLiteral("Memperbaiki penanganan error dan kebocoran sumber daya."),
             QStringLiteral("Menutup celah keamanan yang terlihat, lalu menambah test-nya bila proyek sudah punya test.")},
            {}}},
        {QStringLiteral("QA"), {
            QStringLiteral("Penguji: memeriksa apakah task memenuhi kriteria penerimaan."),
            {QStringLiteral("Memeriksa kriteria penerimaan pada spesifikasi satu per satu, lengkap dengan buktinya."),
             QStringLiteral("Menjalankan test yang sudah ada bila perintahnya diizinkan."),
             QStringLiteral("Membuat laporan singkat: lulus atau gagal per kriteria, lalu kesimpulan.")},
            {QStringLiteral("Hanya memeriksa: tidak mengubah file.")}}},
        {QStringLiteral("DONE"), {
            QStringLiteral("Selesai: task sudah melewati seluruh pipeline."),
            {QStringLiteral("Menandai task sudah melewati seluruh pipeline; tidak ada lagi pekerjaan agent."),
             QStringLiteral("Kode task tinggal di-review dan di-PR manual dari branch-nya.")},
            {QStringLiteral("Worktree task dibersihkan otomatis; branch-nya tetap ada."),
             QStringLiteral("Aplikasi tidak pernah merge sendiri ke branch dasar.")}}},
    };
    const auto it = copies.constFind(key);
    return it == copies.constEnd() ? nullptr : &it.value();
}

// "a", "a atau b", "a, b, atau c"
QString joinAtau(const QStringList &items) {
    if (items.size() < 2) {
        return items.join(QString());
    }
    return items.mid(0, items.size() - 1).join(QStringLiteral(", "))
           + (items.size() > 2 ? QStringLiteral(", atau ") : QStringLiteral(" atau ")) + items.last();
}

// Fitur yang dibaca dari profil stage dan urutan katalog
QStringList configuredFeatures(const StageCatalog &catalog, const StageProfile &profile) {
    const AgentDefinition *agent = profile.agent();
    if (!agent) {
        return {QStringLiteral("Tanpa agent: tombol Run pada kartu tidak aktif.")};
    }

    QStringList features;
    const QString tools = agent->tools.join(QStringLiteral(", "));
    features.append(agent->writesWorkspace()
                        ? QStringLiteral("Tool: %1.").arg(tools)
                        : QStringLiteral("Read-only (%1): tidak mengubah file.").arg(tools));

    // Nilai bawaan stage; sebagian stage bisa ditimpa per task lewat dialog New Task
    QStringList setup = {QStringLiteral("Model bawaan %1").arg(agent->model.isEmpty() ? QStringLiteral("Claude CLI")
                                                                                       : agent->model)};
    if (!agent->effort.isEmpty()) {
        setup.append(QStringLiteral("effort %1").arg(agent->effort));
    }
    setup.append(agent->maxConcurrent > 1 ? QStringLiteral("hingga %1 agent bersamaan").arg(agent->maxConcurrent)
                                          : QStringLiteral("satu agent sekali jalan"));
    features.append(setup.join(QStringLiteral(" · ")) + QLatin1Char('.'));

    // Ke mana task lanjut sesudah run berhasil, dan keputusan yang tersedia bila stage ini ber-gate
    const QStringList keys = catalog.keys();
    const qsizetype index = keys.indexOf(profile.key());
    const QString next = index >= 0 && index + 1 < keys.size() ? keys.at(index + 1) : QString();
    if (profile.exitPolicy().isGated()) {
        QStringList decisions = {next.isEmpty() ? QStringLiteral("Setujui") : QStringLiteral("Setujui → %1").arg(next),
                                 QStringLiteral("Revisi")};
        // "Kembalikan" hanya ada bila sebelum stage ini masih ada stage beragent
        for (qsizetype i = 0; i < index; ++i) {
            const StageProfile *earlier = catalog.profile(keys.at(i));
            if (earlier && earlier->agent()) {
                decisions.append(QStringLiteral("Kembalikan"));
                break;
            }
        }
        features.append(QStringLiteral("Run berhasil → menunggu review Anda: %1.").arg(joinAtau(decisions)));
    } else if (!next.isEmpty()) {
        features.append(QStringLiteral("Run berhasil → otomatis lanjut ke %1.").arg(next));
    }

    if (TaskGit::isHandoffStage(profile.key())) {
        features.append(QStringLiteral("Run di sini lebih dulu meng-commit dan mem-push branch task ke origin "
                                       "(bila task punya branch)."));
    }
    return features;
}

QString heading(const QString &text) {
    return QStringLiteral("<p style='margin:10px 0 3px 0; color:%1; font-size:10px'><b>%2</b></p>")
        .arg(QLatin1String(kBrown), text);
}

// Penanda di kolom sendiri, supaya baris yang terlipat tetap rata dengan teksnya
QString bulletList(const QStringList &items) {
    QString rows;
    for (const QString &item : items) {
        rows += QStringLiteral("<tr><td width='12' style='color:%1'>&#8226;</td><td>%2</td></tr>")
                    .arg(QLatin1String(kBrown), item.toHtmlEscaped());
    }
    return QStringLiteral("<table width='100%' cellspacing='0' cellpadding='1'>%1</table>").arg(rows);
}

}

QString StageInfo::html(const StageCatalog &catalog, const QString &stageKey) {
    const StageProfile *profile = catalog.profile(stageKey);
    if (!profile) {
        return QString();
    }
    const StageCopy *copy = copyOf(stageKey);

    QString html = QStringLiteral("<p style='margin:0; color:%1; font-size:13px'><b>%2</b></p>")
                       .arg(QLatin1String(kNavy), stageKey.toHtmlEscaped());
    if (copy) {
        html += QStringLiteral("<p style='margin:2px 0 0 0'>%1</p>").arg(copy->role.toHtmlEscaped());
        html += heading(QStringLiteral("TUGAS")) + bulletList(copy->duties);
    }
    QStringList features = configuredFeatures(catalog, *profile);
    if (copy) {
        features += copy->extras;
    }
    html += heading(QStringLiteral("FITUR")) + bulletList(features);
    return html;
}

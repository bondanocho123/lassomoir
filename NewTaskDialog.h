#ifndef NEWTASKDIALOG_H
#define NEWTASKDIALOG_H

#pragma once

#include <QDialog>
#include <QMap>
#include <QString>

#include "TaskAttachments.h"
#include "TaskItem.h"

class PromptEditor;
class StageCatalog;
struct GitBranchList;
class QLineEdit;
class QComboBox;
class QLabel;
class QMenu;
class QPushButton;
class QFrame;

// Dialog form untuk membuat TaskItem baru. Field-nya mengikuti struct TaskItem.h;
// tampilannya meniru palet & pola tombol yang sudah dipakai di aplikasi (lihat styles.qss).
class NewTaskDialog : public QDialog
{
    Q_OBJECT

public:
    // catalog: urutan stage dan bawaan model/effort tiap stage, jadi pilihan di combo selalu valid
    NewTaskDialog(const QString &projectId, const StageCatalog &catalog, QWidget *parent = nullptr);

    // Mode edit: form terisi dari task yang ada, termasuk lampiran di attachmentDirectory.
    // Stage dikunci karena perpindahan stage hanya lewat drag di board (yang lolos gerbang
    // StageProfile), bukan lewat form.
    NewTaskDialog(const TaskItem &task, const QString &attachmentDirectory, const StageCatalog &catalog,
                  QWidget *parent = nullptr);

    // Isi awal form task baru dari luar form, mis. "Jadikan task…" di kanvas brainstorm
    void prefill(const QString &title, const QString &subtext);

    // Hanya valid dipanggil setelah exec() == QDialog::Accepted.
    // Mode edit: id, projectId, dan field yang tidak ada di form (status, riwayat run)
    // dibawa dari task asal. attachments masih berisi nama lampiran lama: lampiran di form
    // disimpan pemanggil lewat TaskAttachments::save(attachments()).
    TaskItem resultTask() const;

    // Foto dan dokumen di kotak prompt, termasuk yang baru ditambahkan dan belum tersimpan
    QList<TaskAttachments::Draft> attachments() const;

    // Isi pilihan BRANCH dari repository folder kerja project. Git dibaca di thread pool; tombol
    // simpan menunggu sampai selesai. Folder kosong/bukan repository: task dibuat tanpa branch
    // dasar (branch-nya nanti mengikuti branch yang aktif di folder kerja). Task yang branch-nya
    // sudah dibuat tidak bisa ganti branch dasar, jadi tidak dibaca apa-apa.
    void loadBranches(const QString &workingDirectory);

private slots:
    void updateCreateButtonEnabled();

private:
    QString m_projectId;

    // Task asal saat mode edit; kosong (id kosong) saat membuat task baru
    TaskItem m_original;
    bool m_editing = false;

    // Satu pilihan di menu branch
    struct BranchOption {
        QString text;   // yang tampil: "main (aktif)", "fitur", "origin/rilis"
        QString base;   // nama branch lokal; yang baru ada di origin dibuat di lokal saat di-pull
        QString tip;
    };
    // Branch dasar task: label + caret tanpa kotak input, kliknya membuka menu branch. Tanpa menu
    // (disabled, berisi pesan saja) selama tidak ada branch yang bisa dipilih.
    QPushButton *m_branchButton;
    QMenu *m_branchMenu;
    QList<BranchOption> m_branchOptions;   // kosong = tidak ada branch yang bisa dipilih
    QString m_branch;                      // base yang terpilih
    QLabel *m_branchHint;
    bool m_branchesLoading = false;

    QLineEdit *m_titleInput;
    PromptEditor *m_promptInput;   // subtext multi-baris + foto & dokumen lampiran
    QComboBox *m_categoryInput;
    QComboBox *m_stageInput;
    QPushButton *m_btnCreate;

    // Pilihan model & effort per stage yang boleh disetel (SPECIFIER, CODER); data combo
    // kosong = ikuti bawaan stage
    struct TuningInputs {
        QComboBox *model = nullptr;
        QComboBox *effort = nullptr;
    };
    QMap<QString, TuningInputs> m_tuningInputs;

    // Bungkus label kecil (mis. "TASK TITLE") + widget input jadi satu kolom vertikal
    QWidget *buildField(const QString &labelText, QWidget *inputWidget);
    QFrame *buildDivider();

    // Dua field MODEL & EFFORT untuk satu stage; item pertama menyebut bawaan stage
    QWidget *buildTuningRow(const QString &stageKey, const AgentDefinition &defaults);
    static void selectValue(QComboBox *combo, const QString &value);

    // Tahap kedua loadBranches: fetch origin di latar belakang, lalu daftar dibaca ulang supaya
    // branch yang baru di-push rekan kerja ikut muncul. Simpan tidak menunggu tahap ini.
    void fetchBranches(const QString &workingDirectory);
    // Branch lokal + branch origin yang belum ada di lokal; branch kerja task (lassomoir/...) tidak.
    // Pilihan yang sedang aktif dipertahankan bila masih ada.
    void showBranches(const GitBranchList &list);
    // Label berisi satu pesan (tidak bisa dipilih); hint menjelaskan akibatnya
    void showBranchMessage(const QString &message, const QString &hint);
    // Tandai base terpilih: teks label, tooltip, dan item tebal di menu
    void selectBranch(const QString &base);
};

#endif // NEWTASKDIALOG_H

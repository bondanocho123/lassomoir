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
class QLineEdit;
class QComboBox;
class QLabel;
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

    // Hanya valid dipanggil setelah exec() == QDialog::Accepted.
    // Mode edit: id, projectId, dan field yang tidak ada di form (status, riwayat run)
    // dibawa dari task asal. attachments masih berisi nama lampiran lama: lampiran di form
    // disimpan pemanggil lewat TaskAttachments::save(attachments()).
    TaskItem resultTask() const;

    // Foto dan dokumen di kotak prompt, termasuk yang baru ditambahkan dan belum tersimpan
    QList<TaskAttachments::Draft> attachments() const;

private slots:
    void updateCreateButtonEnabled();

private:
    QString m_projectId;

    // Task asal saat mode edit; kosong (id kosong) saat membuat task baru
    TaskItem m_original;
    bool m_editing = false;

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
};

#endif // NEWTASKDIALOG_H

#ifndef NEWTASKDIALOG_H
#define NEWTASKDIALOG_H

#pragma once

#include <QDialog>
#include <QString>

#include "TaskItem.h"

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
    // stageKeys: urutan stage dari StageCatalog, jadi pilihan di combo selalu valid
    NewTaskDialog(const QString &projectId, const QStringList &stageKeys, QWidget *parent = nullptr);

    // Mode edit: form terisi dari task yang ada. Stage dikunci karena perpindahan stage
    // hanya lewat drag di board (yang lolos gerbang StageProfile), bukan lewat form.
    NewTaskDialog(const TaskItem &task, const QStringList &stageKeys, QWidget *parent = nullptr);

    // Hanya valid dipanggil setelah exec() == QDialog::Accepted.
    // Mode edit: id, projectId, dan field yang tidak ada di form (status, riwayat run)
    // dibawa dari task asal.
    TaskItem resultTask() const;

private slots:
    void updateCreateButtonEnabled();

private:
    QString m_projectId;

    // Task asal saat mode edit; kosong (id kosong) saat membuat task baru
    TaskItem m_original;
    bool m_editing = false;

    QLineEdit *m_titleInput;
    QLineEdit *m_subtextInput;
    QComboBox *m_categoryInput;
    QComboBox *m_stageInput;
    QPushButton *m_btnCreate;

    // Bungkus label kecil (mis. "TASK TITLE") + widget input jadi satu kolom vertikal
    QWidget *buildField(const QString &labelText, QWidget *inputWidget);
    QFrame *buildDivider();
};

#endif // NEWTASKDIALOG_H

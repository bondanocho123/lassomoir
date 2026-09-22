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
    explicit NewTaskDialog(const QString &projectId, QWidget *parent = nullptr);

    // Hanya valid dipanggil setelah exec() == QDialog::Accepted
    TaskItem resultTask() const;

private slots:
    void changeApprovals(int delta);
    void updateCreateButtonEnabled();

private:
    QString m_projectId;
    int m_approvals = 0;

    QLineEdit *m_titleInput;
    QLineEdit *m_subtextInput;
    QComboBox *m_categoryInput;
    QComboBox *m_stageInput;
    QLabel *m_approvalsValueLabel;
    QLabel *m_badgePreviewLabel;
    QPushButton *m_btnCreate;

    // Bungkus label kecil (mis. "TASK TITLE") + widget input jadi satu kolom vertikal
    QWidget *buildField(const QString &labelText, QWidget *inputWidget);
    QFrame *buildDivider();
    void updateBadgePreview();
};

#endif // NEWTASKDIALOG_H

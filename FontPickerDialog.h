#ifndef FONTPICKERDIALOG_H
#define FONTPICKERDIALOG_H

#pragma once

#include <QDialog>
#include <QStringList>

class QLabel;
class QListWidget;

// Daftar font antarmuka: baris pertama font bawaan, lalu font tertanam (AppFonts) yang
// masing-masing ditulis dengan font-nya sendiri, plus pratinjau teks. Terapkan -> fontChosen.
class FontPickerDialog : public QDialog {
    Q_OBJECT

public:
    // current: pilihan yang sedang berlaku (kosong = bawaan)
    FontPickerDialog(const QStringList &families, const QString &current, QWidget *parent = nullptr);

signals:
    // Kosong = kembali ke font bawaan
    void fontChosen(const QString &family);

private:
    void refreshPreview();
    QString selectedFamily() const;

    QListWidget *m_list;
    QLabel *m_preview;
};

#endif // FONTPICKERDIALOG_H

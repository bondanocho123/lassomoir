#ifndef DIFFVIEW_H
#define DIFFVIEW_H

#pragma once

#include <QList>
#include <QWidget>

struct WorkspaceDiff;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTreeWidget;

// Penampil perubahan kode folder kerja: ringkasan, daftar file (+/−), lalu diff berwarna
// semua file dengan nomor baris lama/baru. Klik file di daftar untuk melompat ke diff-nya.
class DiffView : public QWidget {
    Q_OBJECT

public:
    explicit DiffView(QWidget *parent = nullptr);

    void showLoading();
    void showDiff(const WorkspaceDiff &diff);

signals:
    void refreshRequested();

private:
    void showMessage(const QString &text, bool error);
    void fillFileList(const WorkspaceDiff &diff);
    void renderDiff(const WorkspaceDiff &diff);
    void scrollToFile(int index);

    QLabel *m_summary;
    QPushButton *m_refresh;
    QTreeWidget *m_files;
    QPlainTextEdit *m_text;
    QList<int> m_fileBlocks;   // nomor blok judul tiap file di m_text, urutan sama dengan daftar
};

#endif // DIFFVIEW_H

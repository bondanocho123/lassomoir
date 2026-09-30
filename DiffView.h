#ifndef DIFFVIEW_H
#define DIFFVIEW_H

#pragma once

#include "WorkspaceDiff.h"

#include <QList>
#include <QWidget>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QScrollBar;
class QTreeWidget;

// Penampil perubahan kode: ringkasan, daftar file (+/−), lalu diff berwarna semua file dengan
// nomor baris lama/baru. Klik file di daftar untuk melompat ke diff-nya. Dua tampilan: satu kolom
// (baris lama dan baru bergantian) atau dua kolom berdampingan (sebelum | sesudah) yang digulir
// bersamaan, dengan bagian baris yang berubah ditandai lebih tegas.
class DiffView : public QWidget {
    Q_OBJECT

public:
    explicit DiffView(QWidget *parent = nullptr);

    // text kosong = pesan untuk perubahan folder kerja
    void showLoading(const QString &text = QString());
    void showDiff(const WorkspaceDiff &diff);
    // Hanya pesan (mis. "pilih commit dulu"); daftar file dan diff disembunyikan
    void showMessage(const QString &text, bool error = false);

    // Tombol "Muat ulang" hanya berarti untuk perubahan folder kerja yang bisa berubah sewaktu-waktu
    void setRefreshVisible(bool visible);

    bool isSideBySide() const { return m_sideBySide; }
    void setSideBySide(bool sideBySide);

signals:
    void refreshRequested();
    void sideBySideChanged(bool sideBySide);

private:
    void fillFileList(const WorkspaceDiff &diff);
    // Isi tampilan yang aktif dari m_diff; tampilan lainnya dikosongkan
    void render();
    void renderUnified(const WorkspaceDiff &diff);
    void renderSideBySide(const WorkspaceDiff &diff);
    void scrollToFile(int index);
    // Indeks file yang judulnya berada di atas baris teratas yang terlihat
    int visibleFile() const;
    // Gulir dua scrollbar bersamaan; nilai yang dipangkas batas scrollbar lain tidak dipantulkan balik
    void linkScrollBars(QScrollBar *first, QScrollBar *second);

    QLabel *m_summary;
    QWidget *m_modes;
    QPushButton *m_unifiedButton;
    QPushButton *m_splitButton;
    QPushButton *m_refresh;
    QTreeWidget *m_files;
    QPlainTextEdit *m_text;       // satu kolom
    QWidget *m_split;             // dua kolom: m_oldText | m_newText
    QPlainTextEdit *m_oldText;
    QPlainTextEdit *m_newText;
    WorkspaceDiff m_diff;         // yang sedang tampil, untuk digambar ulang saat tampilan diganti
    bool m_sideBySide = false;
    bool m_syncingScroll = false;
    QList<int> m_fileBlocks;   // nomor blok judul tiap file di tampilan aktif, urutan sama dengan daftar
};

#endif // DIFFVIEW_H

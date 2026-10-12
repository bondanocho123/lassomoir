#ifndef NOTIONEXPORTDIALOG_H
#define NOTIONEXPORTDIALOG_H

#pragma once

#include "NotionExport.h"

#include <QDialog>
#include <QList>
#include <QString>
#include <QTimer>

class NotionClient;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;

// "Ke Notion": satu dokumen Markdown dikirim sebagai halaman baru di bawah halaman yang dipilih dari
// daftar halaman yang dibagikan ke integrasi. Halaman induk terakhir dipilih lebih dulu. Halaman
// yang sudah ada di Notion tidak diubah.
class NotionExportDialog : public QDialog {
    Q_OBJECT

public:
    NotionExportDialog(const QString &token, const QString &title, const QString &markdown, QWidget *parent = nullptr);

signals:
    // Halaman barunya sudah ada di Notion
    void exported(const QString &title, const QString &url);

private:
    void search();
    void showPages(const QList<NotionExport::Page> &pages);
    void send();
    void finish(const NotionExport::Page &page);
    void refreshSend();
    // error: pesan gagal (merah); selain itu keterangan biasa. Kosong = tidak ada yang ditampilkan.
    void showStatus(const QString &text, bool error = false);
    QString selectedId() const;
    QString selectedTitle() const;

    QString m_markdown;
    NotionClient *m_client;
    QTimer m_searchDelay;      // pencarian menyusul setelah ketikan berhenti sejenak
    bool m_searching = false;  // daftar belum cocok dengan kata carinya: belum boleh mengirim
    bool m_sending = false;
    QString m_url;             // halaman yang baru dibuat; kosong = belum terkirim

    QLineEdit *m_title;
    QLineEdit *m_search;
    QListWidget *m_pages;
    QLabel *m_status;
    QLabel *m_error;
    QPushButton *m_open;
    QPushButton *m_cancel;
    QPushButton *m_send;
};

#endif // NOTIONEXPORTDIALOG_H

#ifndef NOTIONEXPORT_H
#define NOTIONEXPORT_H

#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QString>

// Ekspor dokumen Markdown ke Notion sebagai halaman baru: isi permintaan ke API Notion, pembacaan
// jawabannya, dan penyesuaian Markdown. Murni (tanpa jaringan); pengirimannya di NotionClient.
namespace NotionExport {

// Versi API yang dipakai semua permintaan (header Notion-Version)
inline constexpr char kApiVersion[] = "2026-03-11";

struct Request {
    QByteArray method;   // "GET" atau "POST"
    QString path;        // di bawah https://api.notion.com/v1, mis. "/pages"
    QJsonObject body;    // kosong untuk GET
};

struct Page {
    QString id;
    QString title;
    QString icon;        // emoji halaman; kosong bila tidak ada
    QString url;
};

// Pemilik token: nama integrasinya dan workspace tempatnya terpasang
struct Account {
    QString name;
    QString workspace;
};

Request whoAmIRequest();
// Halaman yang dibagikan ke integrasi, yang terakhir diubah lebih dulu; query kosong = semuanya
Request searchRequest(const QString &query);
// Halaman baru di bawah parentId; halaman yang sudah ada tidak disentuh
Request createPageRequest(const QString &parentId, const QString &title, const QString &markdown);

Account parseAccount(const QJsonObject &body);
// Hasil yang bukan halaman utuh (parsial, data source) atau sudah di tempat sampah dilewati
QList<Page> parsePages(const QJsonObject &body);
Page parsePage(const QJsonObject &body);

// Alasan gagal untuk pengguna. status 0 = tidak sampai ke Notion (networkError).
QString errorMessage(int status, const QJsonObject &body, const QString &networkError);

// Markdown dialek GitHub (keluaran agent) -> dialek Notion: tabel pipa menjadi <table>, anak butir
// daftar diindentasi dengan tab, kutipan beberapa baris menjadi satu blok. Blok kode tidak diubah.
QString toNotionMarkdown(const QString &markdown);

// Judul halaman dari teks apa pun: satu baris, paling panjang 120 karakter
QString pageTitle(const QString &text);

}

#endif // NOTIONEXPORT_H

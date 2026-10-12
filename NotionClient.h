#ifndef NOTIONCLIENT_H
#define NOTIONCLIENT_H

#pragma once

#include "NotionExport.h"

#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>

#include <functional>

class QNetworkAccessManager;

// Pengirim permintaan NotionExport ke api.notion.com dengan satu token. Setiap panggilan dijawab
// dengan sinyal berhasil atau gagalnya; pencarian yang sudah disusul pencarian lain tidak dijawab.
class NotionClient : public QObject {
    Q_OBJECT

public:
    struct Reply {
        int status = 0;          // status HTTP; 0 = tidak sampai ke Notion
        QJsonObject body;
        QString networkError;
    };
    using Callback = std::function<void(const Reply &reply)>;

    // Hanya untuk test: pengganti pengiriman lewat jaringan, supaya tidak ada yang benar-benar
    // terkirim ke Notion. Kosong = kembali ke jaringan.
    using Transport = std::function<void(const NotionExport::Request &request, const QString &token, Callback done)>;
    static void setTransport(Transport transport);

    explicit NotionClient(const QString &token, QObject *parent = nullptr);

    void verify();
    void searchPages(const QString &query);
    void createPage(const QString &parentId, const QString &title, const QString &markdown);

signals:
    void verified(const NotionExport::Account &account);
    void verifyFailed(const QString &message);
    void pagesFound(const QList<NotionExport::Page> &pages);
    void searchFailed(const QString &message);
    void pageCreated(const NotionExport::Page &page);
    void createFailed(const QString &message);

private:
    void send(const NotionExport::Request &request, Callback done);

    QString m_token;
    QNetworkAccessManager *m_network;
    int m_search = 0;   // nomor pencarian terbaru; jawaban pencarian sebelumnya dibuang
};

#endif // NOTIONCLIENT_H

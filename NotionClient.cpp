#include "NotionClient.h"

#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QUrl>

namespace {

constexpr char kBaseUrl[] = "https://api.notion.com/v1";
constexpr int kTimeoutMs = 60000;

NotionClient::Transport g_transport;

bool succeeded(const NotionClient::Reply &reply) {
    return reply.status >= 200 && reply.status < 300;
}

QString failure(const NotionClient::Reply &reply) {
    return NotionExport::errorMessage(reply.status, reply.body, reply.networkError);
}

}

void NotionClient::setTransport(Transport transport) {
    g_transport = std::move(transport);
}

NotionClient::NotionClient(const QString &token, QObject *parent)
    : QObject(parent),
      m_token(token),
      m_network(new QNetworkAccessManager(this)) {
}

void NotionClient::verify() {
    send(NotionExport::whoAmIRequest(), [this](const Reply &reply) {
        if (succeeded(reply)) {
            emit verified(NotionExport::parseAccount(reply.body));
        } else {
            emit verifyFailed(failure(reply));
        }
    });
}

void NotionClient::searchPages(const QString &query) {
    const int search = ++m_search;
    send(NotionExport::searchRequest(query), [this, search](const Reply &reply) {
        if (search != m_search) {
            return;
        }
        if (succeeded(reply)) {
            emit pagesFound(NotionExport::parsePages(reply.body));
        } else {
            emit searchFailed(failure(reply));
        }
    });
}

void NotionClient::createPage(const QString &parentId, const QString &title, const QString &markdown) {
    send(NotionExport::createPageRequest(parentId, title, markdown), [this](const Reply &reply) {
        if (succeeded(reply)) {
            emit pageCreated(NotionExport::parsePage(reply.body));
        } else {
            emit createFailed(failure(reply));
        }
    });
}

void NotionClient::send(const NotionExport::Request &request, Callback done) {
    // Jawaban yang datang setelah klien ini dibuang (dialognya ditutup) tidak diteruskan
    const QPointer<NotionClient> self(this);
    Callback guarded = [self, done = std::move(done)](const Reply &reply) {
        if (self) {
            done(reply);
        }
    };
    if (g_transport) {
        g_transport(request, m_token, guarded);
        return;
    }

    QNetworkRequest network(QUrl(QLatin1String(kBaseUrl) + request.path));
    network.setRawHeader("Authorization", "Bearer " + m_token.toUtf8());
    network.setRawHeader("Notion-Version", NotionExport::kApiVersion);
    network.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/json"));
    // Token hanya untuk api.notion.com: pengalihan ke alamat lain tidak diikuti
    network.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    network.setTransferTimeout(kTimeoutMs);

    QNetworkReply *reply = request.method == "GET"
                               ? m_network->get(network)
                               : m_network->sendCustomRequest(network, request.method,
                                                              QJsonDocument(request.body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [reply, guarded]() {
        reply->deleteLater();
        Reply result;
        result.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        result.body = QJsonDocument::fromJson(reply->readAll()).object();
        if (result.status == 0) {
            result.networkError = reply->errorString();
        }
        guarded(result);
    });
}

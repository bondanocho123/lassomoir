#include "RuntimeNoticeDialog.h"

#include <QDesktopServices>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

namespace {

struct NoticeText {
    QString title;
    QString message;
    QString openLabel;
};

NoticeText textFor(const RuntimeCheck &check) {
    switch (check.status) {
    case RuntimeCheck::Status::Missing:
        return {QStringLiteral("Claude Code belum terpasang"),
                QStringLiteral("Agent Lassomoir berjalan lewat Claude Code CLI, tapi perintah <b>claude</b> "
                               "tidak ditemukan. Pasang dulu Claude Code, lalu buka ulang aplikasi ini."),
                QStringLiteral("Buka panduan instalasi")};
    case RuntimeCheck::Status::Outdated:
        return {QStringLiteral("Claude Code perlu diperbarui"),
                QStringLiteral("Versi Claude Code yang terpasang terlalu lama untuk Lassomoir. "
                               "Jalankan <b>claude update</b> di terminal, lalu buka ulang aplikasi ini."),
                QStringLiteral("Buka panduan update")};
    case RuntimeCheck::Status::LoggedOut:
        return {QStringLiteral("Claude Code belum login"),
                QStringLiteral("Login Claude Code tidak ada atau sudah tidak berlaku. Login lewat "
                               "<b>File &gt; Integrations</b>, atau jalankan <b>claude</b> di terminal lalu "
                               "ketik <b>/login</b> dan ikuti langkah di browser, setelah itu jalankan ulang "
                               "agent-nya."),
                QStringLiteral("Buka panduan login")};
    case RuntimeCheck::Status::BadApiKey:
        return {QStringLiteral("API key Anthropic bermasalah"),
                QStringLiteral("Run agent memakai API key dari <b>File &gt; Integrations</b>, tapi key itu "
                               "kosong atau ditolak server. Perbarui key-nya atau pilih login Claude Code, "
                               "setelah itu jalankan ulang agent-nya."),
                QStringLiteral("Buka Integrations")};
    case RuntimeCheck::Status::Ok:
        break;
    }
    return {};
}

}

RuntimeNoticeDialog::RuntimeNoticeDialog(const RuntimeCheck &check, QWidget *parent)
    : QDialog(parent), m_status(check.status) {
    setObjectName(QStringLiteral("runtimeNoticeDialog"));
    setProperty("helpUrl", check.helpUrl);
    const NoticeText text = textFor(check);
    setWindowTitle(text.title);

    auto *title = new QLabel(text.title, this);
    title->setObjectName(QStringLiteral("runtimeNoticeTitle"));

    QString body = text.message;
    if (!check.detail.isEmpty()) {
        body += QStringLiteral("<br><br><i>%1</i>").arg(check.detail.toHtmlEscaped());
    }
    auto *message = new QLabel(body, this);
    message->setObjectName(QStringLiteral("runtimeNoticeMessage"));
    message->setTextFormat(Qt::RichText);
    message->setWordWrap(true);
    message->setTextInteractionFlags(Qt::TextSelectableByMouse);

    auto *btnCancel = new QPushButton(QStringLiteral("Batal"), this);
    btnCancel->setObjectName(QStringLiteral("btnRuntimeNoticeCancel"));
    btnCancel->setCursor(Qt::PointingHandCursor);

    auto *btnOpen = new QPushButton(text.openLabel, this);
    btnOpen->setObjectName(QStringLiteral("btnRuntimeNoticeOpen"));
    btnOpen->setCursor(Qt::PointingHandCursor);
    // BadApiKey tidak punya halaman panduan: tombolnya membuka File > Integrations lewat accepted()
    const bool opensIntegrations = check.status == RuntimeCheck::Status::BadApiKey;
    btnOpen->setEnabled(opensIntegrations || !check.helpUrl.isEmpty());
    btnOpen->setDefault(true);

    connect(btnCancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(btnOpen, &QPushButton::clicked, this, [this, opensIntegrations]() {
        if (!opensIntegrations) {
            QDesktopServices::openUrl(QUrl(property("helpUrl").toString()));
        }
        accept();
    });

    auto *actions = new QHBoxLayout();
    actions->setSpacing(8);
    actions->addStretch(1);
    actions->addWidget(btnCancel);
    actions->addWidget(btnOpen);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(18, 16, 18, 14);
    root->setSpacing(10);
    root->addWidget(title);
    root->addWidget(message);
    root->addLayout(actions);

    setMinimumWidth(420);
}

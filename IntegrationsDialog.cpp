#include "IntegrationsDialog.h"
#include "AgentRuntime.h"
#include "NotionAccess.h"
#include "NotionClient.h"

#include <QDesktopServices>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

namespace {

constexpr int kDialogWidth = 480;
constexpr char kNotionIntegrationsUrl[] = "https://www.notion.so/profile/integrations";
// Isi di bawah tiap pilihan sejajar dengan teks radio-nya, bukan dengan bulatannya:
// bulatan 14px + jarak 8px (lihat styles.qss)
constexpr int kOptionIndent = 22;

QString statusText(const std::optional<AccountStatus> &account) {
    if (!account) {
        return QStringLiteral("Memeriksa status login…");
    }
    switch (account->state) {
    case AccountStatus::State::Unavailable:
        return QStringLiteral("Claude Code belum terpasang.");
    case AccountStatus::State::Unknown:
        return QStringLiteral("Status login tidak terbaca.");
    case AccountStatus::State::LoggedOut:
        return QStringLiteral("Belum login.");
    case AccountStatus::State::LoggedIn:
        break;
    }

    if (account->account.isEmpty()) {
        return account->keySource.isEmpty()
                   ? QStringLiteral("Sudah login.")
                   : QStringLiteral("Sudah login dengan API key (%1).").arg(account->keySource);
    }
    QString text = QStringLiteral("Login sebagai %1").arg(account->account);
    if (!account->plan.isEmpty()) {
        text += QStringLiteral(" · Claude %1").arg(account->plan);
    }
    if (!account->keySource.isEmpty()) {
        text += QStringLiteral("\n%1 terpasang di luar aplikasi dan dipakai lebih dulu daripada login ini.")
                    .arg(account->keySource);
    }
    return text;
}

QLabel *hintLabel(const QString &text, QWidget *parent) {
    auto *label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("integrationsHint"));
    label->setWordWrap(true);
    return label;
}

QPushButton *plainButton(const QString &text, const char *objectName, QWidget *parent) {
    auto *button = new QPushButton(text, parent);
    button->setObjectName(QLatin1String(objectName));
    button->setCursor(Qt::PointingHandCursor);
    button->setAutoDefault(false);
    return button;
}

bool hasSpace(const QString &text) {
    return std::any_of(text.cbegin(), text.cend(), [](QChar c) { return c.isSpace(); });
}

}

IntegrationsDialog::IntegrationsDialog(const AgentAccess::Settings &current, AccountLogin *login, QWidget *parent)
    : QDialog(parent),
      m_current(current),
      m_login(login),
      m_savedNotionToken(NotionAccess::token()) {
    setObjectName(QStringLiteral("integrationsDialog"));
    setWindowTitle(QStringLiteral("Integrations"));
    m_login->setParent(this);

    auto *title = new QLabel(QStringLiteral("Integrations"), this);
    title->setObjectName(QStringLiteral("integrationsTitle"));
    QLabel *subtitle = hintLabel(
        QStringLiteral("Cara agent masuk ke Claude. Pilihan berlaku untuk run berikutnya."), this);

    // Pilihan 1: login milik Claude Code
    m_useLogin = new QRadioButton(QStringLiteral("Login Claude Code (OAuth)"), this);
    m_useLogin->setObjectName(QStringLiteral("radioAccessLogin"));
    QLabel *loginHint = hintLabel(
        QStringLiteral("Agent memakai akun yang login di Claude Code pada komputer ini. Login dilakukan di "
                       "browser dan langsung berlaku."),
        this);
    m_loginStatus = new QLabel(this);
    m_loginStatus->setObjectName(QStringLiteral("integrationsLoginStatus"));
    // Isinya dari CLI (email): tampil apa adanya, bukan sebagai HTML
    m_loginStatus->setTextFormat(Qt::PlainText);
    m_loginStatus->setWordWrap(true);
    m_loginStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_btnLogin = new QPushButton(this);
    m_btnLogin->setObjectName(QStringLiteral("btnIntegrationsLogin"));
    m_btnLogin->setCursor(Qt::PointingHandCursor);
    m_btnLogin->setAutoDefault(false);

    auto *loginRow = new QHBoxLayout();
    loginRow->setSpacing(8);
    loginRow->addWidget(m_loginStatus, 1);
    loginRow->addWidget(m_btnLogin, 0, Qt::AlignTop);
    auto *loginBox = new QVBoxLayout();
    loginBox->setContentsMargins(kOptionIndent, 0, 0, 0);
    loginBox->setSpacing(6);
    loginBox->addWidget(loginHint);
    loginBox->addLayout(loginRow);

    // Pilihan 2: API key yang disimpan aplikasi
    m_useApiKey = new QRadioButton(QStringLiteral("API key Anthropic"), this);
    m_useApiKey->setObjectName(QStringLiteral("radioAccessApiKey"));
    QLabel *keyHint = hintLabel(
        QStringLiteral("Run agent ditagihkan ke API key ini, bukan ke langganan. Key disimpan di Windows "
                       "Credential Manager dan dihapus bila kembali ke login Claude Code."),
        this);
    m_apiKey = new QLineEdit(this);
    m_apiKey->setObjectName(QStringLiteral("integrationsApiKey"));
    m_apiKey->setEchoMode(QLineEdit::Password);
    m_apiKey->setPlaceholderText(m_current.apiKey.isEmpty()
                                     ? QStringLiteral("sk-ant-…")
                                     : QStringLiteral("Tersimpan (%1) · isi untuk mengganti")
                                           .arg(AgentAccess::maskedKey(m_current.apiKey)));
    auto *btnReveal = new QPushButton(QStringLiteral("Tampilkan"), this);
    btnReveal->setObjectName(QStringLiteral("btnIntegrationsReveal"));
    btnReveal->setCursor(Qt::PointingHandCursor);
    btnReveal->setCheckable(true);
    btnReveal->setAutoDefault(false);

    auto *keyRow = new QHBoxLayout();
    keyRow->setSpacing(8);
    keyRow->addWidget(m_apiKey, 1);
    keyRow->addWidget(btnReveal);
    auto *keyBox = new QVBoxLayout();
    keyBox->setContentsMargins(kOptionIndent, 0, 0, 0);
    keyBox->setSpacing(6);
    keyBox->addWidget(keyHint);
    keyBox->addLayout(keyRow);

    // Notion: tujuan tombol "Ke Notion", lepas dari cara agent masuk ke Claude
    auto *separator = new QFrame(this);
    separator->setObjectName(QStringLiteral("integrationsSeparator"));
    separator->setFixedHeight(1);
    auto *notionTitle = new QLabel(QStringLiteral("Notion"), this);
    notionTitle->setObjectName(QStringLiteral("integrationsSection"));
    QLabel *notionHint = hintLabel(
        QStringLiteral("Tombol Ke Notion mengirim dokumen sebagai halaman baru. Buat integrasi internal di Notion, "
                       "tempel tokennya di sini, lalu bagikan halaman tujuan ke integrasi itu (••• > Connections). "
                       "Token disimpan di Windows Credential Manager."),
        this);
    m_notionToken = new QLineEdit(this);
    m_notionToken->setObjectName(QStringLiteral("integrationsNotionToken"));
    m_notionToken->setEchoMode(QLineEdit::Password);
    auto *btnNotionReveal = plainButton(QStringLiteral("Tampilkan"), "btnIntegrationsNotionReveal", this);
    btnNotionReveal->setCheckable(true);
    m_btnNotionTest = plainButton(QStringLiteral("Tes koneksi"), "btnIntegrationsNotionTest", this);
    m_btnNotionClear = plainButton(QStringLiteral("Hapus token"), "btnIntegrationsNotionClear", this);
    auto *btnNotionCreate = plainButton(QStringLiteral("Buat token di Notion"), "btnIntegrationsNotionCreate", this);
    btnNotionCreate->setToolTip(QString::fromLatin1(kNotionIntegrationsUrl));
    m_notionStatus = new QLabel(this);
    m_notionStatus->setObjectName(QStringLiteral("integrationsNotionStatus"));
    // Isinya dari Notion (nama integrasi dan workspace): tampil apa adanya, bukan sebagai HTML
    m_notionStatus->setTextFormat(Qt::PlainText);
    m_notionStatus->setWordWrap(true);
    m_notionStatus->hide();

    auto *notionTokenRow = new QHBoxLayout();
    notionTokenRow->setSpacing(8);
    notionTokenRow->addWidget(m_notionToken, 1);
    notionTokenRow->addWidget(btnNotionReveal);
    auto *notionActions = new QHBoxLayout();
    notionActions->setSpacing(8);
    notionActions->addWidget(m_btnNotionTest);
    notionActions->addWidget(m_btnNotionClear);
    notionActions->addStretch(1);
    notionActions->addWidget(btnNotionCreate);
    auto *notionBox = new QVBoxLayout();
    notionBox->setSpacing(6);
    notionBox->addWidget(notionHint);
    notionBox->addLayout(notionTokenRow);
    notionBox->addLayout(notionActions);
    notionBox->addWidget(m_notionStatus);

    m_error = new QLabel(this);
    m_error->setObjectName(QStringLiteral("integrationsError"));
    m_error->setTextFormat(Qt::PlainText);
    m_error->setWordWrap(true);
    m_error->hide();

    auto *btnCancel = new QPushButton(QStringLiteral("Batal"), this);
    btnCancel->setObjectName(QStringLiteral("btnIntegrationsCancel"));
    btnCancel->setCursor(Qt::PointingHandCursor);
    btnCancel->setAutoDefault(false);
    auto *btnSave = new QPushButton(QStringLiteral("Simpan"), this);
    btnSave->setObjectName(QStringLiteral("btnIntegrationsSave"));
    btnSave->setCursor(Qt::PointingHandCursor);
    btnSave->setDefault(true);

    auto *actions = new QHBoxLayout();
    actions->setSpacing(8);
    actions->addStretch(1);
    actions->addWidget(btnCancel);
    actions->addWidget(btnSave);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(18, 16, 18, 14);
    root->setSpacing(10);
    root->addWidget(title);
    root->addWidget(subtitle);
    root->addSpacing(2);
    root->addWidget(m_useLogin);
    root->addLayout(loginBox);
    root->addSpacing(4);
    root->addWidget(m_useApiKey);
    root->addLayout(keyBox);
    root->addSpacing(4);
    root->addWidget(separator);
    root->addWidget(notionTitle);
    root->addLayout(notionBox);
    root->addWidget(m_error);
    root->addStretch(1);
    root->addLayout(actions);

    // Kolom key hanya untuk pilihan API key; login tetap bisa dijalankan di pilihan mana pun
    connect(m_useApiKey, &QRadioButton::toggled, this, [this, btnReveal](bool on) {
        m_apiKey->setEnabled(on);
        btnReveal->setEnabled(on);
        showError(QString());
        if (on) {
            m_apiKey->setFocus();
        }
    });
    // Pesan gagal soal key tidak lagi berlaku begitu key-nya diubah
    connect(m_apiKey, &QLineEdit::textEdited, this, [this]() { showError(QString()); });
    connect(btnReveal, &QPushButton::toggled, this, [this, btnReveal](bool on) {
        m_apiKey->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
        btnReveal->setText(on ? QStringLiteral("Sembunyikan") : QStringLiteral("Tampilkan"));
    });
    connect(m_btnLogin, &QPushButton::clicked, this, &IntegrationsDialog::toggleLogin);
    connect(m_notionToken, &QLineEdit::textEdited, this, [this]() {
        // Token baru diketik: penghapusan yang tadi diminta batal
        m_removeNotionToken = false;
        m_notionStatus->hide();
        showError(QString());
        refreshNotion();
    });
    connect(btnNotionReveal, &QPushButton::toggled, this, [this, btnNotionReveal](bool on) {
        m_notionToken->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
        btnNotionReveal->setText(on ? QStringLiteral("Sembunyikan") : QStringLiteral("Tampilkan"));
    });
    connect(m_btnNotionTest, &QPushButton::clicked, this, &IntegrationsDialog::testNotion);
    connect(m_btnNotionClear, &QPushButton::clicked, this, [this]() {
        m_removeNotionToken = true;
        m_notionToken->clear();
        m_notionStatus->setText(QStringLiteral("Token dihapus saat Simpan ditekan."));
        m_notionStatus->show();
        showError(QString());
        refreshNotion();
    });
    connect(btnNotionCreate, &QPushButton::clicked, this, []() {
        QDesktopServices::openUrl(QUrl(QString::fromLatin1(kNotionIntegrationsUrl)));
    });
    connect(btnCancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(btnSave, &QPushButton::clicked, this, &IntegrationsDialog::save);

    connect(m_login, &AccountLogin::statusChanged, this, [this](const AccountStatus &status) {
        m_account = status;
        refreshLogin();
    });
    connect(m_login, &AccountLogin::finished, this, [this](bool success, const QString &message) {
        // Status baru menyusul lewat statusChanged
        m_account.reset();
        showError(success ? QString() : message);
        refreshLogin();
    });

    const bool usesApiKey = m_current.method == AgentAccess::Method::ApiKey;
    m_apiKey->setEnabled(usesApiKey);
    btnReveal->setEnabled(usesApiKey);
    (usesApiKey ? m_useApiKey : m_useLogin)->setChecked(true);

    setMinimumWidth(kDialogWidth);
    // Font dari stylesheet harus sudah terpasang sebelum tinggi teks dihitung
    ensurePolished();
    resize(kDialogWidth, 0);
    refreshNotion();
    refreshLogin();
    m_login->refresh();
}

void IntegrationsDialog::focusNotionToken() {
    m_notionToken->setFocus();
}

void IntegrationsDialog::fitHeight() {
    // Label yang membungkus teks hanya melaporkan tinggi satu baris sebagai minimumnya, jadi
    // jendela tidak membesar sendiri saat status atau pesan gagal bertambah baris.
    // activate() dulu: layout bersarang baru membuang ukuran lamanya saat diaktifkan.
    layout()->activate();
    const int needed = layout()->totalHeightForWidth(width());
    if (height() < needed) {
        resize(width(), needed);
    }
}

void IntegrationsDialog::refreshLogin() {
    if (m_login->isRunning()) {
        m_loginStatus->setText(QStringLiteral("Menunggu login di browser… Bila browser tidak terbuka, jalankan "
                                              "claude auth login di terminal."));
        m_btnLogin->setText(QStringLiteral("Batal login"));
        m_btnLogin->setEnabled(true);
    } else {
        m_loginStatus->setText(statusText(m_account));
        m_btnLogin->setText(m_account && m_account->state == AccountStatus::State::LoggedIn
                                ? QStringLiteral("Ganti akun")
                                : QStringLiteral("Login lewat browser"));
        m_btnLogin->setEnabled(m_account && m_account->state != AccountStatus::State::Unavailable);
    }
    fitHeight();
}

void IntegrationsDialog::toggleLogin() {
    if (m_login->isRunning()) {
        m_login->cancel();
        return;
    }
    showError(QString());
    m_login->start();
    refreshLogin();
}

void IntegrationsDialog::refreshNotion() {
    const bool saved = !m_savedNotionToken.isEmpty() && !m_removeNotionToken;
    m_notionToken->setPlaceholderText(saved ? QStringLiteral("Tersimpan (%1) · isi untuk mengganti")
                                                  .arg(AgentAccess::maskedKey(m_savedNotionToken))
                                            : QStringLiteral("ntn_…"));
    m_btnNotionClear->setEnabled(saved);
    // Tokennya baru saja diganti atau dihapus: jawaban tes yang masih ditunggu bukan lagi tentangnya
    delete m_notionTest;
    m_notionTest = nullptr;
    m_btnNotionTest->setEnabled(true);
    fitHeight();
}

void IntegrationsDialog::testNotion() {
    // Yang dites adalah yang akan berlaku setelah Simpan: ketikan, atau token tersimpan
    QString token = m_notionToken->text().trimmed();
    if (token.isEmpty() && !m_removeNotionToken) {
        token = m_savedNotionToken;
    }
    m_notionStatus->hide();
    if (token.isEmpty()) {
        showError(QStringLiteral("Isi token Notion dulu."));
        return;
    }
    if (hasSpace(token)) {
        showError(QStringLiteral("Token Notion tidak boleh berisi spasi atau baris baru."));
        return;
    }
    showError(QString());

    // Jawaban tes sebelumnya tidak lagi ditunggu
    delete m_notionTest;
    m_notionTest = new NotionClient(token, this);
    connect(m_notionTest, &NotionClient::verified, this, [this](const NotionExport::Account &account) {
        QString text = account.name.isEmpty() ? QStringLiteral("Terhubung ke Notion")
                                              : QStringLiteral("Terhubung sebagai %1").arg(account.name);
        if (!account.workspace.isEmpty()) {
            text += QStringLiteral(" · workspace %1").arg(account.workspace);
        }
        m_notionStatus->setText(text);
        m_notionStatus->show();
        m_btnNotionTest->setEnabled(true);
        fitHeight();
    });
    connect(m_notionTest, &NotionClient::verifyFailed, this, [this](const QString &message) {
        m_notionStatus->hide();
        m_btnNotionTest->setEnabled(true);
        showError(message);
    });
    m_notionStatus->setText(QStringLiteral("Menghubungi Notion…"));
    m_notionStatus->show();
    m_btnNotionTest->setEnabled(false);
    fitHeight();
    m_notionTest->verify();
}

void IntegrationsDialog::save() {
    const QString notionToken = m_notionToken->text().trimmed();
    if (hasSpace(notionToken)) {
        showError(QStringLiteral("Token Notion tidak boleh berisi spasi atau baris baru."));
        return;
    }

    AgentAccess::Settings settings;
    if (m_useApiKey->isChecked()) {
        const QString typed = m_apiKey->text().trimmed();
        if (hasSpace(typed)) {
            showError(QStringLiteral("API key tidak boleh berisi spasi atau baris baru."));
            return;
        }
        settings.method = AgentAccess::Method::ApiKey;
        // Kolom kosong = key yang sudah tersimpan tetap dipakai
        settings.apiKey = typed.isEmpty() ? m_current.apiKey : typed;
        if (settings.apiKey.isEmpty()) {
            showError(QStringLiteral("Isi API key Anthropic dulu."));
            return;
        }
    }

    QString error;
    if (!AgentAccess::save(settings, &error)) {
        showError(QStringLiteral("API key tidak bisa disimpan: %1").arg(error));
        return;
    }
    m_current = settings;
    emit accessSaved(settings.method);

    // Kolom kosong = token Notion yang sudah tersimpan tetap dipakai
    if (!notionToken.isEmpty() && notionToken != m_savedNotionToken) {
        if (!NotionAccess::saveToken(notionToken, &error)) {
            showError(QStringLiteral("Token Notion tidak bisa disimpan: %1").arg(error));
            return;
        }
        // Halaman induk terakhir milik workspace token yang lama
        NotionAccess::saveLastParent({});
        emit notionSaved(true);
    } else if (m_removeNotionToken && !m_savedNotionToken.isEmpty()) {
        NotionAccess::clear();
        emit notionSaved(false);
    }
    accept();
}

void IntegrationsDialog::showError(const QString &message) {
    m_error->setText(message);
    m_error->setVisible(!message.isEmpty());
    fitHeight();
}

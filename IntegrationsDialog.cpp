#include "IntegrationsDialog.h"
#include "AgentRuntime.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QVBoxLayout>

#include <algorithm>

namespace {

constexpr int kDialogWidth = 480;
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

}

IntegrationsDialog::IntegrationsDialog(const AgentAccess::Settings &current, AccountLogin *login, QWidget *parent)
    : QDialog(parent),
      m_current(current),
      m_login(login) {
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
    refreshLogin();
    m_login->refresh();
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

void IntegrationsDialog::save() {
    AgentAccess::Settings settings;
    if (m_useApiKey->isChecked()) {
        const QString typed = m_apiKey->text().trimmed();
        if (std::any_of(typed.cbegin(), typed.cend(), [](QChar c) { return c.isSpace(); })) {
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
    emit accessSaved(settings.method);
    accept();
}

void IntegrationsDialog::showError(const QString &message) {
    m_error->setText(message);
    m_error->setVisible(!message.isEmpty());
    fitHeight();
}

#ifndef INTEGRATIONSDIALOG_H
#define INTEGRATIONSDIALOG_H

#pragma once

#include "AgentAccess.h"
#include "AgentTypes.h"

#include <QDialog>

#include <optional>

class AccountLogin;
class NotionClient;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;

// File > Integrations. Cara run agent masuk ke Claude: login Claude Code (OAuth; login-nya
// dijalankan CLI di browser, langsung berlaku tanpa menunggu Simpan) atau API key Anthropic
// (disimpan lewat AgentAccess). Simpan -> AgentAccess::save lalu accessSaved; berlaku untuk run
// berikutnya. Di bawahnya token integrasi Notion untuk tombol "Ke Notion" (NotionAccess); token
// baru atau penghapusannya juga baru berlaku saat Simpan.
class IntegrationsDialog : public QDialog {
    Q_OBJECT

public:
    // current: pilihan yang sedang berlaku. login: status dan login akun backend; jadi milik dialog.
    IntegrationsDialog(const AgentAccess::Settings &current, AccountLogin *login, QWidget *parent = nullptr);

    // Dialog dibuka karena token Notion belum ada: kolomnya langsung siap diketik
    void focusNotionToken();

signals:
    void accessSaved(AgentAccess::Method method);
    // Token Notion diganti (true) atau dihapus (false)
    void notionSaved(bool connected);

private:
    void refreshLogin();
    void toggleLogin();
    void refreshNotion();
    void testNotion();
    void save();
    void showError(const QString &message);
    // Tinggikan dialog bila isinya (status login, pesan gagal) tidak lagi muat
    void fitHeight();

    AgentAccess::Settings m_current;
    AccountLogin *m_login;
    std::optional<AccountStatus> m_account;   // kosong selama status sedang diperiksa
    QRadioButton *m_useLogin;
    QRadioButton *m_useApiKey;
    QLabel *m_loginStatus;
    QPushButton *m_btnLogin;
    QLineEdit *m_apiKey;
    QString m_savedNotionToken;
    bool m_removeNotionToken = false;         // "Hapus token" ditekan; dijalankan saat Simpan
    NotionClient *m_notionTest = nullptr;     // tes koneksi yang sedang berjalan
    QLineEdit *m_notionToken;
    QLabel *m_notionStatus;
    QPushButton *m_btnNotionTest;
    QPushButton *m_btnNotionClear;
    QLabel *m_error;
};

#endif // INTEGRATIONSDIALOG_H

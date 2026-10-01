#ifndef INTEGRATIONSDIALOG_H
#define INTEGRATIONSDIALOG_H

#pragma once

#include "AgentAccess.h"
#include "AgentTypes.h"

#include <QDialog>

#include <optional>

class AccountLogin;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;

// File > Integrations: cara run agent masuk ke Claude. Login Claude Code (OAuth; login-nya
// dijalankan CLI di browser, langsung berlaku tanpa menunggu Simpan) atau API key Anthropic
// (disimpan lewat AgentAccess). Simpan -> AgentAccess::save lalu accessSaved; berlaku untuk run
// berikutnya.
class IntegrationsDialog : public QDialog {
    Q_OBJECT

public:
    // current: pilihan yang sedang berlaku. login: status dan login akun backend; jadi milik dialog.
    IntegrationsDialog(const AgentAccess::Settings &current, AccountLogin *login, QWidget *parent = nullptr);

signals:
    void accessSaved(AgentAccess::Method method);

private:
    void refreshLogin();
    void toggleLogin();
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
    QLabel *m_error;
};

#endif // INTEGRATIONSDIALOG_H

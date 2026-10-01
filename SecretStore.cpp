#include "SecretStore.h"

#ifdef Q_OS_WIN
#include <QByteArray>
#include <QCoreApplication>

#include <qt_windows.h>
#include <wincred.h>

#include <string>

namespace {

std::wstring targetName(const QString &name) {
    return QStringLiteral("%1/%2").arg(QCoreApplication::applicationName(), name).toStdWString();
}

}

bool SecretStore::write(const QString &name, const QString &secret, QString *error) {
    std::wstring target = targetName(name);
    QByteArray blob = secret.toUtf8();

    CREDENTIALW credential = {};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = target.data();
    credential.CredentialBlobSize = DWORD(blob.size());
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(blob.data());
    // Untuk akun Windows ini di komputer ini saja: tidak ikut roaming profile ke komputer lain
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;

    if (CredWriteW(&credential, 0)) {
        return true;
    }
    if (error) {
        *error = qt_error_string();
    }
    return false;
}

QString SecretStore::read(const QString &name) {
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(targetName(name).c_str(), CRED_TYPE_GENERIC, 0, &credential)) {
        return QString();
    }
    const QString secret = QString::fromUtf8(reinterpret_cast<const char *>(credential->CredentialBlob),
                                             qsizetype(credential->CredentialBlobSize));
    CredFree(credential);
    return secret;
}

void SecretStore::remove(const QString &name) {
    CredDeleteW(targetName(name).c_str(), CRED_TYPE_GENERIC, 0);
}

#else

// Di luar Windows belum ada penyimpanan rahasia: menolak lebih baik daripada menulis teks polos
bool SecretStore::write(const QString &name, const QString &secret, QString *error) {
    Q_UNUSED(name);
    Q_UNUSED(secret);
    if (error) {
        *error = QStringLiteral("penyimpanan rahasia baru tersedia di Windows");
    }
    return false;
}

QString SecretStore::read(const QString &name) {
    Q_UNUSED(name);
    return QString();
}

void SecretStore::remove(const QString &name) {
    Q_UNUSED(name);
}

#endif

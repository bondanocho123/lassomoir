#ifndef SECRETSTORE_H
#define SECRETSTORE_H

#pragma once

#include <QString>

// Rahasia milik pengguna (API key) di Windows Credential Manager: dienkripsi sistem untuk akun
// Windows yang sedang login dan tidak pernah ditulis ke settings.ini. Terlihat di Control Panel >
// Credential Manager > Windows Credentials sebagai "<nama aplikasi>/<name>"; nama aplikasi ikut di
// depan supaya rahasia milik test terpisah dari milik aplikasi.
namespace SecretStore {

// false + *error bila sistem menolak menyimpan
bool write(const QString &name, const QString &secret, QString *error = nullptr);

// Kosong bila belum pernah disimpan
QString read(const QString &name);

void remove(const QString &name);

}

#endif // SECRETSTORE_H

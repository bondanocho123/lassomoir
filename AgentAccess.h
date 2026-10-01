#ifndef AGENTACCESS_H
#define AGENTACCESS_H

#pragma once

#include <QString>

// Cara run agent masuk ke Claude, dipilih di File > Integrations: login milik Claude Code (OAuth,
// disimpan CLI-nya sendiri) atau API key Anthropic yang disimpan aplikasi.
namespace AgentAccess {

enum class Method { Login, ApiKey };

struct Settings {
    Method method = Method::Login;
    QString apiKey;                    // hanya terisi di metode ApiKey
};

// Metode dari settings.ini, key dari SecretStore
Settings load();

// false + *error bila key tidak bisa disimpan; pilihan lama tetap berlaku. Metode Login menghapus
// key tersimpan: aplikasi tidak menyimpan rahasia yang tidak dipakainya.
bool save(const Settings &settings, QString *error = nullptr);

// Untuk ditampilkan: hanya empat karakter terakhir, mis. "…a1B2"
QString maskedKey(const QString &apiKey);

}

#endif // AGENTACCESS_H

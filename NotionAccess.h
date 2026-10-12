#ifndef NOTIONACCESS_H
#define NOTIONACCESS_H

#pragma once

#include <QString>

// Akses ke Notion untuk tombol "Ke Notion", diisi di File > Integrations: token integrasi internal
// (di SecretStore, tidak pernah di settings.ini) dan halaman induk yang terakhir dipakai.
namespace NotionAccess {

// Kosong bila belum diisi
QString token();

// false + *error bila token tidak bisa disimpan; token lama tetap berlaku
bool saveToken(const QString &token, QString *error = nullptr);

// Token dan halaman induk terakhir dibuang: halaman itu milik workspace token tersebut
void clear();

// Halaman induk ekspor terakhir, dipilih lebih dulu di ekspor berikutnya. id kosong = belum ada.
struct Parent {
    QString id;
    QString title;
};

Parent lastParent();
void saveLastParent(const Parent &parent);

}

#endif // NOTIONACCESS_H

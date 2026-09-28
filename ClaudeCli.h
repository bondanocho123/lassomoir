#ifndef CLAUDECLI_H
#define CLAUDECLI_H

#pragma once

#include "AgentDefinition.h"
#include "AgentTypes.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

// Kontrak pemanggilan Claude Code CLI: lokasi executable dan flag-nya.
// Flag dicocokkan dengan `claude --help` versi 2.1.266.
namespace ClaudeCli {

// Cari claude di PATH, lalu di ~/.local/bin (lokasi installer native). Kosong bila tidak ada.
QString findExecutable();

// Argumen `claude -p` untuk satu run. Isi task tidak pernah ikut di sini: dikirim lewat stdin.
QStringList arguments(const AgentDefinition &agent);

// Argumen untuk launch lengkap: argumen agent, izin baca folder di luar folder kerja
// (readRule untuk tiap readableDirectories), dan --input-format stream-json bila ada foto
QStringList arguments(const AgentLaunch &launch);

// Izin baca-saja satu folder beserta isinya: "E:\File Bondan\lib" -> "Read(//e/File Bondan/lib/**)".
// Aturan Read juga berlaku untuk Grep dan Glob. Edit/Write ke sana tetap ditolak: folder itu bukan
// folder kerja (sengaja bukan --add-dir, yang di mode acceptEdits ikut menerima edit) dan tidak
// ada yang bisa menyetujui prompt izin.
QString readRule(const QString &directory);

// Isi stdin untuk input stream-json: satu baris JSON pesan pengguna berisi tiap foto (didahului
// label namanya) lalu prompt. Foto yang tidak terbaca diganti catatan teks.
QByteArray userMessage(const QString &prompt, const QStringList &imagePaths);

}

#endif

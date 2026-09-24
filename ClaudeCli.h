#ifndef CLAUDECLI_H
#define CLAUDECLI_H

#pragma once

#include "AgentDefinition.h"

#include <QString>
#include <QStringList>

// Kontrak pemanggilan Claude Code CLI: lokasi executable dan flag-nya.
// Flag dicocokkan dengan `claude --help` versi 2.1.266.
namespace ClaudeCli {

// Cari claude di PATH, lalu di ~/.local/bin (lokasi installer native). Kosong bila tidak ada.
QString findExecutable();

// Argumen `claude -p` untuk satu run. Isi task tidak pernah ikut di sini: dikirim lewat stdin.
QStringList arguments(const AgentDefinition &agent);

}

#endif // CLAUDECLI_H

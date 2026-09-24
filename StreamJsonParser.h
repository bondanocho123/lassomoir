#ifndef STREAMJSONPARSER_H
#define STREAMJSONPARSER_H

#pragma once

#include "AgentTypes.h"

#include <QByteArray>
#include <QList>

// Pembaca output `claude -p --output-format stream-json`: satu baris JSON per event.
namespace StreamJsonParser {

// Satu baris stdout -> 0..n event (satu pesan assistant bisa berisi beberapa blok).
// Baris yang bukan JSON, dan tipe yang tidak dipakai (system, rate_limit_event, user,
// blok thinking, dll.) menghasilkan list kosong.
QList<AgentEvent> parseLine(const QByteArray &line);

}

#endif // STREAMJSONPARSER_H

#ifndef STAGEPROFILE_H
#define STAGEPROFILE_H

#pragma once

#include "AgentDefinition.h"
#include "TransitionPolicy.h"

#include <QString>
#include <memory>
#include <optional>

// Satu stage pipeline = key + agent miliknya (opsional) + aturan keluar.
// Konfigurasi agent dan gate sengaja dipisah: keduanya berubah karena alasan berbeda.
// WAITING dan DONE tidak punya agent, jadi agent() mengembalikan nullptr.
class StageProfile {
public:
    StageProfile(QString key,
                 std::optional<AgentDefinition> agent,
                 std::shared_ptr<const TransitionPolicy> exitPolicy);

    // Key UPPERCASE, sama dengan yang disimpan di session.json ("CODER")
    QString key() const { return m_key; }

    // nullptr = stage tanpa agent
    const AgentDefinition *agent() const { return m_agent ? &*m_agent : nullptr; }

    // Gate yang berlaku saat task keluar dari stage ini
    const TransitionPolicy &exitPolicy() const { return *m_exitPolicy; }

private:
    QString m_key;
    std::optional<AgentDefinition> m_agent;
    std::shared_ptr<const TransitionPolicy> m_exitPolicy;   // bisa dipakai bersama antar stage
};

#endif // STAGEPROFILE_H

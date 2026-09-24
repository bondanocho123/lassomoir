#ifndef AGENTDEFINITION_H
#define AGENTDEFINITION_H

#pragma once

#include <QString>
#include <QStringList>

// Konfigurasi agent milik satu stage. Hanya data: cara menjalankannya diurus
// AgentRuntime, sedangkan aturan keluar stage diurus TransitionPolicy.
struct AgentDefinition {
    QString rolePrompt;                // instruksi peran (:/prompts/<STAGE>.md) -> --append-system-prompt
    QStringList tools;                 // tool bawaan yang tersedia -> --tools
    QStringList allowedTools;          // izin tanpa prompt, mis. "Bash(git *)" -> --allowedTools
    QString effort;                    // "low" ... "max" -> --effort
    int maxConcurrent = 1;             // ukuran gerombolan: run paralel maksimal di stage ini
    int timeoutMs = 20 * 60 * 1000;    // batas waktu satu run

    // Agent yang bisa mengubah isi folder kerja wajib bergiliran lewat WorkspaceGuard
    bool writesWorkspace() const {
        static const QStringList writingTools = {"Edit", "Write", "Bash"};
        for (const QString &tool : writingTools) {
            if (tools.contains(tool)) {
                return true;
            }
        }
        return false;
    }
};

#endif // AGENTDEFINITION_H

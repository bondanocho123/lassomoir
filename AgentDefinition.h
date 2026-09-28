#ifndef AGENTDEFINITION_H
#define AGENTDEFINITION_H

#pragma once

#include <QString>
#include <QStringList>

// Pilihan model dan effort yang menimpa bawaan stage. Kosong = pakai bawaan stage.
struct AgentTuning {
    QString model;                     // alias CLI: "haiku", "sonnet", "opus", atau id model penuh
    QString effort;                    // "low" ... "max"

    bool isEmpty() const { return model.isEmpty() && effort.isEmpty(); }
};

// Konfigurasi agent milik satu stage. Hanya data: cara menjalankannya diurus
// AgentRuntime, sedangkan aturan keluar stage diurus TransitionPolicy.
struct AgentDefinition {
    QString rolePrompt;                // instruksi peran (:/prompts/<STAGE>.md) -> --append-system-prompt
    QStringList tools;                 // tool bawaan yang tersedia -> --tools
    QStringList allowedTools;          // izin tanpa prompt, mis. "Bash(git *)" -> --allowedTools
    QString model;                     // kosong = model bawaan CLI -> --model
    QString effort;                    // "low" ... "max" -> --effort
    int maxConcurrent = 1;             // ukuran gerombolan: run paralel maksimal di stage ini
    int timeoutMs = 20 * 60 * 1000;    // batas waktu satu run

    // Pilihan pengguna per task menimpa bawaan stage; field yang kosong dibiarkan
    void applyTuning(const AgentTuning &tuning) {
        if (!tuning.model.isEmpty()) {
            model = tuning.model;
        }
        if (!tuning.effort.isEmpty()) {
            effort = tuning.effort;
        }
    }

    // Tool yang bisa mengubah isi folder kerja
    static bool isWritingTool(const QString &tool) {
        static const QStringList writingTools = {"Edit", "Write", "Bash"};
        return writingTools.contains(tool);
    }

    // Agent yang bisa mengubah isi folder kerja wajib bergiliran lewat WorkspaceGuard
    bool writesWorkspace() const {
        for (const QString &tool : tools) {
            if (isWritingTool(tool)) {
                return true;
            }
        }
        return false;
    }
};

#endif // AGENTDEFINITION_H

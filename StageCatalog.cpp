#include "StageCatalog.h"

#include <QFile>

StageCatalog::StageCatalog(QList<StageProfile> profiles)
    : m_profiles(std::move(profiles)) {
}

StageCatalog StageCatalog::standard() {
    const auto gate = std::make_shared<ApprovalGate>();
    const auto autoAdvance = std::make_shared<AutoAdvance>();

    auto agent = [](const QString &key, const QStringList &tools, const QStringList &allowedTools,
                    const QString &effort, int maxConcurrent) {
        AgentDefinition definition;
        definition.rolePrompt = loadRolePrompt(key);
        definition.tools = tools;
        definition.allowedTools = allowedTools;
        definition.effort = effort;
        definition.maxConcurrent = maxConcurrent;
        return definition;
    };

    // SPECIFIER dan ARCHITECT sengaja tanpa Write: folder artefak belum ada dan hasilnya
    // berupa teks jawaban, jadi agent-nya read-only dan boleh jalan paralel dengan penulis.
    return StageCatalog({
        StageProfile(QStringLiteral("WAITING"), std::nullopt, autoAdvance),
        StageProfile(QStringLiteral("SPECIFIER"),
                     agent(QStringLiteral("SPECIFIER"), {"Read", "Grep", "Glob"}, {}, "high", 3),
                     gate),
        StageProfile(QStringLiteral("CODER"),
                     agent(QStringLiteral("CODER"), {"Read", "Grep", "Glob", "Edit", "Write", "Bash"},
                           {"Bash(git *)"}, "high", 2),
                     autoAdvance),
        StageProfile(QStringLiteral("CLEANER"),
                     agent(QStringLiteral("CLEANER"), {"Read", "Grep", "Glob", "Edit", "Bash"}, {}, "medium", 1),
                     autoAdvance),
        StageProfile(QStringLiteral("ARCHITECT"),
                     agent(QStringLiteral("ARCHITECT"), {"Read", "Grep", "Glob"}, {}, "high", 2),
                     autoAdvance),
        StageProfile(QStringLiteral("HARDENER"),
                     agent(QStringLiteral("HARDENER"), {"Read", "Grep", "Glob", "Edit", "Write", "Bash"}, {}, "high", 1),
                     autoAdvance),
        StageProfile(QStringLiteral("QA"),
                     agent(QStringLiteral("QA"), {"Read", "Grep", "Glob", "Bash"}, {}, "medium", 1),
                     gate),
        StageProfile(QStringLiteral("DONE"), std::nullopt, autoAdvance),
    });
}

const StageProfile *StageCatalog::profile(const QString &key) const {
    for (const StageProfile &profile : m_profiles) {
        if (profile.key() == key) {
            return &profile;
        }
    }
    return nullptr;
}

QStringList StageCatalog::keys() const {
    QStringList result;
    for (const StageProfile &profile : m_profiles) {
        result.append(profile.key());
    }
    return result;
}

QList<const StageProfile *> StageCatalog::agentStages() const {
    QList<const StageProfile *> result;
    for (const StageProfile &profile : m_profiles) {
        if (profile.agent()) {
            result.append(&profile);
        }
    }
    return result;
}

QString StageCatalog::loadRolePrompt(const QString &key) {
    QFile file(QStringLiteral(":/prompts/%1.md").arg(key));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return QString();
    }
    return QString::fromUtf8(file.readAll()).trimmed();
}

#include "StageProfile.h"
#include "TaskItem.h"

#include <QMap>

bool GatedStageProfile::canLeave(const TaskItem &task, QString *reason) const {
    if (task.approvals <= 0){
        if (reason){
            *reason = QStringLiteral("Stage %1 butuh approval sebelum bisa lanjut").arg(stageKey());
        }
        return false;
    }
    return true;
};

std::shared_ptr<StageProfile> stageProfileFor(const QString &stageKey) {
    static const QMap<QString, std::shared_ptr<StageProfile>> registry = {
        {QStringLiteral("WAITING"), std::make_shared<WaitingProfile>()},
        {QStringLiteral("SPECIFIER"), std::make_shared<SpecifierProfile>()},
        {QStringLiteral("CODER"), std::make_shared<CoderProfile>()},
        {QStringLiteral("CLEANER"), std::make_shared<CleanerProfile>()},
        {QStringLiteral("ARCHITECT"), std::make_shared<ArchitectProfile>()},
        {QStringLiteral("HARDENER"), std::make_shared<HardenerProfile>()},
        {QStringLiteral("QA"), std::make_shared<QAProfile>()},
        {QStringLiteral("DONE"), std::make_shared<DoneProfile>()},
    };

    return registry.value(stageKey, nullptr);
}

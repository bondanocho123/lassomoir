#include "TransitionPolicy.h"
#include "TaskItem.h"

bool ApprovalGate::canLeave(const TaskItem &task, QString *reason) const {
    const StageRun *run = task.latestRun(task.stage);
    if (run && run->decision == ReviewDecision::Approved) {
        return true;
    }
    if (reason) {
        *reason = QStringLiteral("stage %1 butuh review & persetujuan dulu").arg(task.stage);
    }
    return false;
}

bool AutoAdvance::canLeave(const TaskItem &task, QString *reason) const {
    Q_UNUSED(task);
    Q_UNUSED(reason);
    return true;
}

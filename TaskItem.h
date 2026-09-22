#ifndef TASKITEM_H
#define TASKITEM_H

#pragma once
#include <QString>

struct TaskItem {
    QString id;
    QString projectId; // "TTT", "spacewar"
    QString stage; // "WAITING", "CODER", "DONE", dll.
    QString category; // "component", "utility"
    QString title;
    QString subtext;
    QString badge; // "✓ 1", "✓ 0"
    int approvals = 0; // dicek GatedStageProfile::canLeave sebelum task boleh pindah stage
};

#endif // TASKITEM_H

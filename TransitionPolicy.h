#ifndef TRANSITIONPOLICY_H
#define TRANSITIONPOLICY_H

#pragma once

#include <QString>

struct TaskItem;

// Aturan boleh-tidaknya task keluar dari sebuah stage (gate).
// Dipasang ke StageProfile lewat komposisi, jadi gate bisa diganti per stage tanpa pewarisan.
class TransitionPolicy {
public:
    virtual ~TransitionPolicy() = default;

    // Stage ini butuh review manusia sebelum task lanjut?
    virtual bool isGated() const = 0;

    // Kembalikan false dan isi *reason untuk menolak perpindahan keluar stage
    virtual bool canLeave(const TaskItem &task, QString *reason) const = 0;
};

// Gate: task baru boleh keluar setelah run terakhirnya di stage ini disetujui (SPECIFIER, QA)
class ApprovalGate final : public TransitionPolicy {
public:
    bool isGated() const override { return true; }
    bool canLeave(const TaskItem &task, QString *reason) const override;
};

// Tanpa gate: task lanjut begitu kerjanya selesai
class AutoAdvance final : public TransitionPolicy {
public:
    bool isGated() const override { return false; }
    bool canLeave(const TaskItem &task, QString *reason) const override;
};

#endif // TRANSITIONPOLICY_H

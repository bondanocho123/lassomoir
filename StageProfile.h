#ifndef STAGEPROFILE_H
#define STAGEPROFILE_H
#pragma once

#include <QString>
#include <QStringList>
#include <memory>

struct TaskItem;

// Konfigurasi + perilaku satu stage pipeline.
// Nilai (tools/effort) beda antar stage -> cukup override getter di kelas daun.
// Perilaku gating beda ALGORITMA-nya (gated vs auto), jadi itu yang dibuat polimorfik
// lewat dua base class di bawah, bukan lewat if/switch(stage) di TaskManager.
class StageProfile {
public:
    virtual ~StageProfile() = default;

    virtual QString stageKey() const = 0;
    virtual QStringList tools() const = 0;
    virtual QStringList allowedTools() const = 0;
    virtual QString effort() const = 0;
    virtual bool isGated() const = 0;

    // Dipanggil TaskManager sebelum task diizinkan pindah KELUAR dari stage ini.
    // Kembalikan false dan isi *reason untuk menolak transisi.
    virtual bool canLeave(const TaskItem &task, QString *reason) const = 0;
};

// Stage yang butuh approval manusia sebelum boleh lanjut (mis. SPECIFIER, QA).
class GatedStageProfile : public StageProfile {
public:
    bool isGated() const override { return true; }
    bool canLeave(const TaskItem &task, QString *reason) const override;
};

// Stage yang lanjut begitu saja begitu kerjanya selesai (mis. CODER, CLEANER, ...).
class AutoStageProfile : public StageProfile {
public:
    bool isGated() const override { return false; }
    bool canLeave(const TaskItem &task, QString *reason) const override {
        Q_UNUSED(task);
        Q_UNUSED(reason);
        return true;
    }
};

class WaitingProfile final : public AutoStageProfile {
public:
    QString stageKey() const override { return QStringLiteral("WAITING"); }
    QStringList tools() const override { return {}; }
    QStringList allowedTools() const override { return {}; }
    QString effort() const override { return QString(); }
};

class SpecifierProfile final : public GatedStageProfile {
public:
    QString stageKey() const override { return QStringLiteral("SPECIFIER"); }
    QStringList tools() const override { return {"Read", "Grep", "Glob", "Write"}; }
    QStringList allowedTools() const override { return {}; }
    QString effort() const override { return QStringLiteral("high"); }
};

class CoderProfile final : public AutoStageProfile {
public:
    QString stageKey() const override { return QStringLiteral("CODER"); }
    QStringList tools() const override { return {"Read", "Grep", "Glob", "Edit", "Write", "Bash"}; }
    QStringList allowedTools() const override { return {"Bash(git *)"}; }
    QString effort() const override { return QStringLiteral("high"); }
};

class CleanerProfile final : public AutoStageProfile {
public:
    QString stageKey() const override { return QStringLiteral("CLEANER"); }
    QStringList tools() const override { return {"Read", "Grep", "Glob", "Edit", "Bash"}; }
    QStringList allowedTools() const override { return {}; }
    QString effort() const override { return QStringLiteral("medium"); }
};

class ArchitectProfile final : public AutoStageProfile {
public:
    QString stageKey() const override { return QStringLiteral("ARCHITECT"); }
    QStringList tools() const override { return {"Read", "Grep", "Glob", "Write"}; }
    QStringList allowedTools() const override { return {}; }
    QString effort() const override { return QStringLiteral("high"); }
};

class HardenerProfile final : public AutoStageProfile {
public:
    QString stageKey() const override { return QStringLiteral("HARDENER"); }
    QStringList tools() const override { return {"Read", "Grep", "Glob", "Edit", "Write", "Bash"}; }
    QStringList allowedTools() const override { return {}; }
    QString effort() const override { return QStringLiteral("high"); }
};

class QAProfile final : public GatedStageProfile {
public:
    QString stageKey() const override { return QStringLiteral("QA"); }
    QStringList tools() const override { return {"Read", "Grep", "Glob", "Bash"}; }
    QStringList allowedTools() const override { return {}; }
    QString effort() const override { return QStringLiteral("medium"); }
};

class DoneProfile final : public AutoStageProfile {
public:
    QString stageKey() const override { return QStringLiteral("DONE"); }
    QStringList tools() const override { return {}; }
    QStringList allowedTools() const override { return {}; }
    QString effort() const override { return QString(); }
};

// Satu titik resolve nama stage -> profile. Tambah stage baru = tambah satu baris di sini.
std::shared_ptr<StageProfile> stageProfileFor(const QString &stageKey);

#endif // STAGEPROFILE_H

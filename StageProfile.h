#ifndef STAGEPROFILE_H
#define STAGEPROFILE_H

#pragma once

#include <QString>
#include <QStringList>
#include <memory>

struct TaskItem;

class StageProfile
{
public:
    virtual ~StageProfile() = default;

    //identitas/key dari stage => ["QA", "SPECIFIER", "CODER", "CLEANER"...]
    virtual QString stageKey() const = 0;

    //Misalnya SPECIFIER cuma boleh Read, Grep, Glob, Write
    //(tidak boleh Bash, karena tugasnya cuma nulis spek, bukan eksekusi kode).
    virtual QStringList tools() const = 0;

    //Daftar izin tambahan yang lebih spesifik/granular di atas tools()
    virtual QStringList allowedTools() const = 0;

    //Level "usaha"/reasoning effort yang dipakai saat memanggil model untuk stage ini
    //(nilai seperti "high", "medium") — jadi argumen --effort.
    //Ini trade-off biaya vs kualitas per stage: stage yang butuh reasoning berat
    //(SPECIFIER, CODER, ARCHITECT, HARDENER) diberi "high", sedangkan yang lebih mekanis (CLEANER, QA) cukup "medium"
    //supaya lebih murah/cepat.
    virtual QString effort() const = 0;
    virtual bool isGated() const = 0;

    // Dipanggil TaskManager sebelum task diizinkan pindah KELUAR dari stage ini.
    // Kembalikan false dan isi *reason untuk menolak transisi.
    virtual bool canLeave(const TaskItem &task, QString *reason) const = 0;
};

// Stage yang butuh approval manusia sebelum boleh lanjut (mis. SPECIFIER, QA).
class GatedStageProfile : public StageProfile {

public:
    bool isGated() const override { return true;}
    bool canLeave(const TaskItem &task, QString *reason) const override;
};

// Stage yang lanjut begitu saja begitu kerjanya selesai (mis. CODER, CLEANER, ...).
class AutoStageProfile : public StageProfile {
    bool isGated() const override { return false;}
    bool canLeave(const TaskItem &task, QString *reason) const override{
        Q_UNUSED(task);
        Q_UNUSED(reason);

        return true;
    };
};

class WaitingProfile : public AutoStageProfile {
public:
    QString stageKey() const override {return QStringLiteral("WAITING");}
    QStringList tools() const override { return {}; }
    QStringList allowedTools() const override { return {}; }
    QString effort() const override { return QString();}
};

class SpecifierProfile : public GatedStageProfile {
public:
    QString stageKey() const override {return QStringLiteral("SPECIFIER");}
    QStringList tools() const override { return { "Read", "Grep", "Glob", "Write" }; }
    QStringList allowedTools() const override { return {}; }
    QString effort() const override { return QStringLiteral("high");}
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

std::shared_ptr<StageProfile> stageProfileFor(const QString &stageKey);

#endif // STAGEPROFILE_H

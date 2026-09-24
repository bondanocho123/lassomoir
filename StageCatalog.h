#ifndef STAGECATALOG_H
#define STAGECATALOG_H

#pragma once

#include "StageProfile.h"

#include <QList>
#include <QString>
#include <QStringList>

// Daftar stage pipeline beserta nilai awalnya, urut dari WAITING sampai DONE.
// Satu-satunya sumber urutan & key stage: dipakai swimlane, dialog New Task,
// TaskManager, dan SwarmCoordinator.
class StageCatalog {
public:
    // 8 stage bawaan; instruksi peran dibaca dari :/prompts/<STAGE>.md
    static StageCatalog standard();

    explicit StageCatalog(QList<StageProfile> profiles);

    // nullptr bila key tidak dikenal
    const StageProfile *profile(const QString &key) const;

    // Urutan kolom pipeline
    QStringList keys() const;

    // Stage yang punya agent; SwarmCoordinator membuat satu StageSwarm per item
    QList<const StageProfile *> agentStages() const;

private:
    // Isi :/prompts/<key>.md; kosong bila tidak ada
    static QString loadRolePrompt(const QString &key);

    QList<StageProfile> m_profiles;   // disimpan sesuai urutan pipeline
};

#endif // STAGECATALOG_H

#ifndef SWARMCOORDINATOR_H
#define SWARMCOORDINATOR_H

#pragma once

#include "AgentTypes.h"
#include "TaskItem.h"
#include "TaskMaterials.h"
#include "WorkspaceGuard.h"

#include <QList>
#include <QObject>
#include <QString>

class AgentRuntime;
class PromptComposer;
class StageCatalog;
class StageSwarm;

// Pintu masuk semua run agent: memeriksa prasyarat, lalu mengarahkan task ke
// gerombolan (StageSwarm) milik stage-nya. Dirakit di main.cpp (composition root).
class SwarmCoordinator : public QObject {
    Q_OBJECT

public:
    // Membuat satu StageSwarm untuk tiap stage yang punya agent
    SwarmCoordinator(const StageCatalog &catalog, AgentRuntime &runtime,
                     const PromptComposer &composer, QObject *parent = nullptr);

    // Kembalikan false dan isi *reason bila ditolak: stage tanpa agent, task sudah
    // berjalan/antre, runtime tidak tersedia, atau folder kerja tidak ada
    bool run(const TaskItem &task, const QString &workingDirectory, QString *reason = nullptr);
    // Sama, dengan lampiran dan folder referensi yang sudah dibaca (TaskAttachments::materials):
    // isinya masuk prompt, fotonya ikut sebagai blok gambar, foldernya boleh dibaca agent
    bool run(const TaskItem &task, const QString &workingDirectory, const TaskMaterials &materials,
             QString *reason = nullptr);

    void cancel(const QString &taskId);

    // Dipanggil saat project ditutup atau dihapus
    void cancelProject(const QString &projectId);

    RunState state(const QString &taskId) const;

    // Sedang ada agent penulis di folder ini? (mis. sebelum aplikasi sendiri menjalankan git merge di sana)
    bool isWriting(const QString &directory) const;

signals:
    // Diteruskan dari semua gerombolan
    void runQueued(const TaskItem &task);
    void runStarted(const TaskItem &task, const AgentLaunch &launch);
    void runEvent(const TaskItem &task, const AgentEvent &event);
    void runFinished(const TaskItem &task, const AgentResult &result);

private:
    StageSwarm *swarmFor(const QString &stageKey) const;     // nullptr = stage tanpa agent
    StageSwarm *swarmHolding(const QString &taskId) const;   // gerombolan yang sedang memegang task
    void pumpAll();

    const StageCatalog &m_catalog;
    AgentRuntime &m_runtime;
    const PromptComposer &m_composer;
    WorkspaceGuard m_guard;             // satu untuk semua gerombolan
    QList<StageSwarm *> m_swarms;       // urutan pipeline; child QObject
};

#endif // SWARMCOORDINATOR_H

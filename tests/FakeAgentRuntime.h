#ifndef FAKEAGENTRUNTIME_H
#define FAKEAGENTRUNTIME_H

#pragma once

#include "AgentRuntime.h"

#include <QList>
#include <QPointer>

// Session palsu yang memenuhi kontrak AgentSession tanpa menjalankan proses.
// Test yang memutuskan kapan event dan hasil akhir dipancarkan.
class FakeAgentSession final : public AgentSession {
    Q_OBJECT

public:
    FakeAgentSession(AgentLaunch launch, bool finishOnStart, QObject *parent)
        : AgentSession(parent), m_launch(std::move(launch)), m_finishOnStart(finishOnStart) {
    }

    void start() override {
        m_started = true;
        if (m_finishOnStart) {
            AgentResult result;
            result.success = true;
            result.outcome = QStringLiteral("success");
            finishWith(result);
        }
    }

    void cancel() override {
        finishWith(AgentResult::failure(QStringLiteral("cancelled"), QStringLiteral("dibatalkan")));
    }

    bool isRunning() const override { return m_started && !m_done; }

    void emitEvent(const AgentEvent &event) {
        if (!m_done) {
            emit eventReceived(event);
        }
    }

    // Panggilan kedua diabaikan, sama seperti session asli
    void finishWith(const AgentResult &result) {
        if (m_done) {
            return;
        }
        m_done = true;
        emit finished(result);
    }

    const AgentLaunch &launch() const { return m_launch; }

private:
    AgentLaunch m_launch;
    bool m_finishOnStart;
    bool m_started = false;
    bool m_done = false;
};

class FakeAgentRuntime final : public AgentRuntime {
public:
    bool isAvailable(QString *reason) const override {
        if (!available && reason) {
            *reason = QStringLiteral("runtime palsu dimatikan");
        }
        return available;
    }

    AgentSession *createSession(const AgentLaunch &launch, QObject *parent) override {
        auto *session = new FakeAgentSession(launch, finishOnStart, parent);
        sessions.append(session);
        return session;
    }

    RuntimeCheck check() const override { return checkResult; }

    // Run gagal (selain dibatalkan) ditafsirkan sebagai failureDiagnosis
    RuntimeCheck diagnose(const AgentResult &result) const override {
        if (result.success || result.outcome == QLatin1String("cancelled")) {
            return RuntimeCheck();
        }
        return failureDiagnosis;
    }

    RuntimeCheck checkResult;                     // jawaban pemeriksaan saat aplikasi dibuka
    RuntimeCheck failureDiagnosis;                // tafsiran run yang gagal
    bool available = true;                        // uji penolakan "runtime tidak tersedia"
    bool finishOnStart = false;                   // uji session yang selesai di dalam start()
    QList<QPointer<FakeAgentSession>> sessions;   // semua session yang pernah dibuat
};

#endif // FAKEAGENTRUNTIME_H

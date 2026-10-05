#include "CanvasAutomation.h"
#include "AgentRuntime.h"
#include "CanvasModel.h"

#include <QDateTime>

#include <algorithm>

namespace {

bool fail(QString *reason, const QString &why) {
    if (reason) {
        *reason = why;
    }
    return false;
}

}

CanvasAutomation::CanvasAutomation(CanvasModel &model, AgentRuntime &runtime, LaunchBuilder builder, QObject *parent)
    : QObject(parent), m_model(model), m_runtime(runtime), m_builder(std::move(builder)) {
    // Kartu yang dihapus (juga lewat undo) tidak boleh tetap berjalan atau menunggu giliran
    connect(&m_model, &CanvasModel::nodeRemoved, this, [this](const QString &id) { cancel(id); });
    connect(&m_model, &CanvasModel::boardReset, this, &CanvasAutomation::dropMissingSteps);
}

bool CanvasAutomation::run(const QString &stepId, QString *reason) {
    const CanvasNode *node = m_model.node(stepId);
    if (!node || node->kind != CanvasNodeKind::Step) {
        return fail(reason, QStringLiteral("langkah AI tidak ditemukan"));
    }
    if (state(stepId) != RunState::Idle) {
        return fail(reason, QStringLiteral("langkah ini sudah berjalan atau sedang antre"));
    }
    return enqueue({stepId}, reason);
}

bool CanvasAutomation::runWithUpstream(const QString &stepId, QString *reason) {
    const CanvasNode *node = m_model.node(stepId);
    if (!node || node->kind != CanvasNodeKind::Step) {
        return fail(reason, QStringLiteral("langkah AI tidak ditemukan"));
    }
    return enqueue(m_model.board().withUpstream(stepId), reason);
}

bool CanvasAutomation::runAll(QString *reason) {
    const QStringList steps = m_model.board().stepIds();
    if (steps.isEmpty()) {
        return fail(reason, QStringLiteral("belum ada langkah AI di kanvas ini"));
    }
    return enqueue(m_model.board().stepOrder(steps), reason);
}

bool CanvasAutomation::enqueue(const QStringList &order, QString *reason) {
    QString why;
    if (!m_runtime.isAvailable(&why)) {
        return fail(reason, why);
    }
    QStringList added;
    for (const QString &id : order) {
        if (state(id) != RunState::Idle) {
            continue;
        }
        Pending pending;
        pending.stepId = id;
        // Hulu yang sudah antre (termasuk yang barusan ditambahkan di atasnya) atau sedang berjalan
        const QStringList upstream = m_model.board().upstreamSteps(id);
        for (const QString &up : upstream) {
            if (state(up) != RunState::Idle) {
                pending.waitingFor.append(up);
            }
        }
        m_queue.append(pending);
        added.append(id);
        emit stateChanged(id, RunState::Queued);
    }
    if (added.isEmpty()) {
        return fail(reason, QStringLiteral("semua langkah itu sudah berjalan atau sedang antre"));
    }
    pump();
    return true;
}

void CanvasAutomation::cancel(const QString &stepId) {
    for (qsizetype i = 0; i < m_queue.size(); ++i) {
        if (m_queue.at(i).stepId == stepId) {
            m_queue.removeAt(i);
            settle(stepId, AgentResult::failure(QStringLiteral("cancelled"), QStringLiteral("dibatalkan sebelum mulai")),
                   false);
            return;
        }
    }
    // finished("cancelled") menyusul, bisa langsung di dalam cancel()
    if (AgentSession *session = m_active.value(stepId)) {
        session->cancel();
    }
}

void CanvasAutomation::cancelAll() {
    // Antrean dulu: run yang berhenti memicu pump(), dan saat itu tidak boleh ada yang menunggu giliran
    QStringList queued;
    for (const Pending &pending : std::as_const(m_queue)) {
        queued.append(pending.stepId);
    }
    for (const QString &id : std::as_const(queued)) {
        cancel(id);
    }
    const QStringList running = m_active.keys();
    for (const QString &id : running) {
        cancel(id);
    }
}

RunState CanvasAutomation::state(const QString &stepId) const {
    if (m_active.contains(stepId)) {
        return RunState::Running;
    }
    const bool queued = std::any_of(m_queue.cbegin(), m_queue.cend(), [&stepId](const Pending &pending) {
        return pending.stepId == stepId;
    });
    return queued ? RunState::Queued : RunState::Idle;
}

void CanvasAutomation::pump() {
    // start() dan settle() bisa memanggil pump() lagi (session yang selesai di dalam start()):
    // panggilan itu cukup ditandai, putaran di bawah yang meneruskannya
    if (m_pumping) {
        m_pumpAgain = true;
        return;
    }
    m_pumping = true;
    do {
        m_pumpAgain = false;
        while (m_active.size() < m_maxConcurrent) {
            const auto next = std::find_if(m_queue.begin(), m_queue.end(), [](const Pending &pending) {
                return pending.waitingFor.isEmpty();
            });
            if (next == m_queue.end()) {
                break;
            }
            const QString stepId = next->stepId;
            m_queue.erase(next);
            start(stepId);
        }
    } while (m_pumpAgain);
    m_pumping = false;

    if (m_queue.isEmpty() && m_active.isEmpty() && m_succeeded + m_failed + m_skipped > 0) {
        const int succeeded = m_succeeded;
        const int failed = m_failed;
        const int skipped = m_skipped;
        m_succeeded = m_failed = m_skipped = 0;
        emit batchFinished(succeeded, failed, skipped);
    }
}

void CanvasAutomation::start(const QString &stepId) {
    QString reason;
    const std::optional<AgentLaunch> launch = m_builder ? m_builder(stepId, &reason) : std::nullopt;
    if (!launch) {
        settle(stepId,
               AgentResult::failure(QStringLiteral("rejected"),
                                    reason.isEmpty() ? QStringLiteral("langkah tidak bisa disiapkan") : reason),
               false);
        return;
    }

    AgentSession *session = m_runtime.createSession(*launch, this);
    // Didaftarkan sebelum start(): session boleh selesai langsung di dalam start()
    m_active.insert(stepId, session);
    connect(session, &AgentSession::eventReceived, this, [this, stepId](const AgentEvent &event) {
        emit stepEvent(stepId, event);
    });
    connect(session, &AgentSession::finished, this, [this, stepId, session](const AgentResult &result) {
        if (m_active.value(stepId) != session) {
            return;
        }
        m_active.remove(stepId);
        session->deleteLater();
        settle(stepId, result, result.outcome != QLatin1String("cancelled"));
    });
    emit stateChanged(stepId, RunState::Running);
    emit stepStarted(stepId, *launch);
    session->start();
}

void CanvasAutomation::settle(const QString &stepId, const AgentResult &result, bool record) {
    if (record) {
        m_model.setStepResult(stepId, result, QDateTime::currentDateTimeUtc());
    }
    if (result.success) {
        ++m_succeeded;
    } else if (result.outcome == QLatin1String("skipped")) {
        ++m_skipped;
    } else {
        ++m_failed;
    }
    emit stateChanged(stepId, RunState::Idle);
    emit stepFinished(stepId, result);

    QStringList blocked;
    for (Pending &pending : m_queue) {
        if (pending.waitingFor.removeAll(stepId) > 0 && !result.success) {
            blocked.append(pending.stepId);
        }
    }
    const QString why = result.outcome == QLatin1String("cancelled")
                            ? QStringLiteral("dilewati: langkah hulunya dibatalkan")
                            : QStringLiteral("dilewati: langkah hulunya tidak berhasil");
    for (const QString &id : std::as_const(blocked)) {
        const auto it = std::find_if(m_queue.begin(), m_queue.end(), [&id](const Pending &pending) {
            return pending.stepId == id;
        });
        if (it == m_queue.end()) {
            continue;   // sudah dilewati lewat hulu lain
        }
        m_queue.erase(it);
        settle(id, AgentResult::failure(QStringLiteral("skipped"), why), false);
    }
    pump();
}

void CanvasAutomation::dropMissingSteps() {
    QStringList missing;
    for (const Pending &pending : std::as_const(m_queue)) {
        if (!m_model.node(pending.stepId)) {
            missing.append(pending.stepId);
        }
    }
    for (auto it = m_active.cbegin(); it != m_active.cend(); ++it) {
        if (!m_model.node(it.key())) {
            missing.append(it.key());
        }
    }
    for (const QString &id : std::as_const(missing)) {
        cancel(id);
    }
}

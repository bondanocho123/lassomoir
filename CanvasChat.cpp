#include "CanvasChat.h"
#include "AgentRuntime.h"
#include "CanvasModel.h"
#include "CanvasWorkflow.h"
#include "RunLogFormatter.h"

#include <QUuid>

namespace {

QString newId() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

bool fail(QString *reason, const QString &why) {
    if (reason) {
        *reason = why;
    }
    return false;
}

}

QJsonObject CanvasChatMessage::toJson() const {
    QJsonObject obj;
    obj[QStringLiteral("id")] = id;
    obj[QStringLiteral("role")] = isUser() ? QStringLiteral("user") : QStringLiteral("agent");
    obj[QStringLiteral("text")] = text;
    obj[QStringLiteral("at")] = at.toString(Qt::ISODate);
    if (isUser()) {
        if (!contextIds.isEmpty()) {
            obj[QStringLiteral("context")] = QJsonArray::fromStringList(contextIds);
        }
        obj[QStringLiteral("contextLabel")] = contextLabel;
        return obj;
    }
    obj[QStringLiteral("replyTo")] = replyTo;
    obj[QStringLiteral("model")] = model;
    obj[QStringLiteral("outcome")] = outcome;
    obj[QStringLiteral("durationMs")] = durationMs;
    obj[QStringLiteral("totalTokens")] = totalTokens;
    obj[QStringLiteral("costUsd")] = costUsd;
    return obj;
}

std::optional<CanvasChatMessage> CanvasChatMessage::fromJson(const QJsonObject &object) {
    const QString role = object.value(QStringLiteral("role")).toString();
    if (role != QLatin1String("user") && role != QLatin1String("agent")) {
        return std::nullopt;
    }
    CanvasChatMessage message;
    message.id = object.value(QStringLiteral("id")).toString();
    if (message.id.isEmpty()) {
        message.id = newId();
    }
    message.role = role == QLatin1String("user") ? Role::User : Role::Agent;
    message.text = object.value(QStringLiteral("text")).toString();
    message.at = QDateTime::fromString(object.value(QStringLiteral("at")).toString(), Qt::ISODate);
    const QJsonArray context = object.value(QStringLiteral("context")).toArray();
    for (const QJsonValue &id : context) {
        if (id.isString()) {
            message.contextIds.append(id.toString());
        }
    }
    message.contextLabel = object.value(QStringLiteral("contextLabel")).toString();
    message.replyTo = object.value(QStringLiteral("replyTo")).toString();
    message.model = object.value(QStringLiteral("model")).toString();
    message.outcome = object.value(QStringLiteral("outcome")).toString();
    message.durationMs = object.value(QStringLiteral("durationMs")).toInteger();
    message.totalTokens = object.value(QStringLiteral("totalTokens")).toInteger();
    message.costUsd = object.value(QStringLiteral("costUsd")).toDouble();
    return message;
}

CanvasChat::CanvasChat(CanvasModel &model, AgentRuntime &runtime, LaunchBuilder builder, QObject *parent)
    : QObject(parent), m_model(model), m_runtime(runtime), m_builder(std::move(builder)) {
}

const CanvasChatMessage *CanvasChat::message(const QString &id) const {
    for (const CanvasChatMessage &message : m_messages) {
        if (message.id == id) {
            return &message;
        }
    }
    return nullptr;
}

void CanvasChat::load(const QList<CanvasChatMessage> &messages) {
    if (isBusy()) {
        return;
    }
    m_messages = messages.mid(qMax<qsizetype>(0, messages.size() - kMaxMessages));
    emit messagesReset();
}

QJsonArray CanvasChat::toJson() const {
    QJsonArray array;
    for (const CanvasChatMessage &message : m_messages) {
        array.append(message.toJson());
    }
    return array;
}

QList<CanvasChatMessage> CanvasChat::fromJson(const QJsonArray &array) {
    QList<CanvasChatMessage> messages;
    for (const QJsonValue &value : array) {
        if (const std::optional<CanvasChatMessage> message = CanvasChatMessage::fromJson(value.toObject())) {
            messages.append(*message);
        }
    }
    return messages;
}

bool CanvasChat::ask(const QString &question, const QStringList &contextIds, const QString &model, QString *reason) {
    const QString text = question.trimmed();
    if (text.isEmpty()) {
        return fail(reason, QStringLiteral("tulis pertanyaannya dulu"));
    }
    if (isBusy()) {
        return fail(reason, QStringLiteral("agent masih menjawab pertanyaan sebelumnya"));
    }
    const CanvasBoard &board = m_model.board();
    if (!contextIds.isEmpty() && CanvasWorkflow::chatContext(board, contextIds).isEmpty()) {
        return fail(reason, QStringLiteral("kartu bahannya sudah tidak ada di kanvas"));
    }
    QString why;
    if (!m_runtime.isAvailable(&why)) {
        return fail(reason, why);
    }
    std::optional<AgentLaunch> launch = m_builder ? m_builder(contextIds, &why) : std::nullopt;
    if (!launch) {
        return fail(reason, why.isEmpty() ? QStringLiteral("chat tidak bisa disiapkan") : why);
    }

    // Percakapan sampai sebelum pertanyaan ini; jawaban yang gagal tidak ikut
    QList<CanvasWorkflow::ChatTurn> history;
    for (const CanvasChatMessage &message : std::as_const(m_messages)) {
        if (message.isUser() || message.succeeded()) {
            history.append({message.isUser(), message.text});
        }
    }
    launch->agent = CanvasWorkflow::chatAgent(model);
    launch->prompt = CanvasWorkflow::chatPrompt(board, contextIds, history, text);

    CanvasChatMessage asked;
    asked.id = newId();
    asked.role = CanvasChatMessage::Role::User;
    asked.text = text;
    asked.at = QDateTime::currentDateTimeUtc();
    // Kosong = seluruh kanvas; selain itu hanya kartu yang memang ada
    asked.contextIds = contextIds.isEmpty() ? QStringList() : CanvasWorkflow::chatContext(board, contextIds);
    asked.contextLabel = CanvasWorkflow::chatContextLabel(board, contextIds);
    append(asked);

    m_live.clear();
    m_activity.clear();
    m_workingDirectory = launch->workingDirectory;
    AgentSession *session = m_runtime.createSession(*launch, this);
    // Didaftarkan sebelum start(): session boleh selesai langsung di dalam start()
    m_session = session;
    const QString questionId = asked.id;
    const QString agentModel = launch->agent.model;
    connect(session, &AgentSession::eventReceived, this, [this, session](const AgentEvent &event) {
        if (m_session != session) {
            return;
        }
        if (event.kind == AgentEvent::Kind::Text && !event.text.trimmed().isEmpty()) {
            m_live += (m_live.isEmpty() ? QString() : QStringLiteral("\n\n")) + event.text.trimmed();
        } else if (event.kind == AgentEvent::Kind::ToolUse) {
            m_activity = RunLogFormatter::toolLabel(event, m_workingDirectory);
        } else {
            return;
        }
        emit liveChanged(m_live, m_activity);
    });
    connect(session, &AgentSession::finished, this,
            [this, session, questionId, agentModel](const AgentResult &result) {
        if (m_session != session) {
            return;
        }
        m_session = nullptr;
        session->deleteLater();
        finish(questionId, agentModel, result);
    });
    emit busyChanged(true);
    emit started(*launch);
    session->start();
    return true;
}

void CanvasChat::finish(const QString &questionId, const QString &model, const AgentResult &result) {
    CanvasChatMessage answer;
    answer.id = newId();
    answer.role = CanvasChatMessage::Role::Agent;
    answer.at = QDateTime::currentDateTimeUtc();
    answer.replyTo = questionId;
    answer.model = model;
    answer.outcome = result.success ? QStringLiteral("success")
                                    : (result.outcome.isEmpty() ? QStringLiteral("no_result") : result.outcome);
    answer.durationMs = result.durationMs;
    answer.totalTokens = result.totalTokens;
    answer.costUsd = result.costUsd;
    if (result.success) {
        answer.text = result.message.trimmed().isEmpty() ? m_live : result.message.trimmed();
        if (answer.text.isEmpty()) {
            answer.text = QStringLiteral("_Agent selesai tanpa menulis jawaban._");
        }
    } else if (answer.outcome == QLatin1String("cancelled")) {
        answer.text = m_live;   // potongan jawaban yang sudah masuk tetap bisa dibaca
    } else {
        answer.text = result.message.trimmed();
    }
    m_live.clear();
    m_activity.clear();
    append(answer);
    emit busyChanged(false);
    emit finished(answer, result);
}

void CanvasChat::append(const CanvasChatMessage &message) {
    m_messages.append(message);
    bool trimmed = false;
    // Pesan terlama dibuang; percakapan selalu mulai dari sebuah pertanyaan
    while (m_messages.size() > kMaxMessages || (!m_messages.isEmpty() && !m_messages.first().isUser())) {
        m_messages.removeFirst();
        trimmed = true;
    }
    if (trimmed) {
        emit messagesReset();
    } else {
        emit messageAdded(message);
    }
    emit changed();
}

void CanvasChat::cancel() {
    if (AgentSession *session = m_session.data()) {
        session->cancel();
    }
}

bool CanvasChat::clear() {
    if (isBusy()) {
        return false;
    }
    if (m_messages.isEmpty()) {
        return true;
    }
    m_messages.clear();
    emit messagesReset();
    emit changed();
    return true;
}

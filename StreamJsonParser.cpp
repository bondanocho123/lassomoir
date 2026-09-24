#include "StreamJsonParser.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <optional>

namespace {

// Keterangan singkat input tool: path file, pola, atau perintah
QString toolDetail(const QJsonObject &input) {
    static const char *const keys[] = {"file_path", "path", "pattern", "command", "url", "description"};
    for (const char *key : keys) {
        const QString value = input.value(QLatin1String(key)).toString();
        if (!value.isEmpty()) {
            return value;
        }
    }
    return QString();
}

std::optional<AgentEvent> parseContentBlock(const QJsonObject &block) {
    const QString type = block.value(QLatin1String("type")).toString();

    AgentEvent event;
    if (type == QLatin1String("text")) {
        event.kind = AgentEvent::Kind::Text;
        event.text = block.value(QLatin1String("text")).toString().trimmed();
        if (event.text.isEmpty()) {
            return std::nullopt;
        }
        return event;
    }
    if (type == QLatin1String("tool_use")) {
        event.kind = AgentEvent::Kind::ToolUse;
        event.toolName = block.value(QLatin1String("name")).toString();
        event.toolDetail = toolDetail(block.value(QLatin1String("input")).toObject());
        return event;
    }
    return std::nullopt;   // thinking dan blok lain tidak ditampilkan
}

AgentResult parseResult(const QJsonObject &object) {
    AgentResult result;
    result.outcome = object.value(QLatin1String("subtype")).toString();
    // Denial tidak membuat run gagal: subtype tetap "success", hanya dicatat di deniedTools
    result.success = result.outcome == QLatin1String("success")
                     && !object.value(QLatin1String("is_error")).toBool();
    result.message = object.value(QLatin1String("result")).toString();
    result.sessionId = object.value(QLatin1String("session_id")).toString();
    result.durationMs = object.value(QLatin1String("duration_ms")).toInteger();
    result.costUsd = object.value(QLatin1String("total_cost_usd")).toDouble();

    const QJsonObject usage = object.value(QLatin1String("usage")).toObject();
    result.totalTokens = usage.value(QLatin1String("input_tokens")).toInteger()
                         + usage.value(QLatin1String("cache_creation_input_tokens")).toInteger()
                         + usage.value(QLatin1String("cache_read_input_tokens")).toInteger()
                         + usage.value(QLatin1String("output_tokens")).toInteger();

    const QJsonArray denials = object.value(QLatin1String("permission_denials")).toArray();
    for (const QJsonValue &denial : denials) {
        result.deniedTools.append(denial.toObject().value(QLatin1String("tool_name")).toString());
    }
    return result;
}

}

QList<AgentEvent> StreamJsonParser::parseLine(const QByteArray &line) {
    QList<AgentEvent> events;

    const QByteArray trimmed = line.trimmed();
    if (trimmed.isEmpty()) {
        return events;
    }

    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(trimmed, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return events;
    }

    const QJsonObject object = document.object();
    const QString type = object.value(QLatin1String("type")).toString();

    if (type == QLatin1String("assistant")) {
        const QJsonArray content = object.value(QLatin1String("message")).toObject()
                                       .value(QLatin1String("content")).toArray();
        for (const QJsonValue &block : content) {
            if (const std::optional<AgentEvent> event = parseContentBlock(block.toObject())) {
                events.append(*event);
            }
        }
    } else if (type == QLatin1String("result")) {
        AgentEvent event;
        event.kind = AgentEvent::Kind::Result;
        event.result = parseResult(object);
        events.append(event);
    }
    return events;
}

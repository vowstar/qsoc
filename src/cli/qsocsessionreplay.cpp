// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "cli/qsocsessionreplay.h"
#include "common/qsocmessageauthority.h"
#include "common/qsoctoolresultstatus.h"

#include <nlohmann/json.hpp>

#include <QBuffer>
#include <QHash>
#include <QImageReader>
#include <QSet>
#include <QStringList>

namespace QSocSessionReplay {

namespace {

using json  = nlohmann::json;
using Event = QSocAgentRuntimeEvent;
using Kind  = QSocAgentRuntimeEvent::Kind;

const QString kSummaryPrefix = QStringLiteral("[Conversation Summary]\n");
const QString kNotification  = QStringLiteral("<task-notification>");

constexpr qsizetype kImageBytes       = 8 * 1024 * 1024;
constexpr qint64    kReplayImageBytes = 64 * 1024 * 1024;

const json &itemsOf(const json &value)
{
    static const json empty = json::array();
    return value.is_array() ? value : empty;
}

struct PendingTool
{
    QString name;
    QString arguments;
};

QString stringValue(const json &object, const char *key)
{
    const auto it = object.find(key);
    if (it == object.end() || !it->is_string())
        return {};
    return QString::fromStdString(it->get<std::string>());
}

QString textOf(const json &content)
{
    if (content.is_string())
        return QString::fromStdString(content.get<std::string>());
    QStringList parts;
    for (const json &part : itemsOf(content)) {
        if (part.is_object() && stringValue(part, "type") == QStringLiteral("text"))
            parts.append(stringValue(part, "text"));
    }
    return parts.join(QLatin1Char('\n'));
}

QString argumentsOf(const json &function)
{
    const auto it = function.find("arguments");
    if (it == function.end())
        return QStringLiteral("{}");
    return it->is_string() ? QString::fromStdString(it->get<std::string>())
                           : QString::fromStdString(it->dump());
}

QString toolStatus(const json &message, const QString &body)
{
    static const QStringList known{
        QStringLiteral("ok"),
        QStringLiteral("failed"),
        QStringLiteral("uncertain"),
        QStringLiteral("dispatched"),
        QStringLiteral("skipped"),
    };
    for (const char *key : {"_qsoc_status", "_qsoc_tool_state"}) {
        const QString value = stringValue(message, key);
        if (known.contains(value))
            return value;
    }
    return QSocToolResult::name(QSocToolResult::classify(body));
}

/* The saved-output notice is one line naming the artifact; show a short hint instead. */
QString withArtifactHint(const json &message, const QString &body)
{
    const auto refs = message.find("_qsoc_artifact_refs");
    if (refs == message.end() || !refs->is_array() || refs->empty() || !refs->front().is_object())
        return body;
    const json   &ref   = refs->front();
    const QString id    = stringValue(ref, "artifact_id");
    const auto    bytes = ref.find("captured_bytes");
    if (id.isEmpty())
        return body;
    const QString hint
        = QStringLiteral("(full output saved: %1, %2 bytes)")
              .arg(id)
              .arg(bytes != ref.end() && bytes->is_number() ? bytes->get<qint64>() : 0);
    QStringList lines = body.split(QLatin1Char('\n'));
    for (qsizetype i = lines.size() - 1; i >= 0; --i) {
        if (lines.at(i).contains(id)) {
            lines[i] = hint;
            return lines.join(QLatin1Char('\n'));
        }
    }
    return body + QLatin1Char('\n') + hint;
}

bool isSyntheticUserMessage(const QString &content)
{
    static const QStringList prefixes{
        QStringLiteral("<goal_context>"),
        QStringLiteral("You are in plan mode and ended your turn without calling a tool."),
        QStringLiteral("[System: Context compacted."),
        QStringLiteral("[Restored file after compaction:"),
        QStringLiteral("[Referenced file after compaction:"),
        QStringLiteral("[Skills restored after compaction]"),
        QStringLiteral("[Background agents still running after compaction]"),
    };
    for (const QString &prefix : prefixes) {
        if (content.startsWith(prefix))
            return true;
    }
    return false;
}

class Builder
{
public:
    void         add(const json &message);
    QList<Event> take();

private:
    void   user(const json &message);
    void   assistant(const json &message);
    void   tool(const json &message);
    void   images(const json &content);
    void   startBatch(const json &calls);
    void   endTurn();
    Event &push(
        Kind kind, const QString &text, const QString &secondary = {}, const QString &id = {});

    QList<Event>                out;
    QHash<QString, PendingTool> pending;
    QSet<QString>               ambiguous;
    bool                        open       = false;
    qint64                      imageBytes = 0;
};

void Builder::add(const json &message)
{
    const QString role = message.is_object() ? stringValue(message, "role") : QString();
    if (role == QStringLiteral("tool")) {
        tool(message);
        return;
    }
    pending.clear();
    ambiguous.clear();
    if (role == QStringLiteral("assistant"))
        assistant(message);
    else if (role == QStringLiteral("user"))
        user(message);
}

QList<Event> Builder::take()
{
    endTurn();
    return std::move(out);
}

void Builder::user(const json &message)
{
    const auto content = message.find("content");
    if (QSocMessageAuthority::isRuntimeReminder(message) || content == message.end())
        return;
    const QString text = textOf(*content);
    if (!QSocMessageAuthority::isUserRequest(message) || text.startsWith(kNotification)) {
        push(Kind::TaskNotification, text);
    } else if (text.startsWith(kSummaryPrefix)) {
        endTurn();
        push(Kind::Compacted, {}).json = {
            {"summary", text.mid(kSummaryPrefix.size()).toStdString()}};
    } else if (!text.isEmpty() && !isSyntheticUserMessage(text)) {
        endTurn();
        push(Kind::UserMessage, text);
    }
    images(*content);
}

void Builder::assistant(const json &message)
{
    open = true;
    for (const char *key : {"reasoning_content", "reasoning", "reasoning_text"}) {
        const QString reasoning = stringValue(message, key);
        if (!reasoning.isEmpty()) {
            push(Kind::ReasoningChunk, reasoning);
            break;
        }
    }
    const auto content = message.find("content");
    if (content != message.end()) {
        const QString text = textOf(*content);
        if (!text.isEmpty())
            push(Kind::ContentChunk, text);
    }
    const auto calls = message.find("tool_calls");
    if (calls != message.end() && calls->is_array())
        startBatch(*calls);
}

void Builder::startBatch(const json &calls)
{
    QSet<QString> seen;
    for (const json &call : calls) {
        const QString id = call.is_object() ? stringValue(call, "id") : QString();
        if (id.isEmpty())
            continue;
        if (seen.contains(id)) {
            pending.remove(id);
            ambiguous.insert(id);
            continue;
        }
        seen.insert(id);
        const auto    function = call.find("function");
        const QString name     = function != call.end() && function->is_object()
                                     ? stringValue(*function, "name")
                                     : QString();
        if (!name.isEmpty())
            pending.insert(id, PendingTool{name, argumentsOf(*function)});
    }
}

void Builder::tool(const json &message)
{
    const QString id = stringValue(message, "tool_call_id");
    if (ambiguous.contains(id))
        return;
    const auto        content = message.find("content");
    const QString     body    = content == message.end() ? QString() : textOf(*content);
    const PendingTool call    = pending.contains(id)
                                    ? pending.take(id)
                                    : PendingTool{QStringLiteral("tool"), QStringLiteral("{}")};
    const QString     status  = toolStatus(message, body);
    open                      = true;
    push(Kind::ToolStarted, call.arguments, call.name, id);
    Event &finished = push(Kind::ToolFinished, withArtifactHint(message, body), call.name, id);
    finished.ok     = status == QStringLiteral("ok");
    finished.json   = {{"status", status.toStdString()}};
}

void Builder::images(const json &content)
{
    static const QString prefix = QStringLiteral("data:");
    for (const json &part : itemsOf(content)) {
        if (!part.is_object() || stringValue(part, "type") != QStringLiteral("image_url")
            || !part.contains("image_url") || !part["image_url"].is_object())
            continue;
        const QString   url    = stringValue(part["image_url"], "url");
        const qsizetype comma  = url.indexOf(QStringLiteral(";base64,"));
        const QString   base64 = comma < 0 ? QString() : url.mid(comma + 8);
        if (!url.startsWith(prefix) || base64.isEmpty() || base64.size() > kImageBytes
            || imageBytes + base64.size() > kReplayImageBytes)
            continue;
        imageBytes += base64.size();
        QByteArray   bytes = QByteArray::fromBase64(base64.toLatin1());
        QBuffer      buffer(&bytes);
        QImageReader reader(&buffer);
        const QSize  size  = reader.size();
        Event       &image = push(Kind::ImagePreview, QStringLiteral("image"));
        image.json
            = {{"mime", url.mid(prefix.size(), comma - prefix.size()).toStdString()},
               {"data", base64.toStdString()},
               {"width", size.width()},
               {"height", size.height()}};
    }
}

void Builder::endTurn()
{
    if (open)
        push(Kind::RunComplete, {});
    open = false;
}

Event &Builder::push(Kind kind, const QString &text, const QString &secondary, const QString &id)
{
    Event event;
    event.kind      = kind;
    event.text      = text;
    event.secondary = secondary;
    event.callId    = id;
    out.append(std::move(event));
    return out.last();
}

} // namespace

QList<QSocAgentRuntimeEvent> events(const nlohmann::json &messages)
{
    Builder builder;
    for (const json &message : itemsOf(messages))
        builder.add(message);
    return builder.take();
}

} // namespace QSocSessionReplay

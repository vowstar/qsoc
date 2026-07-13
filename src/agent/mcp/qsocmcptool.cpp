// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/mcp/qsocmcptool.h"

#include "agent/mcp/qsocmcpclient.h"
#include "common/qlongtaskmonitor.h"

#include <optional>

#include <QEventLoop>
#include <QObject>
#include <QScopeGuard>
#include <QStringList>

namespace {

QString invalidToolResult()
{
    return QStringLiteral(
        "[mcp protocol error] invalid tools/call result; tool may have completed, do not retry "
        "automatically");
}

QString jsonString(const nlohmann::json &value)
{
    const auto &text = value.get_ref<const nlohmann::json::string_t &>();
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

const nlohmann::json *stringMember(const nlohmann::json &object, const char *name)
{
    const auto member = object.find(name);
    if (member == object.end() || !member->is_string()) {
        return nullptr;
    }
    return &*member;
}

bool optionalStringMemberIsValid(const nlohmann::json &object, const char *name)
{
    const auto member = object.find(name);
    return member == object.end() || member->is_string();
}

std::optional<QString> formatResourceContent(const nlohmann::json &item)
{
    const auto resource = item.find("resource");
    if (resource == item.end() || !resource->is_object()
        || stringMember(*resource, "uri") == nullptr
        || !optionalStringMemberIsValid(*resource, "mimeType")) {
        return std::nullopt;
    }

    const auto text = resource->find("text");
    if (text != resource->end() && text->is_string()) {
        return jsonString(*text);
    }

    const auto blob = resource->find("blob");
    if (blob != resource->end() && blob->is_string()) {
        return QStringLiteral("[mcp unsupported content omitted: resource]");
    }
    return std::nullopt;
}

std::optional<QString> formatContentBlock(const nlohmann::json &item)
{
    if (!item.is_object()) {
        return std::nullopt;
    }
    const nlohmann::json *type = stringMember(item, "type");
    if (type == nullptr) {
        return std::nullopt;
    }

    const auto &typeName = type->get_ref<const nlohmann::json::string_t &>();
    if (typeName == "text") {
        const nlohmann::json *text = stringMember(item, "text");
        if (text == nullptr) {
            return std::nullopt;
        }
        return jsonString(*text);
    }
    if (typeName == "image") {
        if (stringMember(item, "data") == nullptr || stringMember(item, "mimeType") == nullptr) {
            return std::nullopt;
        }
        return QStringLiteral("[mcp unsupported content omitted: image]");
    }
    if (typeName == "audio") {
        if (stringMember(item, "data") == nullptr || stringMember(item, "mimeType") == nullptr) {
            return std::nullopt;
        }
        return QStringLiteral("[mcp unsupported content omitted: audio]");
    }
    if (typeName == "resource") {
        return formatResourceContent(item);
    }
    return QStringLiteral("[mcp unsupported content omitted: unknown]");
}

QString formatToolResult(const nlohmann::json &result)
{
    if (!result.is_object()) {
        return invalidToolResult();
    }

    bool       isError     = false;
    const auto errorMember = result.find("isError");
    if (errorMember != result.end()) {
        if (!errorMember->is_boolean()) {
            return invalidToolResult();
        }
        isError = errorMember->get<bool>();
    }

    const auto content = result.find("content");
    if (content == result.end() || !content->is_array()) {
        return invalidToolResult();
    }

    QStringList parts;
    parts.reserve(static_cast<qsizetype>(content->size()));
    for (const auto &item : *content) {
        std::optional<QString> part = formatContentBlock(item);
        if (!part.has_value()) {
            return invalidToolResult();
        }
        parts.append(std::move(*part));
    }

    QString text           = parts.join(QChar('\n'));
    bool    hasVisibleText = !text.trimmed().isEmpty();
    if (!hasVisibleText && result.contains("structuredContent")) {
        text           = QStringLiteral("[mcp unsupported content omitted: structured]");
        hasVisibleText = true;
    }
    if (isError) {
        if (!hasVisibleText) {
            return QStringLiteral("[mcp tool error] no details");
        }
        return QStringLiteral("[mcp tool error] ") + text;
    }
    if (!hasVisibleText) {
        return QStringLiteral("[mcp result] no content");
    }
    return text;
}

} // namespace

QSocMcpTool::QSocMcpTool(QSocMcpClient *client, McpToolDescriptor descriptor, QObject *parent)
    : QSocTool(parent)
    , client_(client)
    , descriptor_(std::move(descriptor))
    , namespacedName_(QSocMcp::buildToolName(descriptor_.serverName, descriptor_.toolName))
{}

QSocMcpTool::~QSocMcpTool()
{
    Q_ASSERT(activeCalls_.isEmpty());
}

QString QSocMcpTool::getName() const
{
    return namespacedName_;
}

QString QSocMcpTool::getDescription() const
{
    return descriptor_.description;
}

json QSocMcpTool::getParametersSchema() const
{
    if (descriptor_.inputSchema.is_object()) {
        return descriptor_.inputSchema;
    }
    return {{"type", "object"}, {"properties", json::object()}};
}

QString QSocMcpTool::execute(const json &arguments)
{
    if (retired_) {
        return QStringLiteral("[mcp error] tool is no longer available");
    }
    if (client_.isNull()) {
        return QStringLiteral("[mcp error] server has gone away");
    }
    if (client_->state() != QSocMcpClient::State::Ready) {
        return QStringLiteral("[mcp error] server not ready");
    }

    json params;
    params["name"]      = descriptor_.toolName.toStdString();
    params["arguments"] = arguments;

    int        requestId = -1;
    CallState  callState;
    QEventLoop loop;
    callState.loop = &loop;
    activeCalls_.insert(&callState);

    QPointer<QSocMcpTool> self(this);
    const auto            removeCall = qScopeGuard([self, &callState]() {
        if (self.isNull()) {
            return;
        }
        self->activeCalls_.remove(&callState);
        if (self->retired_ && self->activeCalls_.isEmpty()) {
            self->deleteLater();
        }
    });

    QObject::connect(
        client_.data(), &QSocMcpClient::responseReceived, &loop, [&](int id, const json &resultJson) {
            if (id != requestId || callState.outcome != CallOutcome::Pending) {
                return;
            }
            callState.result  = formatToolResult(resultJson);
            callState.outcome = CallOutcome::Completed;
            loop.quit();
        });

    QObject::connect(
        client_.data(),
        &QSocMcpClient::requestFailed,
        &loop,
        [&](int id, int code, const QString &message) {
            if (id != requestId || callState.outcome != CallOutcome::Pending) {
                return;
            }
            if (client_.isNull() || client_->state() == QSocMcpClient::State::Disconnected) {
                callState.result  = QStringLiteral("[mcp error] server closed during call");
                callState.outcome = CallOutcome::ClientClosed;
            } else {
                callState.result  = QStringLiteral("[mcp error %1] %2").arg(code).arg(message);
                callState.outcome = CallOutcome::Completed;
            }
            loop.quit();
        });

    QObject::connect(client_.data(), &QSocMcpClient::closed, &loop, [&]() {
        if (callState.outcome != CallOutcome::Pending) {
            return;
        }
        callState.result  = QStringLiteral("[mcp error] server closed during call");
        callState.outcome = CallOutcome::ClientClosed;
        loop.quit();
    });

    QObject::connect(client_.data(), &QObject::destroyed, &loop, [&]() {
        if (callState.outcome != CallOutcome::Pending) {
            return;
        }
        callState.result  = QStringLiteral("[mcp error] server closed during call");
        callState.outcome = CallOutcome::ClientClosed;
        loop.quit();
    });

    const int returnedId = client_->request(QStringLiteral("tools/call"), params, -1, &requestId);
    if (returnedId < 0) {
        return QStringLiteral("[mcp error] could not send request");
    }

    /* Watchdog: cap the round trip and surface a timeout instead of
     * hanging forever on a server that never answers. Stall detection
     * is disabled since JSON-RPC has no per-byte progress to feed. */
    QLongTaskMonitor monitor(
        &loop,
        QLongTaskMonitor::Config{
            .tickIntervalMs       = 1000,
            .stallThresholdMs     = 0,
            .wallClockMs          = 60000,
            .consecutiveIdleTicks = 2,
        });
    QObject::connect(&monitor, &QLongTaskMonitor::wallClockExceeded, &loop, [&](int) {
        if (callState.outcome != CallOutcome::Pending) {
            return;
        }
        callState.outcome = CallOutcome::TimedOut;
        loop.quit();
    });
    if (callState.outcome == CallOutcome::Pending) {
        monitor.start();
        loop.exec();
        monitor.finish();
    }

    /* Tell the server to drop the in-flight request when we walked
     * away. Without this the server keeps working on output we will
     * never consume. */
    if ((callState.outcome == CallOutcome::Aborted || callState.outcome == CallOutcome::TimedOut)
        && !client_.isNull()) {
        json cancelParams;
        cancelParams["requestId"] = requestId;
        cancelParams["reason"] = callState.outcome == CallOutcome::Aborted ? "user abort"
                                                                           : "wall-clock timeout";
        client_->notify(QStringLiteral("notifications/cancelled"), cancelParams);
    }

    switch (callState.outcome) {
    case CallOutcome::Completed:
    case CallOutcome::ClientClosed:
        return callState.result;
    case CallOutcome::TimedOut:
        return QStringLiteral("[mcp timeout] no response after 60s; sent cancel notification");
    case CallOutcome::Aborted:
        return QStringLiteral("[mcp aborted]");
    case CallOutcome::Pending:
        return QStringLiteral("[mcp error] call ended without a response");
    }

    Q_UNREACHABLE_RETURN(QString());
}

void QSocMcpTool::abort()
{
    const QSet<CallState *> calls = activeCalls_;
    for (CallState *call : calls) {
        if (call->outcome != CallOutcome::Pending) {
            continue;
        }
        call->outcome = CallOutcome::Aborted;
        if (call->loop != nullptr) {
            call->loop->quit();
        }
    }
}

void QSocMcpTool::retire()
{
    if (retired_) {
        return;
    }
    retired_ = true;

    if (activeCalls_.isEmpty()) {
        deleteLater();
        return;
    }
    /* The active call stacks reclaim this wrapper after the final call exits. */
    setParent(nullptr);
}

const McpToolDescriptor &QSocMcpTool::descriptor() const
{
    return descriptor_;
}

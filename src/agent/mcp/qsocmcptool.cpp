// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/mcp/qsocmcptool.h"

#include "agent/mcp/qsocmcpclient.h"
#include "common/qlongtaskmonitor.h"

#include <QEventLoop>
#include <QObject>

namespace {

QString joinTextContent(const nlohmann::json &content)
{
    if (!content.is_array()) {
        return {};
    }
    QString out;
    for (const auto &item : content) {
        if (!item.is_object()) {
            continue;
        }
        const std::string type = item.value("type", std::string());
        if (type == "text") {
            out.append(QString::fromStdString(item.value("text", std::string())));
        }
    }
    return out;
}

QString formatToolResult(const nlohmann::json &result)
{
    if (!result.is_object()) {
        return QStringLiteral("[mcp] empty result");
    }
    const bool    isError = result.value("isError", false);
    const QString text    = joinTextContent(result.value("content", nlohmann::json::array()));
    if (isError) {
        return QStringLiteral("[mcp tool error] ") + text;
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

QSocMcpTool::~QSocMcpTool() = default;

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
    if (client_.isNull()) {
        return QStringLiteral("[mcp error] server has gone away");
    }
    if (client_->state() != QSocMcpClient::State::Ready) {
        return QStringLiteral("[mcp error] server not ready");
    }

    json params;
    params["name"]      = descriptor_.toolName.toStdString();
    params["arguments"] = arguments;

    const int requestId = client_->request(QStringLiteral("tools/call"), params);
    if (requestId < 0) {
        return QStringLiteral("[mcp error] could not send request");
    }

    QEventLoop loop;
    CallState  callState{.loop = &loop};
    activeCalls_.insert(&callState);
    QString result;
    bool    completed = false;
    bool    timedOut  = false;

    /* Watchdog: cap the round trip and surface a timeout instead of
     * hanging forever on a server that never answers. Stall detection
     * is disabled since JSON-RPC has no per-byte progress to feed. */
    QLongTaskMonitor monitor(
        this,
        QLongTaskMonitor::Config{
            .tickIntervalMs       = 1000,
            .stallThresholdMs     = 0,
            .wallClockMs          = 60000,
            .consecutiveIdleTicks = 2,
        });
    QObject::connect(&monitor, &QLongTaskMonitor::wallClockExceeded, &loop, [&](int) {
        timedOut = true;
        loop.quit();
    });
    monitor.start();

    const auto responseConn = QObject::connect(
        client_.data(), &QSocMcpClient::responseReceived, this, [&](int id, const json &resultJson) {
            if (id != requestId || completed) {
                return;
            }
            result    = formatToolResult(resultJson);
            completed = true;
            loop.quit();
        });

    const auto failureConn = QObject::connect(
        client_.data(),
        &QSocMcpClient::requestFailed,
        this,
        [&](int id, int code, const QString &message) {
            if (id != requestId || completed) {
                return;
            }
            result    = QStringLiteral("[mcp error %1] %2").arg(code).arg(message);
            completed = true;
            loop.quit();
        });

    const auto closedConn = QObject::connect(client_.data(), &QSocMcpClient::closed, this, [&]() {
        if (completed) {
            return;
        }
        result    = QStringLiteral("[mcp error] server closed during call");
        completed = true;
        loop.quit();
    });

    loop.exec();
    monitor.finish();

    QObject::disconnect(responseConn);
    QObject::disconnect(failureConn);
    QObject::disconnect(closedConn);
    activeCalls_.remove(&callState);

    /* Tell the server to drop the in-flight request when we walked
     * away. Without this the server keeps working on output we will
     * never consume. */
    if ((callState.aborted || timedOut) && !completed && !client_.isNull()) {
        json cancelParams;
        cancelParams["requestId"] = requestId;
        cancelParams["reason"]    = callState.aborted ? "user abort" : "wall-clock timeout";
        client_->notify(QStringLiteral("notifications/cancelled"), cancelParams);
    }

    if (timedOut) {
        return QStringLiteral("[mcp timeout] no response after 60s; sent cancel notification");
    }
    if (callState.aborted) {
        return QStringLiteral("[mcp aborted]");
    }
    return result;
}

void QSocMcpTool::abort()
{
    const QSet<CallState *> calls = activeCalls_;
    for (CallState *call : calls) {
        call->aborted = true;
        if (call->loop != nullptr) {
            call->loop->quit();
        }
    }
}

const McpToolDescriptor &QSocMcpTool::descriptor() const
{
    return descriptor_;
}

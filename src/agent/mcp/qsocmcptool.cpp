// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/mcp/qsocmcptool.h"

#include "agent/mcp/qsocmcpclient.h"
#include "common/qlongtaskmonitor.h"

#include <QEventLoop>
#include <QObject>
#include <QScopeGuard>

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

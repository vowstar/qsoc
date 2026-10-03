// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/tool/qsoctoolresources.h"
#include "agent/client/qsocagentdaemonclient.h"

#include <utility>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>

namespace {
QJsonObject unavailable(const QString &reason)
{
    return {{"scope", "local_daemon"}, {"status", "unknown"}, {"reason", reason}};
}

QJsonObject queryDaemon(const QStringList &paths, QPointer<QSocToolCallContext> context)
{
    const QString endpoint = qEnvironmentVariable("QSOC_AGENT_SOCKET");
    if (endpoint.isEmpty())
        return unavailable(QStringLiteral("No parent daemon endpoint is available"));
    QEventLoop            dispatcher;
    QSocAgentDaemonClient client(endpoint);
    if (context) {
        QObject::connect(
            context,
            &QSocToolCallContext::cancellationRequested,
            &client,
            &QSocAgentDaemonClient::disconnectFromDaemon);
        if (context->isCancellationRequested())
            return unavailable(QStringLiteral("Resource query cancelled"));
    }
    if (!client.connectToDaemon(1000))
        return unavailable(client.error());
    if (context && context->isCancellationRequested())
        return unavailable(QStringLiteral("Resource query cancelled"));
    if (!client.hasCapability(QStringLiteral("resources")))
        return unavailable(QStringLiteral("The daemon does not support resource queries"));
    const QJsonObject reply = client.request(
        QStringLiteral("resources"), {{"paths", QJsonArray::fromStringList(paths)}}, 5000);
    if (context && context->isCancellationRequested())
        return unavailable(QStringLiteral("Resource query cancelled"));
    if (!reply.value("result").isObject())
        return unavailable(reply.value("error").toString(QStringLiteral("Invalid resource reply")));
    return reply.value("result").toObject();
}
} // namespace

QSocToolResources::QSocToolResources(QObject *parent, Paths paths, Query query)
    : QSocTool(parent)
    , paths_(std::move(paths))
    , query_(std::move(query))
{}

QString QSocToolResources::getName() const
{
    return QStringLiteral("system_resources");
}

QString QSocToolResources::getDescription() const
{
    return QStringLiteral(
        "Read a resource snapshot of the local daemon host and its current descendants. "
        "Remote SSH host resources are not included. CPU values are cumulative counters, "
        "not percentages. Memory metrics have distinct meanings and shared pages can be counted "
        "more than once. Null means unavailable, not zero or unlimited. The snapshot does not "
        "reserve capacity or change limits. sampled_at_utc is the collection start time; "
        "old snapshots are not current capacity. Query when a task needs resource information.");
}

json QSocToolResources::getParametersSchema() const
{
    return {{"type", "object"}, {"properties", json::object()}, {"additionalProperties", false}};
}

QString QSocToolResources::execute(const json &arguments)
{
    const QPointer<QSocToolCallContext> context = currentCallContext();
    QJsonObject                         result;
    if (context && context->isCancellationRequested()) {
        result = unavailable(QStringLiteral("Resource query cancelled"));
    } else if (!arguments.is_object() || !arguments.empty()) {
        result = {{"status", "error"}, {"reason", "This tool accepts no arguments"}};
    } else {
        const auto paths = paths_ ? paths_() : QStringList{};
        result           = query_ ? query_(paths) : queryDaemon(paths, context);
    }
    if (context && context->isCancellationRequested()) {
        result = unavailable(QStringLiteral("Resource query cancelled"));
    }
    if (context) {
        const auto state = result.value("status").toString();
        context->setResultStatus(
            state == "ok"      ? QSocToolResultStatus::Ok
            : state == "error" ? QSocToolResultStatus::Failed
                               : QSocToolResultStatus::Uncertain);
    }
    return QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact));
}

#include "moc_qsoctoolresources.cpp"

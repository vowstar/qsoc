// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>
#include "cli/qsocagenttaskmodel.h"
#include <QJsonArray>
#include <QScopedValueRollback>
void QSocAgentTaskModel::refresh()
{
    if (refreshing_)
        return;
    QScopedValueRollback<bool> guard(refreshing_, true);
    const auto                 reply = request_("tasks", {{"history", historyVisible_}});
    if (!reply.contains("result"))
        return;
    const auto result  = reply.value("result").toObject();
    const auto session = result.value("session_id").toString();
    if (session != sessionId_) {
        sessionId_ = session;
        logId_.clear();
        logText_.clear();
        logOffset_ = 0;
    }
    rows_.clear();
    for (const auto &value : result.value("rows").toArray()) {
        const auto object = value.toObject();
        TaggedRow  item;
        item.sourceTag             = object.value("source").toString();
        item.row.id                = object.value("id").toString();
        item.row.label             = object.value("label").toString();
        item.row.summary           = object.value("summary").toString();
        item.row.objective         = object.value("objective").toString();
        item.row.kind              = static_cast<QSocTask::Kind>(object.value("kind").toInt());
        item.row.status            = static_cast<QSocTask::Status>(object.value("status").toInt());
        item.row.startedAtMs       = object.value("started_at").toInteger();
        item.row.canKill           = object.value("can_kill").toBool();
        item.row.waitingForPeer    = object.value("waiting").toBool();
        item.row.agentId           = object.value("agent_id").toString();
        item.row.host              = object.value("host").toString();
        item.row.workspace         = object.value("workspace").toString();
        item.row.live              = object.value("live").toBool();
        item.row.resumable         = object.value("resumable").toBool();
        const auto estimate        = object.value("estimate").toObject();
        item.estimate.summary      = estimate.value("summary").toString();
        item.estimate.reason       = estimate.value("reason").toString();
        item.estimate.updatedAtMs  = estimate.value("updated_at").toInteger();
        item.estimate.progressLow  = estimate.value("progress_low").toInt(-1);
        item.estimate.progressHigh = estimate.value("progress_high").toInt(-1);
        item.estimate.secondsLow   = estimate.value("seconds_low").toInt(-1);
        item.estimate.secondsHigh  = estimate.value("seconds_high").toInt(-1);
        for (const auto &entry : estimate.value("remaining").toArray())
            item.estimate.remaining.append(entry.toString());
        for (const auto &entry : estimate.value("evidence").toArray())
            item.estimate.evidence.append(entry.toString());
        for (const auto &entry : estimate.value("unknowns").toArray())
            item.estimate.unknowns.append(entry.toString());
        rows_.append(item);
    }
    emit anySourceChanged();
}
int QSocAgentTaskModel::activeCount() const
{
    int count = 0;
    for (const auto &item : rows_)
        if (!QSocTask::isTerminal(item.row.status))
            ++count;
    return count;
}
QString QSocAgentTaskModel::tailFor(const QString &tag, const QString &id, int maxBytes) const
{
    if (logReading_)
        return tag == QStringLiteral("agent") && logId_ == id ? logText_ : QString();
    const QScopedValueRollback<bool> guard(logReading_, true);
    if (tag == QStringLiteral("agent")) {
        if (logId_ != id) {
            logId_ = id;
            logText_.clear();
            logOffset_ = 0;
        }
        const auto reply = request_(
            "task_tail",
            {{"source", tag}, {"id", id}, {"offset", logOffset_}, {"max_bytes", maxBytes}});
        const auto page = reply.value("result").toObject();
        if (logId_ != id)
            return {};
        if (page.contains("next_offset")) {
            const qint64 offset = page.value("offset").toInteger();
            if (offset != logOffset_)
                logText_.clear();
            logText_ += page.value("text").toString();
            logOffset_ = page.value("next_offset").toInteger();
            return logText_;
        }
        return page.value("text").toString();
    }
    return request_("task_tail", {{"source", tag}, {"id", id}, {"max_bytes", maxBytes}})
        .value("result")
        .toObject()
        .value("text")
        .toString();
}
bool QSocAgentTaskModel::killTask(const QString &tag, const QString &id)
{
    return request_("task_kill", {{"source", tag}, {"id", id}})
        .value("result")
        .toObject()
        .value("ok")
        .toBool();
}
QSocTask::Estimate QSocAgentTaskModel::estimateFor(const QString &tag, const QString &id) const
{
    for (const auto &item : rows_)
        if (item.sourceTag == tag && item.row.id == id)
            return item.estimate;
    return {};
}

// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/tool/qsoctoolagentstatus.h"

#include "agent/qsocsubagenttasksource.h"
#include "agent/qsoctasksource.h"

QSocToolAgentStatus::QSocToolAgentStatus(QObject *parent, QSocSubAgentTaskSource *taskSource)
    : QSocTool(parent)
    , taskSource_(taskSource)
{}

QString QSocToolAgentStatus::getName() const
{
    return QStringLiteral("agent_status");
}

QString QSocToolAgentStatus::getDescription() const
{
    return QStringLiteral(
        "Query the current status of a sub-agent run started with the `agent` tool "
        "in `run_in_background=true` mode. Returns the run's status (running / "
        "completed / failed), elapsed seconds, and a tail of the captured "
        "transcript / final result. Use this when you need to act on a child run "
        "without waiting for it synchronously.");
}

json QSocToolAgentStatus::getParametersSchema() const
{
    return json{
        {"type", "object"},
        {"properties",
         {{"task_id",
           {{"type", "string"},
            {"description",
             "The task_id returned from a prior `agent` call with run_in_background=true."}}},
          {"max_bytes",
           {{"type", "integer"},
            {"default", 2000},
            {"description",
             "Maximum bytes of tail content to include. Older content is truncated "
             "with a leading marker when over the cap."}}}}},
        {"required", json::array({"task_id"})}};
}

namespace {

const char *statusToString(QSocTask::Status status)
{
    switch (status) {
    case QSocTask::Status::Running:
        return "running";
    case QSocTask::Status::Pending:
        return "pending";
    case QSocTask::Status::Idle:
        return "idle";
    case QSocTask::Status::Stuck:
        return "stuck";
    case QSocTask::Status::Completed:
        return "completed";
    case QSocTask::Status::Failed:
        return "failed";
    }
    return "unknown";
}

} // namespace

QString QSocToolAgentStatus::execute(const json &arguments)
{
    if (taskSource_ == nullptr) {
        return QStringLiteral(R"({"status":"error","error":"task source not configured"})");
    }
    if (!arguments.contains("task_id") || !arguments["task_id"].is_string()) {
        return QStringLiteral(R"({"status":"error","error":"task_id is required"})");
    }
    const QString taskId   = QString::fromStdString(arguments["task_id"].get<std::string>());
    int           maxBytes = 2000;
    if (arguments.contains("max_bytes") && arguments["max_bytes"].is_number_integer()) {
        maxBytes = arguments["max_bytes"].get<int>();
        if (maxBytes < 0) {
            maxBytes = 0;
        }
    }

    QSocTask::Row row;
    if (!taskSource_->findRow(taskId, &row)) {
        return QString::fromUtf8(
            json{
                {"status", "error"},
                {"error", std::string("unknown or evicted task_id: ") + taskId.toStdString()}}
                .dump()
                .c_str());
    }

    const QString tail           = taskSource_->tailFor(taskId, maxBytes);
    const QString subagentType   = taskSource_->subagentTypeFor(taskId);
    const qint64  elapsedSeconds = taskSource_->elapsedSecondsFor(taskId);

    return QString::fromUtf8(
        json{
            {"status", "ok"},
            {"task_id", taskId.toStdString()},
            {"run_status", statusToString(row.status)},
            {"label", row.label.toStdString()},
            {"subagent_type", subagentType.toStdString()},
            {"elapsed_seconds", elapsedSeconds},
            {"tail", tail.toStdString()},
        }
            .dump()
            .c_str());
}

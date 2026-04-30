// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/tool/qsoctoolsendmessage.h"

#include "agent/qsocsubagenttasksource.h"

QSocToolSendMessage::QSocToolSendMessage(QObject *parent, QSocSubAgentTaskSource *taskSource)
    : QSocTool(parent)
    , taskSource_(taskSource)
{}

QString QSocToolSendMessage::getName() const
{
    return QStringLiteral("send_message");
}

QString QSocToolSendMessage::getDescription() const
{
    return QStringLiteral(
        "Push an additional user message into the input queue of an async sub-agent "
        "started with the `agent` tool in `run_in_background=true` mode. The child "
        "consumes the queued message at its next iteration boundary. Useful when the "
        "parent realises mid-flight that the child needs more context, a corrected "
        "scope, or a follow-up task.\n"
        "\nFails when the task_id is unknown or the child has already finished. "
        "Single-recipient: there is no broadcast or team form.");
}

json QSocToolSendMessage::getParametersSchema() const
{
    return json{
        {"type", "object"},
        {"properties",
         {{"task_id",
           {{"type", "string"},
            {"description",
             "task_id returned from a prior `agent` call with run_in_background=true."}}},
          {"message",
           {{"type", "string"},
            {"description",
             "Text to enqueue as a new user-role message inside the child's "
             "conversation."}}}}},
        {"required", json::array({"task_id", "message"})}};
}

QString QSocToolSendMessage::execute(const json &arguments)
{
    if (taskSource_ == nullptr) {
        return QStringLiteral(R"({"status":"error","error":"task source not configured"})");
    }
    if (!arguments.contains("task_id") || !arguments["task_id"].is_string()) {
        return QStringLiteral(R"({"status":"error","error":"task_id is required"})");
    }
    if (!arguments.contains("message") || !arguments["message"].is_string()) {
        return QStringLiteral(R"({"status":"error","error":"message is required"})");
    }
    const QString taskId  = QString::fromStdString(arguments["task_id"].get<std::string>());
    const QString message = QString::fromStdString(arguments["message"].get<std::string>());
    if (message.isEmpty()) {
        return QStringLiteral(R"({"status":"error","error":"message must not be empty"})");
    }

    if (!taskSource_->queueRequestFor(taskId, message)) {
        return QString::fromUtf8(
            json{
                {"status", "error"},
                {"error",
                 std::string("cannot deliver to task ") + taskId.toStdString()
                     + ": unknown id or run is not Running"}}
                .dump()
                .c_str());
    }

    return QString::fromUtf8(
        json{{"status", "ok"}, {"task_id", taskId.toStdString()}, {"queued_bytes", message.size()}}
            .dump()
            .c_str());
}

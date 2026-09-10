// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/tool/qsoctoolsendmessage.h"

#include "agent/qsocagent.h"
#include "agent/qsocagentmailbox.h"
#include "agent/qsocsubagenttasksource.h"
#include <QUuid>

QSocToolSendMessage::QSocToolSendMessage(QObject *parent, QSocSubAgentTaskSource *taskSource)
    : QSocToolAgentMessage(
          parent, taskSource ? taskSource->mailbox() : nullptr, QStringLiteral("send_message"))
    , taskSource_(taskSource)
{}

QString QSocToolSendMessage::getName() const
{
    return QStringLiteral("send_message");
}

QString QSocToolSendMessage::getDescription() const
{
    return QSocToolAgentMessage::getDescription()
           + QStringLiteral(" The legacy task_id/message form only accepts running children.");
}

json QSocToolSendMessage::getParametersSchema() const
{
    json schema                     = QSocToolAgentMessage::getParametersSchema();
    schema["properties"]["task_id"] = {{"type", "string"}};
    const json peer                 = schema;
    const json legacy
        = {{"type", "object"},
           {"properties", {{"task_id", {{"type", "string"}}}, {"message", {{"type", "string"}}}}},
           {"required", {"task_id", "message"}},
           {"additionalProperties", false}};
    schema["required"] = json::array({"message"});
    json peerBranch    = peer;
    peerBranch["properties"].erase("task_id");
    schema["oneOf"] = json::array({peerBranch, legacy});
    return schema;
}

QString QSocToolSendMessage::execute(const json &arguments)
{
    if (!arguments.is_object())
        return QStringLiteral(R"({"status":"error","error":"invalid_arguments"})");
    if (arguments.contains("target")) {
        if (arguments.contains("task_id"))
            return QStringLiteral(R"({"status":"error","error":"ambiguous_target"})");
        return QSocToolAgentMessage::execute(arguments);
    }
    for (auto it = arguments.begin(); it != arguments.end(); ++it) {
        if (it.key() != "task_id" && it.key() != "message")
            return QStringLiteral(R"({"status":"error","error":"unknown_argument"})");
    }
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

    if (taskSource_->mailbox() != nullptr) {
        const auto *context = currentCallContext();
        auto *caller = context ? qobject_cast<QSocAgent *>(context->executionScope()) : nullptr;
        QSocTask::Row row;
        if (caller == nullptr || !taskSource_->findRow(taskId, &row)
            || row.status != QSocTask::Status::Running) {
            return QStringLiteral(
                R"({"status":"error","error":"unknown id or run is not Running"})");
        }
        const auto *target = taskSource_->mailbox()->agentFor(
            taskSource_->mailbox()->resolve(taskId));
        if (context->isCancellationRequested())
            return QStringLiteral(R"({"status":"error","error":"cancelled"})");
        if (caller->getConfig().planMode && target && !target->getConfig().planMode)
            return QStringLiteral(R"({"status":"error","error":"target_not_in_plan_mode"})");
        json result = taskSource_->mailbox()->send(
            taskSource_->mailbox()->idFor(caller),
            taskId,
            QUuid::createUuid().toString(QUuid::WithoutBraces),
            message,
            {},
            false);
        result["task_id"]      = taskId.toStdString();
        result["queued_bytes"] = message.toUtf8().size();
        return QString::fromStdString(result.dump());
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

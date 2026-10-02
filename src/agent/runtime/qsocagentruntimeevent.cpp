// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/runtime/qsocagentruntimeevent.h"

#include <QDateTime>

namespace {

struct KindName
{
    const char                 *name;
    QSocAgentRuntimeEvent::Kind kind;
};

const KindName kKindNames[] = {
    {"image_preview", QSocAgentRuntimeEvent::Kind::ImagePreview},
    {"user_status_line", QSocAgentRuntimeEvent::Kind::UserStatusLine},
    {"input_prediction", QSocAgentRuntimeEvent::Kind::InputPrediction},
    {"diff", QSocAgentRuntimeEvent::Kind::Diff},
    {"content_chunk", QSocAgentRuntimeEvent::Kind::ContentChunk},
    {"reasoning_chunk", QSocAgentRuntimeEvent::Kind::ReasoningChunk},
    {"run_complete", QSocAgentRuntimeEvent::Kind::RunComplete},
    {"run_error", QSocAgentRuntimeEvent::Kind::RunError},
    {"run_aborted", QSocAgentRuntimeEvent::Kind::RunAborted},
    {"tool_started", QSocAgentRuntimeEvent::Kind::ToolStarted},
    {"tool_output", QSocAgentRuntimeEvent::Kind::ToolOutput},
    {"tool_finished", QSocAgentRuntimeEvent::Kind::ToolFinished},
    {"status", QSocAgentRuntimeEvent::Kind::Status},
    {"heartbeat", QSocAgentRuntimeEvent::Kind::Heartbeat},
    {"retrying", QSocAgentRuntimeEvent::Kind::Retrying},
    {"compacting", QSocAgentRuntimeEvent::Kind::Compacting},
    {"tokens", QSocAgentRuntimeEvent::Kind::Tokens},
    {"stuck", QSocAgentRuntimeEvent::Kind::Stuck},
    {"session_started", QSocAgentRuntimeEvent::Kind::SessionStarted},
    {"session_resumed", QSocAgentRuntimeEvent::Kind::SessionResumed},
    {"session_cleared", QSocAgentRuntimeEvent::Kind::SessionCleared},
    {"session_persisted", QSocAgentRuntimeEvent::Kind::SessionPersisted},
    {"compacted", QSocAgentRuntimeEvent::Kind::Compacted},
    {"context_restored", QSocAgentRuntimeEvent::Kind::ContextRestored},
    {"context_usage", QSocAgentRuntimeEvent::Kind::ContextUsage},
    {"ask_user", QSocAgentRuntimeEvent::Kind::AskUser},
    {"plan_approval", QSocAgentRuntimeEvent::Kind::PlanApproval},
    {"output", QSocAgentRuntimeEvent::Kind::Output},
    {"task_notification", QSocAgentRuntimeEvent::Kind::TaskNotification},
    {"queued_request", QSocAgentRuntimeEvent::Kind::QueuedRequest},
    {"processing_queued", QSocAgentRuntimeEvent::Kind::ProcessingQueued},
    {"model_changed", QSocAgentRuntimeEvent::Kind::ModelChanged},
    {"effort_changed", QSocAgentRuntimeEvent::Kind::EffortChanged},
    {"plan_mode_changed", QSocAgentRuntimeEvent::Kind::PlanModeChanged},
    {"remote_changed", QSocAgentRuntimeEvent::Kind::RemoteChanged},
    {"working_dir_changed", QSocAgentRuntimeEvent::Kind::WorkingDirChanged},
    {"project_changed", QSocAgentRuntimeEvent::Kind::ProjectChanged},
    {"goal_changed", QSocAgentRuntimeEvent::Kind::GoalChanged},
    {"tasks_changed", QSocAgentRuntimeEvent::Kind::TasksChanged},
    {"stop_notice", QSocAgentRuntimeEvent::Kind::StopNotice},
};

} // namespace

QString QSocAgentRuntimeEvent::kindName() const
{
    for (const KindName &entry : kKindNames) {
        if (entry.kind == kind) {
            return QString::fromLatin1(entry.name);
        }
    }
    return QStringLiteral("output");
}

QSocAgentRuntimeEvent::Kind QSocAgentRuntimeEvent::kindFromName(const QString &name)
{
    for (const KindName &entry : kKindNames) {
        if (name == QLatin1String(entry.name)) {
            return entry.kind;
        }
    }
    return Kind::Output;
}

nlohmann::json QSocAgentRuntimeEvent::toJson() const
{
    nlohmann::json value = nlohmann::json::object();
    value["kind"]        = kindName().toStdString();
    value["text"]        = text.toStdString();
    value["secondary"]   = secondary.toStdString();
    value["call_id"]     = callId.toStdString();
    value["at_ms"] = at.isValid() ? at.toMSecsSinceEpoch() : QDateTime::currentMSecsSinceEpoch();
    value["iteration"]       = iteration;
    value["elapsed_seconds"] = elapsedSeconds;
    value["attempt"]         = attempt;
    value["max_attempts"]    = maxAttempts;
    value["layer"]           = layer;
    value["before_tokens"]   = beforeTokens;
    value["after_tokens"]    = afterTokens;
    value["saved_tokens"]    = savedTokens;
    value["message_count"]   = messageCount;
    value["used_tokens"]     = usedTokens;
    value["max_tokens"]      = maxTokens;
    value["threshold"]       = threshold;
    value["input_tokens"]    = inputTokens;
    value["output_tokens"]   = outputTokens;
    value["ok"]              = ok;
    value["flag"]            = flag;
    value["style"]           = static_cast<int>(style);
    if (!json.is_null()) {
        value["json"] = json;
    }
    return value;
}

QSocAgentRuntimeEvent QSocAgentRuntimeEvent::fromJson(const nlohmann::json &value)
{
    QSocAgentRuntimeEvent event;
    if (!value.is_object()) {
        return event;
    }
    const auto str = [&value](const char *key) {
        const auto it = value.find(key);
        return it != value.end() && it->is_string() ? QString::fromStdString(it->get<std::string>())
                                                    : QString();
    };
    const auto num = [&value](const char *key) {
        const auto it = value.find(key);
        return it != value.end() && it->is_number_integer() ? it->get<int>() : 0;
    };
    const auto num64 = [&value](const char *key) {
        const auto it = value.find(key);
        return it != value.end() && it->is_number_integer() ? it->get<qint64>() : qint64(0);
    };
    const auto boolean = [&value](const char *key) {
        const auto it = value.find(key);
        return it != value.end() && it->is_boolean() ? it->get<bool>() : false;
    };

    event.kind           = kindFromName(str("kind"));
    event.text           = str("text");
    event.secondary      = str("secondary");
    event.callId         = str("call_id");
    event.iteration      = num("iteration");
    event.elapsedSeconds = num("elapsed_seconds");
    event.attempt        = num("attempt");
    event.maxAttempts    = num("max_attempts");
    event.layer          = num("layer");
    event.beforeTokens   = num("before_tokens");
    event.afterTokens    = num("after_tokens");
    event.savedTokens    = num("saved_tokens");
    event.messageCount   = num("message_count");
    event.usedTokens     = num("used_tokens");
    event.maxTokens      = num("max_tokens");
    event.inputTokens    = num64("input_tokens");
    event.outputTokens   = num64("output_tokens");
    event.ok             = boolean("ok");
    event.flag           = boolean("flag");
    {
        const auto it = value.find("style");
        if (it != value.end() && it->is_number_integer()) {
            event.style = static_cast<QSocAgentRuntimeStyle>(it->get<int>());
        }
    }
    {
        const auto it = value.find("json");
        if (it != value.end() && !it->is_null()) {
            event.json = *it;
        }
    }
    {
        const auto it = value.find("at_ms");
        if (it != value.end() && it->is_number_integer()) {
            event.at = QDateTime::fromMSecsSinceEpoch(it->get<qint64>());
        }
    }
    return event;
}

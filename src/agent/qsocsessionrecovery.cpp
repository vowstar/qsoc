// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocsessionrecovery.h"

#include <QList>
#include <QMap>
#include <QSet>

namespace {

using json = nlohmann::json;

struct ToolScan
{
    bool                   valid = true;
    bool                   open  = false;
    QList<QString>         callOrder;
    QSet<QString>          expected;
    QSet<QString>          completed;
    QSet<QString>          uncertain;
    QSet<QString>          skipped;
    QMap<QString, QString> names;
};

QString messageRole(const json &message)
{
    if (!message.is_object() || !message.contains("role") || !message["role"].is_string()) {
        return {};
    }
    return QString::fromStdString(message["role"].get<std::string>());
}

bool validToolCall(const json &call, QString *id, QString *name)
{
    if (!call.is_object() || !call.contains("id") || !call["id"].is_string()
        || !call.contains("function") || !call["function"].is_object()) {
        return false;
    }
    const json &function = call["function"];
    if (!function.contains("name") || !function["name"].is_string()
        || !function.contains("arguments") || !function["arguments"].is_string()) {
        return false;
    }
    *id   = QString::fromStdString(call["id"].get<std::string>());
    *name = QString::fromStdString(function["name"].get<std::string>());
    return !id->trimmed().isEmpty() && !name->trimmed().isEmpty();
}

ToolScan scanTools(const json &messages)
{
    ToolScan scan;
    for (const json &message : messages) {
        const QString role = messageRole(message);
        if (role.isEmpty()) {
            scan.valid = false;
            return scan;
        }

        if (role == QStringLiteral("assistant")) {
            if (message.contains("content") && !message["content"].is_null()
                && !message["content"].is_string()) {
                scan.valid = false;
                return scan;
            }
            if (scan.open) {
                scan.valid = false;
                return scan;
            }
            if (!message.contains("tool_calls")) {
                continue;
            }
            if (!message["tool_calls"].is_array()) {
                scan.valid = false;
                return scan;
            }
            if (message["tool_calls"].empty()) {
                continue;
            }
            scan.callOrder.clear();
            scan.expected.clear();
            scan.completed.clear();
            scan.uncertain.clear();
            scan.skipped.clear();
            scan.names.clear();
            for (const json &call : message["tool_calls"]) {
                QString id;
                QString name;
                if (!validToolCall(call, &id, &name) || scan.expected.contains(id)) {
                    scan.valid = false;
                    return scan;
                }
                scan.callOrder.append(id);
                scan.expected.insert(id);
                scan.names.insert(id, name);
            }
            scan.open = true;
            continue;
        }

        if (role == QStringLiteral("tool")) {
            if (!scan.open || !message.contains("tool_call_id")
                || !message["tool_call_id"].is_string() || !message.contains("content")
                || !message["content"].is_string()) {
                scan.valid = false;
                return scan;
            }
            const QString id = QString::fromStdString(message["tool_call_id"].get<std::string>());
            if (!scan.expected.contains(id) || scan.completed.contains(id)) {
                scan.valid = false;
                return scan;
            }
            if (message.contains("_qsoc_tool_state")) {
                if (!message["_qsoc_tool_state"].is_string()) {
                    scan.valid = false;
                    return scan;
                }
                const QString state = QString::fromStdString(
                    message["_qsoc_tool_state"].get<std::string>());
                if (state == QStringLiteral("uncertain")) {
                    scan.uncertain.insert(id);
                } else if (state == QStringLiteral("skipped")) {
                    scan.skipped.insert(id);
                } else {
                    scan.valid = false;
                    return scan;
                }
            }
            scan.completed.insert(id);
            if (scan.completed.size() == scan.expected.size()) {
                scan.open = false;
            }
            continue;
        }

        if (role != QStringLiteral("user") && role != QStringLiteral("system")) {
            scan.valid = false;
            return scan;
        }
        if (!message.contains("content")
            || (role == QStringLiteral("user") && !message["content"].is_string()
                && !message["content"].is_array())
            || (role == QStringLiteral("system") && !message["content"].is_string())) {
            scan.valid = false;
            return scan;
        }
        if (scan.open) {
            scan.valid = false;
            return scan;
        }
    }
    return scan;
}

QSocSessionRecovery::Plan waitPlan(
    const json &messages, const QString &reason, bool requiresUserInput = false)
{
    QSocSessionRecovery::Plan plan;
    plan.messages          = messages;
    plan.reason            = reason;
    plan.requiresUserInput = requiresUserInput;
    return plan;
}

QSocSessionRecovery::Plan continuePlan(
    QSocSessionRecovery::Action action, const json &messages, const QString &reason)
{
    QSocSessionRecovery::Plan plan;
    plan.action   = action;
    plan.messages = messages;
    plan.reason   = reason;
    return plan;
}

json closeToolBatch(const json &messages, const ToolScan &scan, const QSet<QString> &started)
{
    json repaired = messages;
    for (const QString &id : scan.callOrder) {
        if (scan.completed.contains(id)) {
            continue;
        }
        const bool    uncertain = started.contains(id);
        const QString result
            = uncertain
                  ? QStringLiteral(
                        "The prior process stopped while this tool was running. Completion is "
                        "uncertain, and side effects may have occurred. Verify current state "
                        "before retrying.")
                  : QStringLiteral(
                        "Not executed because the prior process stopped before this tool was "
                        "started.");
        repaired.push_back(
            {{"role", "tool"},
             {"tool_call_id", id.toStdString()},
             {"content", result.toStdString()},
             {"_qsoc_tool_state", uncertain ? "uncertain" : "skipped"}});
    }
    return repaired;
}

bool sameExecutionContext(
    const QSocSession::RunRecord &interrupted, const QSocSession::RunRecord &current)
{
    return interrupted.registryModel == current.registryModel
           && interrupted.modelId == current.modelId
           && interrupted.effortLevel == current.effortLevel
           && interrupted.reasoningModel == current.reasoningModel
           && interrupted.planMode == current.planMode
           && interrupted.remoteMode == current.remoteMode
           && interrupted.remoteName == current.remoteName
           && interrupted.projectRoot == current.projectRoot
           && interrupted.workingDir == current.workingDir;
}

} // namespace

bool QSocSessionRecovery::historySafeForNewTurn(const nlohmann::json &messages)
{
    if (!messages.is_array()) {
        return false;
    }
    const ToolScan scan = scanTools(messages);
    return scan.valid && !scan.open;
}

QSocSessionRecovery::Plan QSocSessionRecovery::makePlan(
    const std::optional<QSocSession::RunRecord> &run,
    const nlohmann::json                        &messages,
    const QSocSession::RunRecord                &currentContext,
    const QString                               &activeGoalId)
{
    if (!messages.is_array()) {
        return waitPlan(messages, QStringLiteral("Session history is malformed."));
    }
    if (!run.has_value()) {
        return waitPlan(messages, QStringLiteral("This session has no resumable run record."));
    }
    if (run->event == QSocSession::RunEvent::Invalid) {
        return waitPlan(messages, QStringLiteral("The latest run record is malformed."));
    }
    if (!run->isRunning()) {
        return waitPlan(messages, QStringLiteral("The latest run already reached a terminal state."));
    }
    if (!run->contextPresent || !currentContext.contextPresent) {
        return waitPlan(messages, QStringLiteral("The resumable execution context is unavailable."));
    }
    if (!sameExecutionContext(*run, currentContext)) {
        return waitPlan(
            messages, QStringLiteral("The current execution context differs from the saved run."));
    }
    if (run->goalId != activeGoalId) {
        return waitPlan(messages, QStringLiteral("The active goal no longer matches this run."));
    }

    ToolScan scan = scanTools(messages);
    if (!scan.valid) {
        return waitPlan(messages, QStringLiteral("Session tool history is malformed."));
    }

    if (run->event == QSocSession::RunEvent::Started) {
        if (scan.open) {
            return waitPlan(messages, QStringLiteral("A prior tool batch is incomplete."));
        }
        if (run->messageCount < 0 || run->historyDigest.trimmed().isEmpty()) {
            return waitPlan(messages, QStringLiteral("The interrupted run has no history baseline."));
        }
        const auto baseline = static_cast<json::size_type>(run->messageCount);
        if (messages.size() == baseline
            && QSocSession::historyDigest(messages) == run->historyDigest) {
            if (run->input.trimmed().isEmpty()) {
                if (!run->goalId.isEmpty()) {
                    return continuePlan(
                        Action::ContinueGoal,
                        messages,
                        QStringLiteral("The active goal needs its next continuation turn."));
                }
                return waitPlan(
                    messages, QStringLiteral("The interrupted run has no input to replay."));
            }
            if (!run->inputReplaySafe) {
                return waitPlan(
                    messages, QStringLiteral("The interrupted input cannot be replayed safely."));
            }
            Plan plan = continuePlan(
                Action::ReplayInput,
                messages,
                QStringLiteral("The interrupted input was staged before history was updated."));
            plan.input = run->input;
            return plan;
        }
        if (messages.size() == baseline + 1
            && messageRole(messages.back()) == QStringLiteral("user")) {
            const json &stagedInput = messages.back();
            if (!stagedInput.contains("content") || !stagedInput["content"].is_string()
                || (run->inputReplaySafe
                    && QString::fromStdString(stagedInput["content"].get<std::string>())
                           != run->input)) {
                return waitPlan(
                    messages, QStringLiteral("The staged input differs from the recorded request."));
            }
            json prefix = json::array();
            for (json::size_type index = 0; index < baseline; ++index) {
                prefix.push_back(messages[index]);
            }
            if (QSocSession::historyDigest(prefix) != run->historyDigest) {
                return waitPlan(
                    messages,
                    QStringLiteral("Session history changed beyond the recorded run boundary."));
            }
            return continuePlan(
                Action::ResumeHistory,
                messages,
                QStringLiteral("The staged input is already present in session history."));
        }
        return waitPlan(
            messages, QStringLiteral("Session history changed beyond the recorded run boundary."));
    }

    if (run->event == QSocSession::RunEvent::ToolStarted) {
        if (run->startedToolCallIds.isEmpty()) {
            return waitPlan(messages, QStringLiteral("The active tool checkpoint is inconsistent."));
        }
        QSet<QString> started;
        for (const QString &id : run->startedToolCallIds) {
            started.insert(id);
        }
        bool contextChanging = false;
        for (const QString &id : started) {
            if (!scan.expected.contains(id)) {
                return waitPlan(
                    messages, QStringLiteral("The active tool checkpoint is inconsistent."));
            }
            const QString name = scan.names.value(id);
            if (name == QStringLiteral("enter_plan_mode")
                || name == QStringLiteral("exit_plan_mode")
                || name == QStringLiteral("path_context")) {
                contextChanging = true;
            }
        }
        for (const QString &id : scan.uncertain) {
            if (!started.contains(id)) {
                return waitPlan(
                    messages, QStringLiteral("The active tool checkpoint is inconsistent."));
            }
        }
        for (const QString &id : scan.skipped) {
            if (started.contains(id)) {
                return waitPlan(
                    messages, QStringLiteral("The active tool checkpoint is inconsistent."));
            }
        }
        if (contextChanging) {
            return waitPlan(
                scan.open ? closeToolBatch(messages, scan, started) : messages,
                QStringLiteral("The interrupted context transition needs confirmation."),
                true);
        }
        if (!scan.open) {
            if (!scan.uncertain.isEmpty()) {
                return waitPlan(
                    messages,
                    QStringLiteral(
                        "An interrupted tool may have produced side effects; review its state."),
                    true);
            }
            const QString tailRole = messageRole(messages.back());
            if (tailRole != QStringLiteral("tool") && tailRole != QStringLiteral("user")) {
                return waitPlan(
                    messages, QStringLiteral("The active tool checkpoint is inconsistent."));
            }
            return continuePlan(
                Action::ResumeHistory,
                messages,
                QStringLiteral("The completed tool results were persisted before interruption."));
        }
        bool hasUncertainResult = !scan.uncertain.isEmpty();
        for (const QString &id : started) {
            hasUncertainResult = hasUncertainResult || !scan.completed.contains(id);
        }
        const json repaired = closeToolBatch(messages, scan, started);
        if (hasUncertainResult) {
            return waitPlan(
                repaired,
                QStringLiteral(
                    "An interrupted tool may have produced side effects; review its state."),
                true);
        }
        return continuePlan(
            Action::ResumeHistory,
            repaired,
            QStringLiteral("Tools not started before interruption were marked skipped."));
    }

    if (scan.open) {
        if (!scan.completed.isEmpty()) {
            return waitPlan(
                messages, QStringLiteral("A model checkpoint has inconsistent tool results."));
        }
        return continuePlan(
            Action::ResumeHistory,
            closeToolBatch(messages, scan, {}),
            QStringLiteral("Tools not started before interruption were marked skipped."));
    }
    if (messages.empty()) {
        return waitPlan(messages, QStringLiteral("The model checkpoint has no conversation history."));
    }

    const QString tailRole = messageRole(messages.back());
    if (tailRole == QStringLiteral("user") || tailRole == QStringLiteral("tool")) {
        return continuePlan(
            Action::ResumeHistory,
            messages,
            QStringLiteral("The interrupted model request can continue from persisted history."));
    }
    if (tailRole == QStringLiteral("assistant") && !run->goalId.isEmpty()) {
        return continuePlan(
            Action::ContinueGoal,
            messages,
            QStringLiteral("The active goal needs its next continuation turn."));
    }
    return waitPlan(messages, QStringLiteral("The persisted response already completed this turn."));
}

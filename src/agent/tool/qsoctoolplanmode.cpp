// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/tool/qsoctoolplanmode.h"

#include <utility>

QSocToolEnterPlanMode::QSocToolEnterPlanMode(QObject *parent, Callback onEnter)
    : QSocTool(parent)
    , onEnter_(std::move(onEnter))
{}

void QSocToolEnterPlanMode::setCallback(Callback onEnter)
{
    onEnter_ = std::move(onEnter);
}

QString QSocToolEnterPlanMode::getName() const
{
    return QStringLiteral("enter_plan_mode");
}

QString QSocToolEnterPlanMode::getDescription() const
{
    return QStringLiteral(
        "Switch into read-only plan mode before tackling a non-trivial task. "
        "While in plan mode you may only explore (read files, search, run "
        "read-only shell, spawn read-only sub-agents) and ask the user "
        "questions; you cannot edit files, run mutating commands, or commit. "
        "Use this when the task warrants a plan the user should approve first, "
        "then call exit_plan_mode when the plan is ready.");
}

json QSocToolEnterPlanMode::getParametersSchema() const
{
    return json{{"type", "object"}, {"properties", json::object()}};
}

QString QSocToolEnterPlanMode::execute(const json & /*arguments*/)
{
    if (onEnter_) {
        onEnter_();
    }
    return QStringLiteral(
        "Entered plan mode. You are now read-only: explore the code, use "
        "ask_user to clarify anything ambiguous, then call exit_plan_mode "
        "with a concrete plan for the user to approve.");
}

QSocToolExitPlanMode::QSocToolExitPlanMode(QObject *parent, Callback onExit)
    : QSocTool(parent)
    , onExit_(std::move(onExit))
{}

void QSocToolExitPlanMode::setCallback(Callback onExit)
{
    onExit_ = std::move(onExit);
}

QString QSocToolExitPlanMode::getName() const
{
    return QStringLiteral("exit_plan_mode");
}

QString QSocToolExitPlanMode::getDescription() const
{
    return QStringLiteral(
        "Present your implementation plan and ask the user to approve it. Only "
        "available in plan mode. Pass the full plan as `plan`. If the user "
        "approves you may start making changes; otherwise keep refining the "
        "plan. Do not ask for plan approval any other way.");
}

json QSocToolExitPlanMode::getParametersSchema() const
{
    return json{
        {"type", "object"},
        {"properties",
         {{"plan",
           {{"type", "string"},
            {"description",
             "The complete implementation plan, in markdown, for the user to "
             "review and approve."}}}}},
        {"required", json::array({"plan"})}};
}

QString QSocToolExitPlanMode::execute(const json &arguments)
{
    if (!onExit_) {
        return QStringLiteral(
            R"({"status":"error","error":"exit_plan_mode is unavailable in this context"})");
    }
    QString plan;
    if (arguments.contains("plan") && arguments["plan"].is_string()) {
        plan = QString::fromStdString(arguments["plan"].get<std::string>());
    }
    plan = plan.trimmed();
    if (plan.isEmpty()) {
        return QStringLiteral(
            R"({"status":"error","error":"plan is required: pass the full plan text"})");
    }

    const QSocPlanApproval result = onExit_(plan);
    if (result.approved) {
        return QStringLiteral(
                   "User approved the plan. You may now make changes. Follow the approved "
                   "plan; deviate only with good reason and say so.\n\nApproved plan:\n%1")
            .arg(plan);
    }

    QString feedback = result.feedback.trimmed();
    if (feedback.isEmpty()) {
        feedback = QStringLiteral("(no specific feedback)");
    }
    return QStringLiteral(
               "User did not approve the plan. Stay in plan mode and keep refining "
               "it. Feedback: %1")
        .arg(feedback);
}

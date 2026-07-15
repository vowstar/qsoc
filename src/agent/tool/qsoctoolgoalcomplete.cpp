// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/tool/qsoctoolgoalcomplete.h"

#include "agent/qsocgoal.h"

namespace {

QString errorResult(const QString &message)
{
    return QString::fromStdString(
        json{{"status", "error"}, {"error", message.toStdString()}}.dump());
}

QString successResult(const QString &objective)
{
    return QString::fromStdString(
        json{{"status", "ok"}, {"completed_goal", objective.toStdString()}}.dump());
}

} // namespace

QSocToolGoalComplete::QSocToolGoalComplete(QObject *parent, QSocGoalCatalog *catalog)
    : QSocTool(parent)
    , catalog_(catalog)
{}

QString QSocToolGoalComplete::getName() const
{
    return QStringLiteral("goal_complete");
}

QString QSocToolGoalComplete::getDescription() const
{
    return QStringLiteral(
        "Mark the active project goal as complete. ONLY call when the objective "
        "has actually been achieved and no required work remains; do not call "
        "merely because the token budget is nearly exhausted or because you are "
        "stopping for the turn. Pause, resume, and budget-limited transitions "
        "are controlled by the user or the runtime, not through this tool. "
        "Not available in sub-agent dispatch: a child run must not declare the "
        "parent's goal done, surface progress back to the parent instead.");
}

json QSocToolGoalComplete::getParametersSchema() const
{
    return json{
        {"type", "object"},
        {"properties",
         {{"status",
           {{"type", "string"},
            {"enum", json::array({"complete"})},
            {"description",
             "Set to 'complete' only when the objective has actually been "
             "achieved and no required work remains."}}}}},
        {"required", json::array({"status"})}};
}

QString QSocToolGoalComplete::execute(const json &arguments)
{
    const QPointer<QSocGoalCatalog> catalog = catalog_;
    if (catalog.isNull()) {
        return errorResult(
            QStringLiteral("goal_complete unavailable: this run has no project goal catalog"));
    }
    if (!arguments.contains("status") || !arguments["status"].is_string()) {
        return errorResult(QStringLiteral("status is required"));
    }
    const QString status = QString::fromStdString(arguments["status"].get<std::string>());
    if (status != QStringLiteral("complete")) {
        return errorResult(QStringLiteral(
            "goal_complete only accepts status=complete; pause/resume/budget changes go through "
            "user commands"));
    }
    const auto current = catalog->current();
    if (!current.has_value()) {
        return errorResult(QStringLiteral("no active goal to complete"));
    }
    const QString completed = current->objective;
    QString       err;
    if (!catalog->setStatus(QSocGoalStatus::Complete, &err)) {
        return errorResult(err);
    }
    QString summary = completed;
    if (summary.size() > 140) {
        summary = summary.left(137) + QStringLiteral("...");
    }
    summary.replace(QLatin1Char('\n'), QLatin1Char(' '));
    return successResult(summary);
}

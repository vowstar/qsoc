// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/tool/qsoctoolschedule.h"

#include "cli/qsocloopscheduler.h"
#include "common/qsoccron.h"

#include <QDateTime>
#include <QString>

/* ========== QSocToolScheduleCreate ========== */

QSocToolScheduleCreate::QSocToolScheduleCreate(QObject *parent, QSocLoopScheduler *scheduler)
    : QSocTool(parent)
    , scheduler_(scheduler)
{}

QSocToolScheduleCreate::~QSocToolScheduleCreate() = default;

QString QSocToolScheduleCreate::getName() const
{
    return "schedule_create";
}

QString QSocToolScheduleCreate::getDescription() const
{
    return "Schedule a prompt or slash command to run at a future time, either recurring on a cron "
           "schedule or once at a specific time. Use proactively when the user asks to monitor, "
           "poll, retry periodically, remind them later, or run something at a future time. Time "
           "is a 5-field cron expression in local time (minute hour day-of-month month "
           "day-of-week). Recurring tasks use patterns like \"*/5 * * * *\" with recurring=true; "
           "one-shot tasks pin minute+hour+dom+month like \"30 14 27 2 *\" with recurring=false. "
           "Default durable=false (session-only); set durable=true only when the user asks the "
           "task to survive QSoC restarts. Preserve /commands and !commands verbatim in prompt.";
}

json QSocToolScheduleCreate::getParametersSchema() const
{
    return {
        {"type", "object"},
        {"properties",
         {{"prompt",
           {{"type", "string"},
            {"description",
             "Prompt, slash command, or shell escape to run when the schedule fires. "
             "Slash commands and !shell escapes are preserved verbatim and routed through "
             "CLI dispatch."}}},
          {"cron",
           {{"type", "string"},
            {"description",
             "Standard 5-field cron in local time: \"M H DoM Mon DoW\" "
             "(e.g. \"*/5 * * * *\" = every 5 minutes; \"30 14 27 2 *\" = Feb 27 at 2:30pm "
             "local once)."}}},
          {"recurring",
           {{"type", "boolean"},
            {"description",
             "true (default) = fire on every cron match until deleted. false = fire once at "
             "the next match, then auto-delete. Use false for \"remind me at X\" one-shot "
             "requests with pinned minute/hour/dom/month."}}},
          {"durable",
           {{"type", "boolean"},
            {"description",
             "true = persist to .qsoc/loops.json and survive QSoC restarts. false (default) "
             "= in-memory only, dies when this QSoC session ends. Use true only when the "
             "user asks the task to survive across sessions."}}}}},
        {"required", json::array({"prompt", "cron"})}};
}

QString QSocToolScheduleCreate::execute(const json &arguments)
{
    if (scheduler_ == nullptr) {
        return "Error: scheduler not configured";
    }
    if (!arguments.contains("prompt") || !arguments["prompt"].is_string()) {
        return "Error: 'prompt' is required and must be a string";
    }
    if (!arguments.contains("cron") || !arguments["cron"].is_string()) {
        return "Error: 'cron' is required and must be a string";
    }
    const QString prompt = QString::fromStdString(arguments["prompt"].get<std::string>());
    const QString cron   = QString::fromStdString(arguments["cron"].get<std::string>());
    if (prompt.trimmed().isEmpty()) {
        return "Error: 'prompt' must not be empty";
    }
    if (!QSocCron::isValid(cron)) {
        return QString("Error: invalid 5-field cron expression: %1").arg(cron);
    }
    bool recurring = true;
    if (arguments.contains("recurring") && arguments["recurring"].is_boolean()) {
        recurring = arguments["recurring"].get<bool>();
    }
    bool durable = false;
    if (arguments.contains("durable") && arguments["durable"].is_boolean()) {
        durable = arguments["durable"].get<bool>();
    }

    const QString id = scheduler_->addJob(cron, prompt, recurring, durable);
    if (id.isEmpty()) {
        if (!scheduler_->isOwner() && durable) {
            return "Error: another QSoC session owns this project's scheduled tasks; "
                   "durable mutations are refused.";
        }
        return "Error: failed to schedule task. Causes: scheduled task limit reached, "
               "persist failed, or invalid input.";
    }

    const QString kind  = recurring ? QStringLiteral("recurring") : QStringLiteral("one-shot");
    const QString where = durable ? QStringLiteral("durable (.qsoc/loops.json)")
                                  : QStringLiteral("session-only");
    return QString(
               "Scheduled %1: cron \"%2\" (%3), %4, %5. "
               "Cancel with schedule_delete id=%1.")
        .arg(id, cron, QSocCron::cronToHuman(cron), kind, where);
}

/* ========== QSocToolScheduleList ========== */

QSocToolScheduleList::QSocToolScheduleList(QObject *parent, QSocLoopScheduler *scheduler)
    : QSocTool(parent)
    , scheduler_(scheduler)
{}

QSocToolScheduleList::~QSocToolScheduleList() = default;

QString QSocToolScheduleList::getName() const
{
    return "schedule_list";
}

QString QSocToolScheduleList::getDescription() const
{
    return "List all scheduled tasks created by /loop or schedule_create. Returns id, cron "
           "expression, human cadence, recurring flag, durability, next-fire ETA, and prompt "
           "summary for each task.";
}

json QSocToolScheduleList::getParametersSchema() const
{
    return {{"type", "object"}, {"properties", json::object()}, {"required", json::array()}};
}

QString QSocToolScheduleList::execute(const json &arguments)
{
    Q_UNUSED(arguments);
    if (scheduler_ == nullptr) {
        return "Error: scheduler not configured";
    }
    const auto jobs = scheduler_->listJobs();
    if (jobs.isEmpty()) {
        return "No scheduled tasks.";
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QString      out;
    for (const auto &job : jobs) {
        const qint64 anchor = job.lastFiredAt > 0 ? job.lastFiredAt : job.createdAt;
        const qint64 next   = QSocCron::nextRunMs(job.cron, anchor);
        const qint64 due    = (next == 0) ? -1 : next - now;
        QString      eta;
        if (next == 0) {
            eta = QStringLiteral("never");
        } else if (due <= 0) {
            eta = QStringLiteral("now");
        } else if (due < 60000) {
            eta = QStringLiteral("%1s").arg(due / 1000);
        } else if (due < 3600000) {
            eta = QStringLiteral("%1m").arg(due / 60000);
        } else {
            eta = QStringLiteral("%1h").arg(due / 3600000);
        }
        QString promptSummary = job.prompt;
        promptSummary.replace(QLatin1Char('\n'), QLatin1Char(' '));
        if (promptSummary.size() > 60) {
            promptSummary = promptSummary.left(57) + QStringLiteral("...");
        }
        out += QString("- %1  cron=\"%2\"  %3  %4  %5  next=%6  prompt=%7\n")
                   .arg(job.id)
                   .arg(job.cron)
                   .arg(QSocCron::cronToHuman(job.cron))
                   .arg(job.recurring ? QStringLiteral("recurring") : QStringLiteral("one-shot"))
                   .arg(job.durable ? QStringLiteral("durable") : QStringLiteral("session"))
                   .arg(eta, promptSummary);
    }
    return out;
}

/* ========== QSocToolScheduleDelete ========== */

QSocToolScheduleDelete::QSocToolScheduleDelete(QObject *parent, QSocLoopScheduler *scheduler)
    : QSocTool(parent)
    , scheduler_(scheduler)
{}

QSocToolScheduleDelete::~QSocToolScheduleDelete() = default;

QString QSocToolScheduleDelete::getName() const
{
    return "schedule_delete";
}

QString QSocToolScheduleDelete::getDescription() const
{
    return "Cancel a scheduled task by id. The id is reported by schedule_create / schedule_list "
           "or shown in /loop list output (8-character hex string).";
}

json QSocToolScheduleDelete::getParametersSchema() const
{
    return {
        {"type", "object"},
        {"properties",
         {{"id",
           {{"type", "string"}, {"description", "8-character hex id of the task to cancel."}}}}},
        {"required", json::array({"id"})}};
}

QString QSocToolScheduleDelete::execute(const json &arguments)
{
    if (scheduler_ == nullptr) {
        return "Error: scheduler not configured";
    }
    if (!arguments.contains("id") || !arguments["id"].is_string()) {
        return "Error: 'id' is required and must be a string";
    }
    const QString id = QString::fromStdString(arguments["id"].get<std::string>());
    if (scheduler_->removeJob(id)) {
        return QString("Cancelled scheduled task %1.").arg(id);
    }
    /* Distinguish not-found from non-owner refusal by peeking at the list. */
    bool present = false;
    for (const auto &job : scheduler_->listJobs()) {
        if (job.id == id) {
            present = true;
            break;
        }
    }
    if (!present) {
        return QString("No scheduled task with id %1.").arg(id);
    }
    if (!scheduler_->isOwner()) {
        return "Error: another QSoC session owns this project's scheduled tasks; "
               "durable mutations are refused.";
    }
    return QString("Error: failed to remove task %1 (persist write failed).").arg(id);
}

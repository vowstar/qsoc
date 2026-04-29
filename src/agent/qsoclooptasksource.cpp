// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsoclooptasksource.h"

#include "cli/qsocloopscheduler.h"
#include "common/qsoccron.h"

#include <QDateTime>

QSocLoopTaskSource::QSocLoopTaskSource(QSocLoopScheduler *scheduler, QObject *parent)
    : QSocTaskSource(parent)
    , scheduler_(scheduler)
{
    if (scheduler_ != nullptr) {
        connect(scheduler_, &QSocLoopScheduler::jobsChanged, this, &QSocLoopTaskSource::tasksChanged);
    }
}

QList<QSocTask::Row> QSocLoopTaskSource::listTasks() const
{
    QList<QSocTask::Row> out;
    if (scheduler_ == nullptr)
        return out;
    for (const auto &job : scheduler_->listJobs()) {
        QSocTask::Row row;
        row.id      = job.id;
        row.label   = job.prompt;
        row.summary = QSocCron::cronToHuman(job.cron);
        row.kind    = QSocTask::Kind::Loop;
        /* Loop tasks are scheduled rather than actively running; mark
         * Pending so the overlay sorts them after live bash processes. */
        row.status      = QSocTask::Status::Pending;
        row.startedAtMs = job.createdAt;
        row.canKill     = true;
        out.append(row);
    }
    return out;
}

QString QSocLoopTaskSource::tailFor(const QString &id, int maxBytes) const
{
    Q_UNUSED(maxBytes);
    if (scheduler_ == nullptr)
        return QString();
    for (const auto &job : scheduler_->listJobs()) {
        if (job.id != id)
            continue;
        const qint64  now    = QDateTime::currentMSecsSinceEpoch();
        const qint64  anchor = job.lastFiredAt > 0 ? job.lastFiredAt : job.createdAt;
        const qint64  next   = QSocCron::nextRunMs(job.cron, anchor);
        const QString lastFired
            = job.lastFiredAt > 0
                  ? QDateTime::fromMSecsSinceEpoch(job.lastFiredAt).toString(Qt::ISODate)
                  : QStringLiteral("(never)");
        const QString nextFired = next > 0
                                      ? QDateTime::fromMSecsSinceEpoch(next).toString(Qt::ISODate)
                                      : QStringLiteral("(never)");
        const QString eta = next > 0 && next > now ? QStringLiteral("%1s").arg((next - now) / 1000)
                                                   : QStringLiteral("now");
        const QString durable   = job.durable ? QStringLiteral("durable")
                                              : QStringLiteral("session-only");
        const QString recurring = job.recurring ? QStringLiteral("recurring")
                                                : QStringLiteral("one-shot");
        return QStringLiteral(
                   "Cron:        %1\n"
                   "Cadence:     %2\n"
                   "Mode:        %3, %4\n"
                   "Created:     %5\n"
                   "Last fired:  %6\n"
                   "Next fire:   %7  (%8)\n"
                   "\n"
                   "Prompt:\n%9\n")
            .arg(job.cron)
            .arg(QSocCron::cronToHuman(job.cron))
            .arg(recurring)
            .arg(durable)
            .arg(QDateTime::fromMSecsSinceEpoch(job.createdAt).toString(Qt::ISODate))
            .arg(lastFired)
            .arg(nextFired)
            .arg(eta)
            .arg(job.prompt);
    }
    return QString();
}

bool QSocLoopTaskSource::killTask(const QString &id)
{
    if (scheduler_ == nullptr)
        return false;
    return scheduler_->removeJob(id);
}

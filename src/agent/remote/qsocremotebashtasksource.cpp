// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/remote/qsocremotebashtasksource.h"

#include "agent/qsoctaskeventqueue.h"
#include "agent/remote/qsocagentremote.h"
#include "agent/remote/qsocremotejobwatcher.h"

namespace {

QSocTask::Status endedStatus(bool stopped, int exitCode)
{
    if (stopped)
        return QSocTask::Status::Aborted;
    return exitCode == 0 ? QSocTask::Status::Completed : QSocTask::Status::Failed;
}

} // namespace

QSocRemoteBashTaskSource::QSocRemoteBashTaskSource(QSocRemoteConnection *conn, QObject *parent)
    : QSocTaskSource(parent)
    , conn_(conn)
{
    connect(
        conn_->watcher(),
        &QSocRemoteJobWatcher::jobSettled,
        this,
        &QSocRemoteBashTaskSource::settle);
}

void QSocRemoteBashTaskSource::setTaskEventQueue(QSocTaskEventQueue *queue)
{
    eventQueue_ = queue;
}

QSocTask::Status QSocRemoteBashTaskSource::statusOf(const QString &jobId) const
{
    const auto ended = ended_.constFind(jobId);
    if (ended != ended_.cend())
        return *ended;
    const QSocRemoteJobRecord record = conn_->jobs()->record(jobId);
    return record.settled ? endedStatus(record.stopped, record.exitCode)
                          : QSocTask::Status::Running;
}

QList<QSocTask::Row> QSocRemoteBashTaskSource::listTasks() const
{
    QList<QSocTask::Row> out;
    for (const QSocRemoteJobRecord &record : conn_->jobs()->records()) {
        if (record.monitor)
            continue;
        QSocTask::Row row;
        row.id          = record.jobId;
        row.label       = record.commandLine;
        row.summary     = conn_->target();
        row.kind        = QSocTask::Kind::BackgroundBash;
        row.status      = statusOf(record.jobId);
        row.startedAtMs = record.launchedMs;
        row.canKill     = row.status == QSocTask::Status::Running;
        out.append(row);
    }
    return out;
}

/* Served from what the watcher saw last, so opening a row never blocks on SSH. */
QString QSocRemoteBashTaskSource::tailFor(const QString &id, int maxBytes) const
{
    if (!conn_->jobs()->has(id))
        return {};
    QString tail = tails_.value(id);
    if (tail.isEmpty())
        tail = QString::fromUtf8(conn_->watcher()->savedTail(id));
    if (tail.isEmpty())
        return QStringLiteral("(no output yet)\n");
    return maxBytes > 0 ? tail.right(maxBytes) : tail;
}

/* Queued on the watcher, so the UI never waits on the link. */
bool QSocRemoteBashTaskSource::killTask(const QString &id)
{
    if (statusOf(id) != QSocTask::Status::Running || !conn_->isUsable())
        return false;
    conn_->watcher()->requestStop(id);
    emit tasksChanged();
    return true;
}

void QSocRemoteBashTaskSource::settle(const QString &jobId, int exitCode, const QByteArray &tail)
{
    const QSocRemoteJobRecord record = conn_->jobs()->record(jobId);
    if (record.jobId.isEmpty() || record.monitor)
        return;
    for (const QString &gone : ended_.keys()) {
        if (!conn_->jobs()->has(gone)) {
            ended_.remove(gone);
            tails_.remove(gone);
        }
    }
    const QSocTask::Status status = endedStatus(record.stopped, exitCode);
    ended_.insert(jobId, status);
    tails_.insert(jobId, QString::fromUtf8(tail));
    emit taskTerminal(jobId, status, tails_.value(jobId));
    emit tasksChanged();
    /* A stopped job was stopped by its owner, who already knows. */
    if (eventQueue_.isNull() || status == QSocTask::Status::Aborted)
        return;
    QSocTaskEvent event;
    event.taskId      = jobId;
    event.sourceTag   = sourceTag();
    event.kind        = QStringLiteral("task_notification");
    event.status      = QSocTask::statusWord(status);
    event.description = record.commandLine.left(120);
    event.content     = tails_.value(jobId);
    event.outputFile  = remoteJobDir(conn_, jobId) + QStringLiteral("/output.log");
    event.agentId     = record.ownerId;
    eventQueue_->enqueue(event);
}

#include "moc_qsocremotebashtasksource.cpp"

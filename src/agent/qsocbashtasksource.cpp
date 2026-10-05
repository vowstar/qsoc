// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocbashtasksource.h"

#include "agent/qsocagent.h"
#include "agent/qsoctaskeventqueue.h"
#include "agent/tool/qsoctoolshell.h"

#include <QDateTime>

QSocBashTaskSource::QSocBashTaskSource(QSocToolShellBash *bashTool, QObject *parent)
    : QSocTaskSource(parent)
    , bashTool_(bashTool)
{
    if (bashTool_ != nullptr) {
        connect(
            bashTool_,
            &QSocToolShellBash::backgroundProcessFinished,
            this,
            [this](int processId, int, const QString &) {
                settle(processId);
                emit tasksChanged();
            });
        connect(
            bashTool_,
            &QSocToolShellBash::processStuckDetected,
            this,
            [this](int, const QString &, const QString &) { emit tasksChanged(); });
    }
}

namespace {

QSocTask::Status statusOf(const QSocToolShellBash::BackgroundSnapshot &snap)
{
    if (snap.isRunning)
        return snap.isStuck ? QSocTask::Status::Stuck : QSocTask::Status::Running;
    if (snap.stopRequested)
        return QSocTask::Status::Aborted;
    return snap.crashed || snap.exitCode != 0 ? QSocTask::Status::Failed
                                              : QSocTask::Status::Completed;
}

/* A process no live agent started belongs to the user. */
QString ownerOf(QObject *scope)
{
    const auto *agent = qobject_cast<QSocAgent *>(scope);
    return agent != nullptr ? agent->agentIdentity() : QSocTaskEvent::userOwner();
}

} // namespace

void QSocBashTaskSource::settle(int processId)
{
    for (const auto &snap : QSocToolShellBash::snapshotActive(bashTool_)) {
        const QSocTask::Status status = statusOf(snap);
        if (snap.id != processId || !QSocTask::isTerminal(status))
            continue;
        const QString tail = QSocToolShellBash::tailActive(processId, 4000);
        emit          taskTerminal(QString::number(processId), status, tail);
        /* A stopped job was stopped by its owner, who already knows. */
        if (eventQueue_.isNull() || status == QSocTask::Status::Aborted)
            return;
        QSocTaskEvent event;
        event.taskId      = QString::number(processId);
        event.sourceTag   = sourceTag();
        event.kind        = QStringLiteral("task_notification");
        event.status      = QSocTask::statusWord(status);
        event.description = snap.command.left(120);
        event.content     = tail;
        event.outputFile  = snap.outputPath;
        event.agentId     = ownerOf(snap.scope.data());
        eventQueue_->enqueue(event);
        return;
    }
}

QList<QSocTask::Row> QSocBashTaskSource::listTasks() const
{
    QList<QSocTask::Row> out;
    for (const auto &snap : QSocToolShellBash::snapshotActive(bashTool_)) {
        QSocTask::Row row;
        row.id          = QString::number(snap.id);
        row.label       = snap.command;
        row.summary     = snap.outputPath;
        row.kind        = QSocTask::Kind::BackgroundBash;
        row.status      = statusOf(snap);
        row.startedAtMs = snap.startedAtMs;
        row.canKill     = snap.isRunning;
        out.append(row);
    }
    return out;
}

QString QSocBashTaskSource::tailFor(const QString &id, int maxBytes) const
{
    bool      ok        = false;
    const int processId = id.toInt(&ok);
    if (!ok || !bashTool_ || !bashTool_->ownsProcess(processId))
        return QString();
    const QString tail = QSocToolShellBash::tailActive(processId, maxBytes);
    if (tail.isEmpty())
        return QStringLiteral("(no output yet)\n");
    return tail;
}

bool QSocBashTaskSource::killTask(const QString &id)
{
    bool      ok        = false;
    const int processId = id.toInt(&ok);
    if (!ok || !bashTool_ || !bashTool_->ownsProcess(processId))
        return false;
    const bool result = QSocToolShellBash::killActive(processId);
    if (result)
        emit tasksChanged();
    return result;
}

void QSocBashTaskSource::setTaskEventQueue(QSocTaskEventQueue *queue)
{
    eventQueue_ = queue;
}

#include "moc_qsocbashtasksource.cpp"

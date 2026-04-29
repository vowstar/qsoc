// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocbashtasksource.h"

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
            [this](int, int, const QString &) { emit tasksChanged(); });
        connect(
            bashTool_,
            &QSocToolShellBash::processStuckDetected,
            this,
            [this](int, const QString &, const QString &) { emit tasksChanged(); });
    }
}

QList<QSocTask::Row> QSocBashTaskSource::listTasks() const
{
    QList<QSocTask::Row> out;
    for (const auto &snap : QSocToolShellBash::snapshotActive()) {
        QSocTask::Row row;
        row.id          = QString::number(snap.id);
        row.label       = snap.command;
        row.summary     = snap.outputPath;
        row.kind        = QSocTask::Kind::BackgroundBash;
        row.status      = snap.isStuck     ? QSocTask::Status::Stuck
                          : snap.isRunning ? QSocTask::Status::Running
                                           : QSocTask::Status::Completed;
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
    if (!ok)
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
    if (!ok)
        return false;
    const bool result = QSocToolShellBash::killActive(processId);
    if (result)
        emit tasksChanged();
    return result;
}

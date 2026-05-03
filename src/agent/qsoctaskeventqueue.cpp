// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsoctaskeventqueue.h"

#include <QDateTime>

namespace {

QString tag(const QString &name, const QString &value)
{
    if (value.isEmpty())
        return {};
    return QStringLiteral("<%1>%2</%1>\n").arg(name, value.toHtmlEscaped());
}

} /* namespace */

QSocTaskEventQueue::QSocTaskEventQueue(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<QSocTaskEvent>("QSocTaskEvent");
}

void QSocTaskEventQueue::enqueue(const QSocTaskEvent &event)
{
    QSocTaskEvent copy = event;
    if (copy.createdAtMs <= 0) {
        copy.createdAtMs = QDateTime::currentMSecsSinceEpoch();
    }
    emit this->taskEventQueued(copy);
    if (copy.kind == QStringLiteral("monitor_line")
        || copy.kind == QStringLiteral("task_notification")) {
        emit this->taskNotificationReady(formatTaskNotification(copy), copy.agentId);
    }
}

QString QSocTaskEventQueue::formatTaskNotification(const QSocTaskEvent &event)
{
    QString out = QStringLiteral("<task-notification>\n");
    out += tag(QStringLiteral("task-id"), event.taskId);
    out += tag(QStringLiteral("source"), event.sourceTag);
    out += tag(QStringLiteral("kind"), event.kind);
    out += tag(QStringLiteral("status"), event.status);
    out += tag(QStringLiteral("summary"), event.description);
    out += tag(QStringLiteral("output-file"), event.outputFile);
    out += tag(QStringLiteral("content"), event.content);
    out += QStringLiteral("</task-notification>");
    return out;
}

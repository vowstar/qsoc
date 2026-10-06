// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsoctaskeventqueue.h"
#include "agent/protocol/qsocmessagemarkup.h"

#include <QDateTime>

namespace {

QString tag(const QString &name, const QString &value)
{
    if (value.isEmpty())
        return {};
    return QStringLiteral("<%1>%2</%1>\n").arg(name, value.toHtmlEscaped());
}

QString keyFor(const QSocTaskEvent &event)
{
    return event.sourceTag + QLatin1Char('/') + event.taskId;
}

QString either(const QString &newer, const QString &older)
{
    return newer.isEmpty() ? older : newer;
}

/* Keep the newest lines: cut at a line start inside the last `limit` chars. */
QString keepTail(const QString &text, qsizetype limit)
{
    if (text.size() <= limit)
        return text;
    QString   tail  = text.right(limit);
    const int start = tail.indexOf(QLatin1Char('\n'));
    if (start >= 0 && start + 1 < tail.size())
        tail = tail.mid(start + 1);
    return tail;
}

QSocTaskEvent merged(const QSocTaskEvent &older, const QSocTaskEvent &newer)
{
    QSocTaskEvent out    = newer;
    out.description      = either(newer.description, older.description);
    out.outputFile       = either(newer.outputFile, older.outputFile);
    out.agentType        = either(newer.agentType, older.agentType);
    out.count            = older.count + newer.count;
    const QString joined = older.content.isEmpty()
                               ? newer.content
                               : older.content + QLatin1Char('\n') + newer.content;
    out.content          = keepTail(joined, QSocTaskNotices::contentChars);
    return out;
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
    if (notifies(copy)) {
        emit this->taskNotificationReady(formatTaskNotification(copy), copy.agentId);
    }
}

bool QSocTaskEventQueue::notifies(const QSocTaskEvent &event)
{
    return event.kind == QStringLiteral("monitor_line")
           || event.kind == QStringLiteral("task_notification");
}

QString QSocTaskEventQueue::formatTaskNotification(const QSocTaskEvent &event)
{
    QString out = QStringLiteral("<task-notification>\n");
    out += tag(QStringLiteral("task-id"), event.taskId);
    out += tag(QStringLiteral("source"), event.sourceTag);
    out += tag(QStringLiteral("kind"), event.kind);
    out += tag(QStringLiteral("subagent-type"), event.agentType);
    out += tag(QStringLiteral("status"), event.status);
    out += tag(QStringLiteral("summary"), event.description);
    out += tag(QStringLiteral("events"), event.count > 1 ? QString::number(event.count) : QString());
    out += tag(QStringLiteral("output-file"), event.outputFile);
    out += tag(QStringLiteral("content"), event.content);
    out += QStringLiteral("</task-notification>");
    return out;
}

QString QSocTaskEventQueue::summaryLine(const QSocTaskEvent &event)
{
    return QSocTaskNotificationText::summaryLine(
        {event.taskId,
         event.sourceTag,
         event.kind,
         event.status,
         event.agentType,
         event.description,
         event.content});
}

void QSocTaskNotices::forgetTaken(const std::function<bool(const QString &)> &stillQueued)
{
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (stillQueued(it.key())) {
            ++it;
            continue;
        }
        sizes_.remove(it.key());
        it = pending_.erase(it);
    }
    if (!stillQueued(QStringLiteral("qsoc/dropped")))
        dropped_ = 0;
}

QSocTaskNotices::Entry QSocTaskNotices::add(const QSocTaskEvent &event)
{
    const QString key   = keyFor(event);
    const auto    found = pending_.constFind(key);
    qsizetype     total = 0;
    for (const qsizetype size : std::as_const(sizes_))
        total += size;
    QSocTaskEvent next = found == pending_.cend() ? event : merged(*found, event);
    next.content       = keepTail(next.content, contentChars);
    const QString text = QSocTaskEventQueue::formatTaskNotification(next);
    const bool    full = pending_.size() >= maxEntries
                         || total - sizes_.value(key) + text.size() > maxChars;
    if (found == pending_.cend() && full) {
        QSocTaskEvent summary;
        summary.taskId      = QStringLiteral("dropped");
        summary.sourceTag   = QStringLiteral("qsoc");
        summary.kind        = QStringLiteral("task_notification");
        summary.status      = QStringLiteral("dropped");
        summary.description = QStringLiteral("%1 more background events dropped").arg(++dropped_);
        return {QStringLiteral("qsoc/dropped"), QSocTaskEventQueue::formatTaskNotification(summary)};
    }
    pending_.insert(key, next);
    sizes_.insert(key, text.size());
    return {key, text};
}

#include "moc_qsoctaskeventqueue.cpp"

// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCTASKEVENTQUEUE_H
#define QSOCTASKEVENTQUEUE_H

#include <QHash>
#include <QList>
#include <QObject>
#include <QPair>
#include <QString>

#include <functional>

struct QSocTaskEvent
{
    QString taskId;
    QString sourceTag;
    QString kind;
    QString status;
    QString description;
    QString content;
    QString outputFile;
    QString agentId;
    QString agentType;
    int     count       = 1;
    qint64  createdAtMs = 0;

    /** Owner of a process the user started: never reaches the model. */
    static QString userOwner() { return QStringLiteral("user"); }
};

Q_DECLARE_METATYPE(QSocTaskEvent)

/**
 * @brief Queue boundary for background task events that should reach the agent.
 * @details Producers enqueue structured task events. Consumers can inspect the
 *          raw event, or use taskNotificationReady() to inject the XML-wrapped
 *          model-facing notification into an agent input queue.
 */
class QSocTaskEventQueue : public QObject
{
    Q_OBJECT

public:
    explicit QSocTaskEventQueue(QObject *parent = nullptr);

    void enqueue(const QSocTaskEvent &event);

    /** The one model-facing envelope; every field is escaped. */
    static QString formatTaskNotification(const QSocTaskEvent &event);
    /** Whether the event carries a notification for the model. */
    static bool notifies(const QSocTaskEvent &event);
    /** One display line built from the fields, never from the envelope. */
    static QString summaryLine(const QSocTaskEvent &event);

signals:
    void taskEventQueued(const QSocTaskEvent &event);
    void taskNotificationReady(const QString &message, const QString &agentId);
};

/**
 * @brief Pending notifications of one agent: one entry per task, bounded.
 * @details A later event of a task folds into its pending entry, so a chatty
 *          source costs one queued notification. New tasks beyond the bound
 *          are counted into a single "dropped" entry.
 */
class QSocTaskNotices
{
public:
    static constexpr int       maxEntries   = 32;
    static constexpr qsizetype maxChars     = 1024 * 1024;
    static constexpr qsizetype contentChars = 8192;

    using Entry = QPair<QString, QString>; /* queue key, notification text */

    /** Forget entries the consumer has already taken. */
    void forgetTaken(const std::function<bool(const QString &key)> &stillQueued);
    /** Fold one event in; returns the entry to add or replace. */
    Entry add(const QSocTaskEvent &event);

private:
    QHash<QString, QSocTaskEvent> pending_;
    QHash<QString, qsizetype>     sizes_;
    int                           dropped_ = 0;
};

#endif /* QSOCTASKEVENTQUEUE_H */

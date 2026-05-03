// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCTASKEVENTQUEUE_H
#define QSOCTASKEVENTQUEUE_H

#include <QObject>
#include <QString>

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
    qint64  createdAtMs = 0;
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

    static QString formatTaskNotification(const QSocTaskEvent &event);

signals:
    void taskEventQueued(const QSocTaskEvent &event);
    void taskNotificationReady(const QString &message, const QString &agentId);
};

#endif /* QSOCTASKEVENTQUEUE_H */

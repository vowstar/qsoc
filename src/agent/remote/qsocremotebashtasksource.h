// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCREMOTEBASHTASKSOURCE_H
#define QSOCREMOTEBASHTASKSOURCE_H

#include "common/qsoctasksource.h"

#include <QHash>
#include <QPointer>

class QSocRemoteConnection;
class QSocTaskEventQueue;

/**
 * @brief Remote background bash jobs as task rows.
 * @details Rows come from the connection's job ledger, so listing costs no
 *          round trip. A job the watcher sees end on its own is reported once
 *          as a terminal task and a task event for the agent that launched it;
 *          a job its owner stopped, or whose end bash_manage already read, is
 *          not reported.
 */
class QSocRemoteBashTaskSource : public QSocTaskSource
{
    Q_OBJECT

public:
    explicit QSocRemoteBashTaskSource(QSocRemoteConnection *conn, QObject *parent = nullptr);

    QString              sourceTag() const override { return QStringLiteral("rbash"); }
    QList<QSocTask::Row> listTasks() const override;
    QString              tailFor(const QString &id, int maxBytes) const override;
    bool                 killTask(const QString &id) override;

    /** Bus that carries job completions to the owning agent. */
    void setTaskEventQueue(QSocTaskEventQueue *queue);

private:
    void             settle(const QString &jobId, int exitCode, const QByteArray &tail);
    QSocTask::Status statusOf(const QString &jobId) const;

    QSocRemoteConnection            *conn_ = nullptr;
    QPointer<QSocTaskEventQueue>     eventQueue_;
    QHash<QString, QSocTask::Status> ended_;
    QHash<QString, QString>          tails_;
};

#endif /* QSOCREMOTEBASHTASKSOURCE_H */

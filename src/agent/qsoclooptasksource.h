// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCLOOPTASKSOURCE_H
#define QSOCLOOPTASKSOURCE_H

#include "agent/qsoctasksource.h"

class QSocLoopScheduler;

/**
 * @brief Adapter exposing QSocLoopScheduler jobs as task overlay rows.
 * @details Listens to scheduler::jobsChanged for shape changes (add /
 *          remove / clear / one-shot fire-and-erase). Recurring fires
 *          alone do not retrigger because they only update lastFiredAt
 *          without changing the job count or composition.
 */
class QSocLoopTaskSource : public QSocTaskSource
{
    Q_OBJECT

public:
    explicit QSocLoopTaskSource(QSocLoopScheduler *scheduler, QObject *parent = nullptr);
    ~QSocLoopTaskSource() override = default;

    QString              sourceTag() const override { return QStringLiteral("loop"); }
    QList<QSocTask::Row> listTasks() const override;
    QString              tailFor(const QString &id, int maxBytes) const override;
    bool                 killTask(const QString &id) override;

private:
    QSocLoopScheduler *scheduler_ = nullptr;
};

#endif /* QSOCLOOPTASKSOURCE_H */

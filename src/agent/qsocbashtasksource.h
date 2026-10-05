// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCBASHTASKSOURCE_H
#define QSOCBASHTASKSOURCE_H

#include "common/qsoctasksource.h"

#include <QPointer>

class QSocTaskEventQueue;
class QSocToolShellBash;

/**
 * @brief Adapter exposing background bash processes as task overlay rows.
 * @details Listens to bashTool::backgroundProcessFinished and
 *          ::processStuckDetected so the overlay refreshes when processes
 *          exit or get flagged. The bash tool's activeProcesses map is
 *          static so multiple sources would step on each other; the
 *          design intentionally has one source instance per agent. A job
 *          that ends on its own, unread and not stopped, is reported once
 *          as a terminal task and a task event for its owner.
 */
class QSocBashTaskSource : public QSocTaskSource
{
    Q_OBJECT

public:
    explicit QSocBashTaskSource(QSocToolShellBash *bashTool, QObject *parent = nullptr);
    ~QSocBashTaskSource() override = default;

    QString              sourceTag() const override { return QStringLiteral("bg"); }
    QList<QSocTask::Row> listTasks() const override;
    QString              tailFor(const QString &id, int maxBytes) const override;
    bool                 killTask(const QString &id) override;

    /** Bus that carries background completions to the owning agent. */
    void setTaskEventQueue(QSocTaskEventQueue *queue);

private:
    void settle(int processId);

    QSocToolShellBash           *bashTool_ = nullptr;
    QPointer<QSocTaskEventQueue> eventQueue_;
};

#endif /* QSOCBASHTASKSOURCE_H */

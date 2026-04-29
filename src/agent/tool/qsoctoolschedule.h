// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCTOOLSCHEDULE_H
#define QSOCTOOLSCHEDULE_H

#include "agent/qsoctool.h"

class QSocLoopScheduler;

/**
 * @brief Tool to schedule a prompt or slash command for later or recurring execution.
 * @details Use proactively when the user asks to monitor, poll, retry, remind,
 *          "check back later", or "every N min/hour/day". Time is expressed as
 *          a 5-field cron string in local time. Recurring (`*\/5 * * * *`) and
 *          one-shot pinned dates (`30 14 27 2 *`) share the same field.
 */
class QSocToolScheduleCreate : public QSocTool
{
    Q_OBJECT

public:
    explicit QSocToolScheduleCreate(
        QObject *parent = nullptr, QSocLoopScheduler *scheduler = nullptr);
    ~QSocToolScheduleCreate() override;

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;

private:
    QSocLoopScheduler *scheduler_ = nullptr;
};

/**
 * @brief Tool to list all scheduled tasks created by `/loop` or schedule_create.
 */
class QSocToolScheduleList : public QSocTool
{
    Q_OBJECT

public:
    explicit QSocToolScheduleList(QObject *parent = nullptr, QSocLoopScheduler *scheduler = nullptr);
    ~QSocToolScheduleList() override;

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;

private:
    QSocLoopScheduler *scheduler_ = nullptr;
};

/**
 * @brief Tool to cancel a scheduled task by id.
 */
class QSocToolScheduleDelete : public QSocTool
{
    Q_OBJECT

public:
    explicit QSocToolScheduleDelete(
        QObject *parent = nullptr, QSocLoopScheduler *scheduler = nullptr);
    ~QSocToolScheduleDelete() override;

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;

private:
    QSocLoopScheduler *scheduler_ = nullptr;
};

#endif /* QSOCTOOLSCHEDULE_H */

// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCTOOLPLANMODE_H
#define QSOCTOOLPLANMODE_H

#include "agent/qsoctool.h"

#include <functional>

/**
 * @brief Outcome of presenting a plan to the user for approval.
 */
struct QSocPlanApproval
{
    bool    approved = false;
    QString feedback; /**< Free-form note when the user keeps planning. */
};

/**
 * @brief enter_plan_mode: the model switches itself into read-only
 *        planning before touching a non-trivial task.
 * @details Read-only and callback-driven, mirroring claude-code's
 *          EnterPlanMode. The CLI installs a callback that flips the
 *          agent into plan mode and lights the status chip; the
 *          sub-agent dispatch path is gated off in QSocAgent so a child
 *          can never toggle the parent's plan state.
 */
class QSocToolEnterPlanMode : public QSocTool
{
    Q_OBJECT

public:
    using Callback = std::function<void()>;

    explicit QSocToolEnterPlanMode(QObject *parent = nullptr, Callback onEnter = {});

    /** @brief Install or replace the enter-plan-mode callback. */
    void setCallback(Callback onEnter);

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;
    bool    isReadOnly() const override { return true; }

private:
    Callback onEnter_;
};

/**
 * @brief exit_plan_mode: present the finished plan for user approval.
 * @details Read-only for gating purposes. The CLI installs a callback
 *          that shows the plan and an Approve / Keep-planning choice,
 *          persists the approved plan, and clears plan mode. Reachable
 *          only inside plan mode and only on the main agent (both gated
 *          in QSocAgent).
 */
class QSocToolExitPlanMode : public QSocTool
{
    Q_OBJECT

public:
    using Callback = std::function<QSocPlanApproval(const QString &plan)>;

    explicit QSocToolExitPlanMode(QObject *parent = nullptr, Callback onExit = {});

    /** @brief Install or replace the approval callback. */
    void setCallback(Callback onExit);

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;
    bool    isReadOnly() const override { return true; }

private:
    Callback onExit_;
};

#endif // QSOCTOOLPLANMODE_H

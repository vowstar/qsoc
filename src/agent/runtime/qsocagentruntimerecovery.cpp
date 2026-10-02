// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>
#include "agent/qsocagent.h"
#include "agent/qsocgoal.h"
#include "agent/qsocgoalprompt.h"
#include "agent/runtime/qsocagentruntime_p.h"
#include <QScopedValueRollback>

namespace {
QString activeGoalId(QSocGoalCatalog *catalog)
{
    const auto goal = catalog ? catalog->current() : std::nullopt;
    return goal && goal->status == QSocGoalStatus::Active ? goal->id : QString();
}
} // namespace

void QSocAgentRuntime::prepareRecovery()
{
    const auto run = QSocSession::latestRun(d->currentSession->filePath());
    if (!run || !run->isRunning())
        return;
    if (!QSocSession::hasRecoveryClaim(run->runId)) {
        emitOutput("Interrupted run not continued: no local recovery authorization.\n");
        d->recoveryRequiresInput = true;
        return;
    }
    QSocSession::RunRecord context;
    d->applyRunContext(this, context);
    auto plan = QSocSessionRecovery::makePlan(
        run, d->persistedMessages, context, activeGoalId(d->goalCatalog));
    QSocSessionRecovery::guardHookReplay(plan, d->agent->getConfig().hooks);
    if (plan.action != QSocSessionRecovery::Action::Wait) {
        d->recoveryRun = run;
        return;
    }
    d->recoveryRequiresInput = true;
    // Incomplete tool batches are repaired with explicit uncertain/skipped
    // results. Never replay a tool whose side effects cannot be established.
    if (plan.messages != d->persistedMessages
        && QSocSessionRecovery::historySafeForNewTurn(plan.messages)
        && QSocAgentRuntimeInternal::persistRecoverySnapshot(
            d->currentSession.get(), plan.messages, d->persistedMessages, d->lastPersistedIndex)) {
        d->agent->setMessages(plan.messages);
        d->historyInputBlocked = false;
    }
    emitOutput("Interrupted run not continued: " + plan.reason + "\n");
}

bool QSocAgentRuntime::hasPendingRecovery() const
{
    return d->recoveryRun.has_value();
}

QSocAgentTurnResult QSocAgentRuntime::recoverPendingTurn()
{
    QSocAgentTurnResult result;
    const auto          run = std::exchange(d->recoveryRun, std::nullopt);
    if (!run)
        return result;
    QSocSession::RunRecord context;
    d->applyRunContext(this, context);
    auto plan = QSocSessionRecovery::makePlan(
        run, d->persistedMessages, context, activeGoalId(d->goalCatalog));
    QSocSessionRecovery::guardHookReplay(plan, d->agent->getConfig().hooks);
    if (!QSocSession::hasRecoveryClaim(run->runId)
        || plan.action == QSocSessionRecovery::Action::Wait
        || !QSocSessionRecovery::historySafeForNewTurn(plan.messages)
        || !QSocAgentRuntimeInternal::persistRecoverySnapshot(
            d->currentSession.get(), plan.messages, d->persistedMessages, d->lastPersistedIndex)) {
        result.error             = true;
        result.errorText         = "Interrupted run could not be recovered safely. " + plan.reason;
        d->recoveryRequiresInput = true;
        return result;
    }
    d->agent->setMessages(plan.messages);
    d->historyInputBlocked = false;
    d->activeRunId         = run->runId;
    QString input          = plan.input;
    if (plan.action == QSocSessionRecovery::Action::ContinueGoal) {
        const auto goal = d->goalCatalog->current();
        input           = QSocGoalPrompt::continuation(*goal);
        d->goalCatalog->noteContinuation("recovery");
    }
    QScopedValueRollback<bool>
        resume(d->resumeHistory, plan.action == QSocSessionRecovery::Action::ResumeHistory);
    emitOutput("(Continuing interrupted run)\n");
    return runTurn(input);
}

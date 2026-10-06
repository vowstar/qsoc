// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocagent.h"
#include "agent/qsocagentmailbox.h"
#include "agent/qsocsubagenttasksource.h"
#include "agent/runtime/qsocagentruntime_p.h"

#include <QScopedValueRollback>

/* One sink for every background source: monitors, sub-agents, background
 * bash. Mailbox replies to a main request stay in the mailbox, which the
 * drain and the inbox tools read, and only arm the gate. */
void QSocAgentRuntime::wireTaskBus()
{
    connect(
        d->taskEventQueue,
        &QSocTaskEventQueue::taskEventQueued,
        this,
        [this](const QSocTaskEvent &event) { deliverTaskEvent(event); });
    auto *mailbox = d->subAgentTaskSource->mailbox();
    connect(mailbox, &QSocAgentMailbox::changed, this, [this]() {
        if (pendingWakeWork() > 0)
            armWake();
    });
    d->agent->setGoalContinuationGate([this]() {
        if (!goalBelongsHere())
            return false;
        const int limit = d->agent->getConfig().backgroundWakeLimit;
        if (d->wake.origin == TurnOrigin::Wake && limit > 0 && d->wake.consecutive >= limit)
            return false;
        ++d->wake.consecutive;
        return true;
    });
}

void QSocAgentRuntime::deliverTaskEvent(const QSocTaskEvent &event)
{
    if (!QSocTaskEventQueue::notifies(event))
        return;
    QSocAgent *owner    = taskEventOwner(event.agentId);
    const bool forModel = owner != nullptr;
    const bool idle     = !d->agent->isRunning();
    if (forModel) {
        QSocTaskNotices &notices = owner == d->agent ? d->notices : d->childNotices[event.agentId];
        notices.forgetTaken(
            [owner](const QString &key) { return owner->hasQueuedNotification(key); });
        const auto [key, text] = notices.add(event);
        owner->queueTaskNotification(text, key);
        if (owner == d->agent)
            armWake();
    }
    if (!idle)
        return;
    QSocAgentRuntimeEvent display;
    display.kind = QSocAgentRuntimeEvent::Kind::TaskNotification;
    display.text = QSocTaskEventQueue::summaryLine(event);
    display.json
        = {{"source", event.sourceTag.toStdString()},
           {"task_id", event.taskId.toStdString()},
           {"status", event.status.toStdString()},
           {"to_model", forModel},
           {"body", QSocTaskEventQueue::formatTaskNotification(event).toStdString()}};
    display.at = QDateTime::currentDateTimeUtc();
    emit eventRaised(display);
}

/* The agent a task event is for: main for an empty owner, a live sub-agent by
 * its mailbox id, and nobody for the user or an agent that is gone. */
QSocAgent *QSocAgentRuntime::taskEventOwner(const QString &agentId)
{
    if (agentId.isEmpty() || agentId == d->agent->agentIdentity())
        return d->agent;
    auto *mailbox = d->subAgentTaskSource->mailbox();
    for (auto it = d->childNotices.begin(); it != d->childNotices.end();) {
        it = mailbox != nullptr && mailbox->agentFor(it.key()) != nullptr
                 ? std::next(it)
                 : d->childNotices.erase(it);
    }
    if (agentId == QSocTaskEvent::userOwner() || mailbox == nullptr)
        return nullptr;
    return mailbox->agentFor(agentId);
}

int QSocAgentRuntime::pendingWakeWork() const
{
    int   work    = d->agent->pendingNotificationCount();
    auto *mailbox = d->subAgentTaskSource->mailbox();
    if (mailbox == nullptr)
        return work;
    for (const auto &message : mailbox->take(d->agent->agentIdentity(), {}, {}, true)) {
        if (!message.value("reply_to", std::string()).empty())
            ++work;
    }
    return work;
}

/* The first arrival opens the debounce window. An arming older than one more
 * window found the gate shut or its work gone, so it opens a fresh one. */
void QSocAgentRuntime::armWake()
{
    const qint64 now = d->wake.clock();
    if (d->wake.dueAtMs == 0 || now - d->wake.dueAtMs > Private::Wake::debounceMs)
        d->wake.dueAtMs = now + Private::Wake::debounceMs;
}

void QSocAgentRuntime::settleWake(const QSocAgentTurnResult &result)
{
    if (result.error || result.aborted || !result.stopNotice.isEmpty())
        d->wake.latched = true;
    d->wake.origin  = TurnOrigin::User;
    d->wake.dueAtMs = 0;
    if (pendingWakeWork() > 0)
        armWake();
}

bool QSocAgentRuntime::hasPendingWake() const
{
    const QSocAgentConfig &config = d->agent->getConfig();
    const Private::Wake   &wake   = d->wake;
    const bool             open = config.backgroundWake && !d->options.singleQuery && !wake.latched
                                  && !planMode()
                                  && (config.backgroundWakeLimit == 0
                                      || wake.consecutive < config.backgroundWakeLimit);
    return open && wake.dueAtMs > 0 && wake.clock() >= wake.dueAtMs && !isRunning()
           && !hasPendingRecovery() && !d->recoveryRequiresInput && !d->historyInputBlocked
           && pendingWakeWork() > 0;
}

QSocAgentTurnResult QSocAgentRuntime::runWakeTurn()
{
    if (!hasPendingWake()) {
        QSocAgentTurnResult result;
        result.error     = true;
        result.errorText = QStringLiteral("no background notification is waiting");
        return result;
    }
    ++d->wake.consecutive;
    emitOutput(
        QStringLiteral("(background: %1 waiting, continuing)\n").arg(pendingWakeWork()),
        static_cast<int>(QSocAgentRuntimeStyle::Dim));
    const QScopedValueRollback<bool> resume(d->resumeHistory, true);
    return runTurn(QString(), TurnOrigin::Wake);
}

void QSocAgentRuntime::setWakeClock(std::function<qint64()> clock)
{
    d->wake.clock = std::move(clock);
}

// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocgoal.h"
#include "agent/runtime/qsocagentruntime_p.h"
#include "agent/services/qsocloopscheduler.h"

/* Project state (a resumed session, the goal, /loop tasks) belongs to the
 * workspace binding it was created on and acts only while that binding is
 * live. */

QSocWorkspaceBinding QSocAgentRuntime::liveBinding() const
{
    return isRemote() ? QSocWorkspaceBinding{d->remoteAlias, d->remoteConn->workspace()}
                      : QSocWorkspaceBinding{};
}

/* A record written before aliases were kept names the SSH target instead. */
bool QSocAgentRuntime::isLiveBinding(const QSocWorkspaceBinding &binding) const
{
    if (binding.isLocal() || !isRemote()) {
        return binding.isLocal() == !isRemote();
    }
    return binding.workspace == d->remoteConn->workspace()
           && (binding.target == d->remoteAlias || binding.target == d->remoteConn->target());
}

void QSocAgentRuntime::publishBinding()
{
    const QSocWorkspaceBinding live = liveBinding();
    d->goalCatalog->setBinding(live);
    d->loopScheduler->setBinding(live);
    d->goalWaitNoticed = false;
    if (d->staleBinding && isLiveBinding(*d->staleBinding)) {
        d->staleBinding.reset();
    }
}

QString QSocAgentRuntime::staleBindingText(const QSocWorkspaceBinding &recorded) const
{
    return QStringLiteral(
               "This session last ran on %1 and is now on %2, so its paths may name the "
               "other workspace. Tool calls are refused until you choose. /ssh %3 binds "
               "it again, as does starting with --ssh %3 --workspace %4. /local or "
               "/ssh <host> continues on the binding you pick.")
        .arg(recorded.label(), liveBinding().label(), recorded.target, recorded.workspace);
}

bool QSocAgentRuntime::goalBelongsHere()
{
    const auto foreign = d->goalCatalog->foreignBinding();
    if (!foreign) {
        return true;
    }
    if (!d->goalWaitNoticed) {
        d->goalWaitNoticed = true;
        emitOutput(
            QStringLiteral(
                "Goal paused: it was set on %1 and this session is on %2. It continues when "
                "%1 is bound again.\n")
                .arg(foreign->label(), liveBinding().label()),
            static_cast<int>(QSocAgentRuntimeStyle::Warning));
    }
    return false;
}

std::optional<QSocWorkspaceBinding> QSocAgentRuntime::foreignSessionBinding(
    const QString &sessionPath) const
{
    const auto run = QSocSession::latestRun(sessionPath);
    if (!run || !run->contextPresent || !run->remoteMode) {
        return std::nullopt;
    }
    const QSocWorkspaceBinding
        recorded{run->remoteAlias.isEmpty() ? run->remoteName : run->remoteAlias, run->projectRoot};
    if (isLiveBinding(recorded)) {
        return std::nullopt;
    }
    return recorded;
}

QSocAgentRuntime::ResumeBinding QSocAgentRuntime::chooseResumeBinding(
    const QSocWorkspaceBinding &recorded)
{
    if (!d->menu || d->options.singleQuery) {
        return ResumeBinding::Undecided;
    }
    const QString here   = liveBinding().label();
    const int     picked = d->menu(
        QStringLiteral("This session last ran on %1. Bind it again?").arg(recorded.label()),
        {QStringLiteral("Rebind to %1").arg(recorded.label()),
         QStringLiteral("Continue here on %1").arg(here),
         QStringLiteral("Cancel the resume")},
        {QStringLiteral("tool calls reach the workspace the session used"),
         QStringLiteral("paths in the history may not exist here"),
         QString()},
        {});
    switch (picked) {
    case 0:
        return ResumeBinding::Rebind;
    case 1:
        return ResumeBinding::Keep;
    default:
        return ResumeBinding::Cancel;
    }
}

void QSocAgentRuntime::applyResumeBinding(const QSocWorkspaceBinding &recorded, ResumeBinding choice)
{
    if (choice == ResumeBinding::Rebind) {
        emitOutput(QStringLiteral("Connecting to %1 ...\n").arg(recorded.label()));
        QString error;
        if (connectRemote(
                {.target = recorded.target, .workspace = recorded.workspace, .remember = false},
                &error)) {
            d->resumeBindingChosen = true;
            return;
        }
        emitOutput(
            QStringLiteral("Could not bind %1 again: %2\n").arg(recorded.label(), error),
            static_cast<int>(QSocAgentRuntimeStyle::Warning));
        choice = ResumeBinding::Undecided;
    }
    if (choice == ResumeBinding::Keep) {
        d->resumeBindingChosen = true;
        return;
    }
    d->staleBinding = recorded;
    emitOutput(
        staleBindingText(recorded) + QLatin1Char('\n'),
        static_cast<int>(QSocAgentRuntimeStyle::Warning));
}

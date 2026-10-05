// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

/**
 * @file qsocagentruntimecommand.cpp
 * @brief Runtime-owned command execution (slash commands, !, #).
 * @details Presentation stays with the frontend: every user-visible line
 *          is emitted as an Output event, and every interactive step is
 *          delegated to an installed handler.
 */

#include "agent/runtime/qsocagentruntime.h"
#include "agent/runtime/qsocagentruntime_p.h"

#include "agent/mcp/qsocmcpclient.h"
#include "agent/mcp/qsocmcpmanager.h"
#include "agent/qsocagent.h"
#include "agent/qsocagentdefinitionregistry.h"
#include "agent/qsocfilehistory.h"
#include "agent/qsocgoal.h"
#include "agent/qsocmemorymanager.h"
#include "agent/qsocmemoryrecall.h"
#include "agent/qsocrewind.h"
#include "agent/qsocsession.h"
#include "agent/qsocsubagenttasksource.h"
#include "agent/remote/qsocagentremote.h"
#include "agent/remote/qsocsftpclient.h"
#include "agent/remote/qsoctoolremote.h"
#include "agent/services/qsocloopscheduler.h"
#include "agent/tool/qsoctoolagent.h"
#include "agent/tool/qsoctoolshell.h"
#include "agent/tool/qsoctoolskill.h"
#include "common/qllmservice.h"
#include "common/qsocconfig.h"
#include "common/qsoccron.h"
#include "common/qsoclinediff.h"
#include "common/qsocmachine.h"
#include "common/qsocmessageauthority.h"
#include "common/qsocshellpath.h"

#include <QDeadlineTimer>
#include <QProcess>
#include <QTimer>
#ifdef Q_OS_UNIX
#include <signal.h>
#endif
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>

#include <cstdlib>
#ifdef Q_OS_UNIX
#include <sys/wait.h>
#include <unistd.h>
#endif
#ifdef Q_OS_WIN
#include <windows.h>
#endif

using json = nlohmann::json;

using QSocAgentRuntimeInternal::existingSessionPathIsRegular;
using QSocAgentRuntimeInternal::freshSessionPathAvailable;
using QSocAgentRuntimeInternal::lockSession;
using QSocAgentRuntimeInternal::persistRecoverySnapshot;
using QSocAgentRuntimeInternal::persistSessionState;
using QSocAgentRuntimeInternal::sessionProjectPath;

QString QSocAgentRuntimeInternal::runLocalShellEscape(
    const QString &command, const QString &directory, std::stop_token stop)
{
    QProcess process;
    process.setWorkingDirectory(directory);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0) && defined(Q_OS_UNIX)
    process.setChildProcessModifier([] { ::setsid(); });
#endif
    process.setProcessChannelMode(QProcess::MergedChannels);
    const QSocMachine machine = localMachine();
    if (machineShellEscapeMode(machine) == QSocShellEscapeMode::Passthrough)
        return QStringLiteral("Error: no shell to run the line (%1)\n").arg(machine.shellError);
#ifdef Q_OS_WIN
    process.setProgram(QStringLiteral("cmd.exe"));
    process.setNativeArguments(QSocShellPath::cmdExeNativeArguments(command));
    /* Always a console of its own, so its code page is the OEM one. */
    process.setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments *args) { args->flags |= CREATE_NO_WINDOW; });
#else
    process.setProgram(machine.shell.path);
    process.setArguments({QStringLiteral("-c"), command});
#endif
    process.start();
    if (!process.waitForStarted())
        return process.errorString() + QLatin1Char('\n');
    QEventLoop loop;
    QObject::connect(&process, &QProcess::finished, &loop, &QEventLoop::quit);
    QTimer cancellation;
    cancellation.setInterval(25);
    QObject::connect(&cancellation, &QTimer::timeout, &loop, [&] {
        if (!stop.stop_requested())
            return;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0) && defined(Q_OS_UNIX)
        if (process.processId() > 0)
            ::kill(-process.processId(), SIGKILL);
#endif
        process.kill();
    });
    cancellation.start();
    if (process.state() != QProcess::NotRunning)
        loop.exec();
    QString    result;
    const auto append = [&result](const QString &text) {
        result += text;
        if (!result.isEmpty() && !result.endsWith(QLatin1Char('\n')))
            result += QLatin1Char('\n');
    };
    append(QSocShellPath::decodeConsoleOutput(process.readAll()));
    if (process.exitCode() != 0)
        append(QStringLiteral("(exit code: %1)").arg(process.exitCode()));
    append(QStringLiteral("(shell: %1)").arg(shellEscapeShellName(machine)));
    return result;
}

namespace {

QString fmtTok(qint64 tokens)
{
    if (tokens >= 1000) {
        return QString::number(tokens / 1000.0, 'f', 1) + "k";
    }
    return QString::number(tokens);
}

} // namespace

bool QSocAgentRuntime::executeCommand(const QString &input)
{
    d->commandStop        = std::stop_source();
    d->cancelRequested    = false;
    const QString trimmed = input.trimmed();
    if (trimmed.isEmpty()) {
        return false;
    }

    /* `!command`: shell escape on the active workspace. */
    if (trimmed.startsWith(QLatin1Char('!'))) {
        const QString shellCmd = trimmed.mid(1).trimmed();
        if (shellCmd.isEmpty()) {
            return true;
        }
        emitOutput(
            QStringLiteral("$ ") + shellCmd + QStringLiteral("\n"),
            static_cast<int>(QSocAgentRuntimeStyle::Bold));
        const QString output = isRemote()
                                   ? runBoundRemoteShellEscape(d->remoteConn, shellCmd)
                                   : QSocAgentRuntimeInternal::runLocalShellEscape(
                                         shellCmd, workingDirectory(), d->commandStop.get_token());
        if (!output.isEmpty()) {
            emitOutput(output, static_cast<int>(QSocAgentRuntimeStyle::Dim));
        }
        return true;
    }

    /* `#fact`: quick memory write. */
    if (trimmed.startsWith(QLatin1Char('#'))) {
        const QString fact = trimmed.mid(1).simplified();
        auto         *mm   = d->agent->getMemoryManager();
        if (fact.isEmpty() || mm == nullptr) {
            emitOutput(
                QStringLiteral("Usage: #<fact to remember>\n"),
                static_cast<int>(QSocAgentRuntimeStyle::Dim));
            return true;
        }
        const QString name = fact.split(' ', Qt::SkipEmptyParts).mid(0, 6).join('-');
        static const QRegularExpression alnumRe(QStringLiteral("[A-Za-z0-9]"));
        if (!name.contains(alnumRe)) {
            emitOutput(
                QStringLiteral("Could not derive a topic name; include a word.\n"),
                static_cast<int>(QSocAgentRuntimeStyle::Dim));
            return true;
        }
        const bool ok = mm->writeTopicFile("project", name, "project", fact.left(60), fact);
        emitOutput(
            ok ? QStringLiteral("(remembered: %1)\n").arg(name)
               : QStringLiteral("Failed to save memory.\n"),
            static_cast<int>(QSocAgentRuntimeStyle::Dim));
        return true;
    }

    if (!trimmed.startsWith(QLatin1Char('/'))) {
        return false;
    }

    const int     spaceIdx = trimmed.indexOf(QLatin1Char(' '));
    const QString cmd      = (spaceIdx > 0 ? trimmed.left(spaceIdx) : trimmed).toLower();
    const QString rest     = spaceIdx > 0 ? trimmed.mid(spaceIdx + 1).trimmed() : QString();

    if (cmd == QStringLiteral("/help")) {
        emitOutput(QStringLiteral(
            "Commands\n"
            "  /help /status /context /cost /cache /compact /clear\n"
            "  /model [id] /effort [off|low|medium|high] /plan\n"
            "  /resume [id] /rewind /branch [name] /rename <title> /diff /btw <question>\n"
            "  /memory /goal /agents /agents-history /mcp /loop\n"
            "  /cwd [path] /project [path] /ssh [target] /local\n"
            "  !command runs a shell command; #fact saves memory\n"
            "Keyboard shortcuts:\n"
            "  /exit quits; Esc stops; Ctrl+R searches history\n"
            "  Ctrl+G opens the editor; Shift+Tab toggles plan mode\n"));
        return true;
    }
    if (cmd == QStringLiteral("/model")) {
        QString selected = rest;
        if (selected.isEmpty() && d->menu) {
            const auto  models = availableModels();
            QList<bool> marked;
            for (const auto &model : models)
                marked.append(model == currentModelId());
            const int index = d->menu(QStringLiteral("Model Selection"), models, {}, marked);
            if (index >= 0 && index < models.size())
                selected = models[index];
        }
        if (!selected.isEmpty() && !setCurrentModel(selected))
            emitOutput(QStringLiteral("Unknown model: %1\n").arg(selected));
        else if (selected.isEmpty())
            emitOutput(QStringLiteral("Model: %1\nAvailable: %2\n")
                           .arg(currentModelId(), availableModels().join(", ")));
        return true;
    }
    if (cmd == QStringLiteral("/effort")) {
        const QStringList levels   = {"off", "low", "medium", "high"};
        QString           selected = rest.toLower();
        if (selected.isEmpty() && d->menu) {
            QList<bool> marked;
            for (const auto &level : levels)
                marked.append(level == effortLevel());
            const int index = d->menu(QStringLiteral("Reasoning effort"), levels, {}, marked);
            if (index >= 0 && index < levels.size())
                selected = levels[index];
        }
        if (levels.contains(selected))
            setEffortLevel(selected == "off" ? QString() : selected);
        else if (!selected.isEmpty())
            emitOutput(QStringLiteral("Expected off, low, medium, or high.\n"));
        return true;
    }
    if (cmd == QStringLiteral("/resume")) {
        using ResumeKind = QSocSession::ResumeTarget::Kind;
        const auto target
            = QSocSession::resolveResume(sessionProjectPath(d->projectManager), rest, sessionId());
        if (target.kind == ResumeKind::Empty) {
            emitOutput(QStringLiteral("No other saved session to resume.\n"));
            return true;
        }
        if (target.kind == ResumeKind::NoMatch) {
            emitOutput(QStringLiteral("No session matches '%1'.\n").arg(rest));
            return true;
        }
        if (target.kind == ResumeKind::Current) {
            emitOutput(QStringLiteral("Already in session %1.\n").arg(target.id.left(8)));
            return true;
        }
        QString selected = target.id;
        if (target.kind == ResumeKind::Pick && d->menu) {
            QStringList labels, hints;
            for (const auto &session : target.choices) {
                QString label = !session.title.isEmpty()         ? session.title
                                : !session.firstPrompt.isEmpty() ? session.firstPrompt
                                : !session.branch.isEmpty()      ? session.branch
                                                                 : QStringLiteral("(empty)");
                label.replace(QRegularExpression(QStringLiteral("[\\r\\n]+")), " ");
                labels.append(label);
                hints.append(QStringLiteral("%1 · %2 msgs · %3")
                                 .arg(session.id.left(8))
                                 .arg(session.messageCount)
                                 .arg(session.lastModified.toLocalTime().toString(Qt::ISODate)));
            }
            const int index = d->menu(QStringLiteral("Resume session"), labels, hints, {});
            if (index >= 0 && index < target.choices.size())
                selected = target.choices[index].id;
        }
        if (selected.isEmpty())
            emitOutput(QStringLiteral("Resume cancelled.\n"));
        else if (!openSessionById(selected))
            emitOutput(lastError() + "\n");
        return true;
    }

    if (cmd == QStringLiteral("/rewind")) {
        if (!d->menu || !d->currentSession || d->agent->isRunning())
            return true;
        if (d->subAgentTaskSource && d->subAgentTaskSource->hasUnsettledRun()) {
            emitOutput("Rewind refused: a sub-agent is still running.\n");
            return true;
        }
        const json  messages = d->agent->getMessages();
        QList<int>  indexes;
        QStringList labels;
        for (int i = 0; i < static_cast<int>(messages.size()); ++i) {
            const auto &message = messages[i];
            if (message.value("role", std::string()) == "user" && message.contains("content")
                && message["content"].is_string()
                && !QSocMessageAuthority::isRuntimeReminder(message)) {
                indexes.append(i);
                labels.append(
                    QString::fromStdString(message["content"].get<std::string>()).left(100));
            }
        }
        if (indexes.isEmpty()) {
            emitOutput("No messages to rewind.\n");
            return true;
        }
        const int selection = d->menu("Rewind to message", labels, {}, {});
        if (selection < 0 || selection >= indexes.size())
            return true;
        const int mode = d->menu(
            "Rewind mode", {"Conversation and files", "Conversation only", "Files only"}, {}, {});
        if (mode < 0)
            return true;
        QSocRewindRequest request;
        request.restoreConversation = mode != 2;
        request.restoreFiles        = mode != 1;
        request.targetSnapshot      = selection;
        request.keptMessages        = json::array();
        for (int i = 0; i < indexes[selection]; ++i)
            request.keptMessages.push_back(messages[i]);
        request.originalCreatedAt = QSocSession::readInfo(d->currentSession->filePath()).createdAt;
        const auto result
            = qsocApplyRewind(request, d->currentSession.get(), d->currentFileHistory.get(), [this] {
                  return remoteWorkspaceRewindRefusal(d->remoteConn, 5000);
              });
        if (result.outcome != QSocRewindResult::Outcome::Refused && request.restoreConversation) {
            d->agent->setMessages(request.keptMessages);
            d->persistedMessages   = request.keptMessages;
            d->lastPersistedIndex  = result.kept;
            d->memoryCursor.index  = qMin(d->memoryCursor.index, result.kept);
            d->historyInputBlocked = false;
            QSocAgentRuntimeEvent event;
            event.kind = QSocAgentRuntimeEvent::Kind::SessionResumed;
            event.json
                = {{"messages", request.keptMessages},
                   {"input", messages[indexes[selection]]["content"]}};
            emit eventRaised(event);
        }
        if (qsocRewindMovesTurnCounter(request, result))
            d->turnCounter = request.targetSnapshot;
        emitOutput(qsocRewindReport(request, result));
        return true;
    }
    if (cmd == QStringLiteral("/btw")) {
        if (rest.isEmpty()) {
            emitOutput("Usage: /btw <question>\n");
            return true;
        }
        json messages = json::array();
        messages.push_back(
            {{"role", "system"}, {"content", d->agent->requestSystemPrompt().toStdString()}});
        for (const auto &message : d->agent->getMessages())
            messages.push_back(QSocMessageAuthority::toWire(message));
        messages.push_back(
            {{"role", "user"},
             {"content",
              (QStringLiteral(
                   "Answer this side question in one response using the conversation context. "
                   "You have no tools and no follow-up turn.\n\n")
               + rest)
                  .toStdString()}});
        std::unique_ptr<QLLMService> llm(d->llmService->clone(nullptr));
        const auto                   result = llm->sendChatCompletion(
            messages,
            json::array(),
            d->agent->getConfig().temperature,
            d->commandStop.get_token(),
            d->agent->getConfig().effortLevel);
        if (result.contains("choices") && result["choices"].is_array()
            && !result["choices"].empty()) {
            const auto &message = result["choices"][0]["message"];
            if (message.contains("content") && message["content"].is_string())
                emitOutput(QString::fromStdString(message["content"].get<std::string>()) + "\n");
        } else
            emitOutput(QStringLiteral("Side question failed: %1\n")
                           .arg(QString::fromStdString(result.dump())));
        return true;
    }

    if (cmd == QStringLiteral("/compact")) {
        emitOutput(QStringLiteral("Compacting...\n"), static_cast<int>(QSocAgentRuntimeStyle::Dim));
        compactNow();
        return true;
    }

    if (cmd == QStringLiteral("/clear")) {
        clearSession();
        return true;
    }

    if (cmd == QStringLiteral("/status")) {
        emitOutput(QStringLiteral("\nStatus\n"), static_cast<int>(QSocAgentRuntimeStyle::Bold));
        emitOutput(
            QStringLiteral("  Model:    %1\n")
                .arg(currentModelId().isEmpty() ? QStringLiteral("(none)") : currentModelId()));
        emitOutput(QStringLiteral("  Effort:   %1\n").arg(effortLevel()));
        if (isRemote()) {
            emitOutput(QStringLiteral("  Remote:   %1\n").arg(remoteTarget()));
            emitOutput(QStringLiteral("  Workspace:%1\n").arg(remoteWorkspace()));
        } else {
            emitOutput(QStringLiteral("  Remote:   (local mode)\n"));
        }
        if (d->currentSession) {
            emitOutput(QStringLiteral("  Session:  %1 (%2 messages)\n")
                           .arg(d->currentSession->id().left(8))
                           .arg(d->agent->getMessages().size()));
        }
        emitOutput(QStringLiteral("  Project:  %1\n")
                       .arg(
                           d->projectManager->getProjectPath().isEmpty()
                               ? QStringLiteral("(none)")
                               : d->projectManager->getProjectPath()));
        if (d->currentFileHistory && !d->currentFileHistory->isEmpty()) {
            emitOutput(
                QStringLiteral("  File history: %1 snapshot(s)\n")
                    .arg(d->currentFileHistory->listSnapshots().size()),
                static_cast<int>(QSocAgentRuntimeStyle::Dim));
        }
        emitOutput(QStringLiteral("\n"));
        return true;
    }

    if (cmd == QStringLiteral("/context")) {
        emitOutput(QStringLiteral("\nContext\n"), static_cast<int>(QSocAgentRuntimeStyle::Bold));
        const UsageSnapshot snapshot = usage();
        emitOutput(QStringLiteral("  Used:      %1%2 / %3 tokens\n")
                       .arg(snapshot.approximate ? QStringLiteral("\u2248") : QString())
                       .arg(fmtTok(snapshot.usedTokens))
                       .arg(fmtTok(snapshot.maxTokens)));
        emitOutput(QStringLiteral("  Threshold: %1%\n\n").arg(int(snapshot.compactThreshold * 100)));
        return true;
    }

    if (cmd == QStringLiteral("/cache")) {
        emitOutput(QStringLiteral(
            "\nCache diagnostics for the current LLM service.\n"
            "Calls count service invocations, not user turns.\n"
            "Missing values are not reported by the provider.\n"));
        if (d->llmService)
            emitOutput(
                QString::fromStdString(d->llmService->requestDiagnostics().dump(2))
                + QLatin1Char('\n'));
        return true;
    }

    if (cmd == QStringLiteral("/cost")) {
        emitOutput(QStringLiteral("\nSession Cost\n"), static_cast<int>(QSocAgentRuntimeStyle::Bold));
        const UsageSnapshot snapshot = usage();
        emitOutput(QStringLiteral("  Input tokens:  %1\n").arg(fmtTok(snapshot.inputTokens)));
        emitOutput(QStringLiteral("  Output tokens: %1\n").arg(fmtTok(snapshot.outputTokens)));
        double  inputRate  = 0;
        double  outputRate = 0;
        QString currency;
        if (d->socConfig) {
            const QString inStr  = d->socConfig->getValue("llm.cost_input_per_mtok");
            const QString outStr = d->socConfig->getValue("llm.cost_output_per_mtok");
            currency             = d->socConfig->getValue("llm.cost_currency");
            if (!inStr.isEmpty()) {
                inputRate = inStr.toDouble();
            }
            if (!outStr.isEmpty()) {
                outputRate = outStr.toDouble();
            }
        }
        if (inputRate > 0 || outputRate > 0) {
            if (currency.isEmpty()) {
                currency = QStringLiteral("USD");
            }
            const double inputCost  = snapshot.inputTokens * inputRate / 1e6;
            const double outputCost = snapshot.outputTokens * outputRate / 1e6;
            emitOutput(
                QStringLiteral("  Input cost:    %1 %2\n").arg(inputCost, 0, 'f', 4).arg(currency));
            emitOutput(
                QStringLiteral("  Output cost:   %1 %2\n").arg(outputCost, 0, 'f', 4).arg(currency));
            emitOutput(
                QStringLiteral("  Total:         %1 %2\n")
                    .arg(inputCost + outputCost, 0, 'f', 4)
                    .arg(currency),
                static_cast<int>(QSocAgentRuntimeStyle::Bold));
        } else {
            emitOutput(
                QStringLiteral("\n  Cost rates not configured. Set in project config:\n"),
                static_cast<int>(QSocAgentRuntimeStyle::Dim));
        }
        emitOutput(QStringLiteral("\n"));
        return true;
    }

    if (cmd == QStringLiteral("/rename")) {
        if (rest.isEmpty()) {
            emitOutput(QStringLiteral("Usage: /rename <title>\n"));
            return true;
        }
        if (!d->currentSession) {
            emitOutput(QStringLiteral("(no active session)\n"));
            return true;
        }
        d->currentSession->appendMeta(QStringLiteral("title"), rest);
        emitOutput(QStringLiteral("Session renamed to: %1\n").arg(rest));
        return true;
    }

    if (cmd == QStringLiteral("/plan")) {
        bool target = !planMode();
        if (rest == QStringLiteral("on")) {
            target = true;
        } else if (rest == QStringLiteral("off")) {
            target = false;
        }
        setPlanMode(target);
        emitOutput(
            target ? QStringLiteral(
                         "Plan mode ON: read-only until you approve a plan via "
                         "exit_plan_mode.\n")
                   : QStringLiteral("Plan mode OFF.\n"));
        return true;
    }

    if (cmd == QStringLiteral("/local")) {
        if (!isRemote()) {
            emitOutput(QStringLiteral("Already in local mode.\n"));
            return true;
        }
        disconnectRemote();
        emitOutput(QStringLiteral("Returned to local workspace (binding kept).\n"));
        return true;
    }

    if (cmd == QStringLiteral("/ssh")) {
        const QString target = rest.isEmpty() ? pickRemoteHost() : rest;
        if (target.isEmpty()) {
            return true;
        }
        emitOutput(QStringLiteral("Connecting to %1 ...\n").arg(target));
        QString error;
        if (!connectRemote(target, &error)) {
            emitOutput(error + QStringLiteral("\n"));
            return true;
        }
        emitOutput(QStringLiteral("Connected. Remote workspace: %1\n").arg(remoteWorkspace()));
        return true;
    }

    if (cmd == QStringLiteral("/cwd")) {
        if (rest.isEmpty()) {
            emitOutput(QStringLiteral("Working dir: %1\n").arg(workingDirectory()));
            return true;
        }
        QString error;
        if (!setWorkingDirectory(rest, &error)) {
            emitOutput(QStringLiteral("%1\n").arg(error));
        } else {
            emitOutput(QStringLiteral("Working dir: %1\n").arg(workingDirectory()));
        }
        return true;
    }

    if (cmd == QStringLiteral("/project")) {
        if (rest.isEmpty()) {
            const QString projectDir = d->projectManager->getProjectPath();
            emitOutput(QStringLiteral("Project: %1\n")
                           .arg(projectDir.isEmpty() ? QStringLiteral("(none)") : projectDir));
            return true;
        }
        QString error;
        if (!switchProject(rest, &error)) {
            emitOutput(QStringLiteral("%1\n").arg(error));
        } else {
            emitOutput(
                QStringLiteral("Project: %1 (new session %2)\n").arg(rest, sessionId().left(8)));
        }
        return true;
    }

    if (cmd == QStringLiteral("/goal")) {
        const auto cur = d->goalCatalog->current();
        if (rest.isEmpty()) {
            if (!cur.has_value()) {
                emitOutput(QStringLiteral(
                    "No active goal. Usage:\n"
                    "  /goal <objective>          set a new goal\n"
                    "  /goal pause                pause auto-continuation\n"
                    "  /goal resume               resume\n"
                    "  /goal budget <tokens>      set token budget (0 = none)\n"
                    "  /goal clear                drop the active goal\n"));
                return true;
            }
            QString line = QStringLiteral("Goal [%1]: %2\n")
                               .arg(qSocGoalStatusToString(cur->status), cur->objective);
            emitOutput(line);
            return true;
        }
        QString err;
        if (rest == QStringLiteral("clear")) {
            emitOutput(
                d->goalCatalog->clear(&err) ? QStringLiteral("Goal cleared.\n")
                                            : QStringLiteral("Error: %1\n").arg(err));
            return true;
        }
        if (rest == QStringLiteral("pause")) {
            emitOutput(
                d->goalCatalog->setStatus(QSocGoalStatus::Paused, &err)
                    ? QStringLiteral("Goal paused.\n")
                    : QStringLiteral("Error: %1\n").arg(err));
            return true;
        }
        if (rest == QStringLiteral("resume")) {
            emitOutput(
                d->goalCatalog->setStatus(QSocGoalStatus::Active, &err)
                    ? QStringLiteral("Goal resumed.\n")
                    : QStringLiteral("Error: %1\n").arg(err));
            return true;
        }
        if (rest.startsWith(QStringLiteral("budget"))) {
            bool      ok  = false;
            const int bud = rest.mid(6).trimmed().toInt(&ok);
            if (!ok || bud < 0) {
                emitOutput(QStringLiteral("Usage: /goal budget <non-negative integer>\n"));
                return true;
            }
            emitOutput(
                d->goalCatalog->setTokenBudget(bud, &err)
                    ? QStringLiteral("Goal token budget set to %1.\n").arg(bud)
                    : QStringLiteral("Error: %1\n").arg(err));
            return true;
        }
        if (cur.has_value()) {
            /* Replace needs a confirmation; use the menu handler when
             * available, otherwise refuse rather than silently replace. */
            if (!d->menu) {
                emitOutput(QStringLiteral(
                    "A goal is already active; confirm the replacement from an interactive "
                    "frontend.\n"));
                return true;
            }
            const int picked = d->menu(
                QStringLiteral("Replace goal '%1' with '%2'?")
                    .arg(cur->objective.left(40), rest.left(40)),
                {QStringLiteral("Keep current goal"), QStringLiteral("Replace with new objective")},
                {},
                {false, false});
            if (picked != 1) {
                emitOutput(QStringLiteral("Kept the current goal.\n"));
                return true;
            }
            emitOutput(
                d->goalCatalog->replace(rest, cur->tokenBudget, &err)
                    ? QStringLiteral("Goal replaced.\n")
                    : QStringLiteral("Error: %1\n").arg(err));
        } else {
            emitOutput(
                d->goalCatalog->create(rest, 0, &err) ? QStringLiteral("Goal set.\n")
                                                      : QStringLiteral("Error: %1\n").arg(err));
        }
        return true;
    }

    if (cmd == QStringLiteral("/memory")) {
        auto *mm = d->agent->getMemoryManager();
        if (mm == nullptr) {
            emitOutput(
                QStringLiteral("Memory not available.\n"),
                static_cast<int>(QSocAgentRuntimeStyle::Dim));
            return true;
        }
        if (rest.isEmpty()) {
            const auto headers = mm->scanHeaders("all");
            if (headers.isEmpty()) {
                emitOutput(
                    QStringLiteral("No memory yet. Use #<fact> to add one.\n"),
                    static_cast<int>(QSocAgentRuntimeStyle::Dim));
            } else {
                emitOutput(
                    QStringLiteral("Memory topics:\n"),
                    static_cast<int>(QSocAgentRuntimeStyle::Bold));
                for (const auto &header : headers) {
                    emitOutput(
                        QStringLiteral("  %1 [%2/%3] %4\n")
                            .arg(
                                header.name,
                                header.type.isEmpty() ? QStringLiteral("note") : header.type,
                                header.scope,
                                header.description),
                        static_cast<int>(QSocAgentRuntimeStyle::Dim));
                }
            }
            return true;
        }
        if (rest.startsWith(QStringLiteral("rm "), Qt::CaseInsensitive)) {
            const QString name = rest.mid(3).trimmed();
            QStringList   deleted;
            for (const auto &header : mm->scanHeaders("all")) {
                if (QString(header.name).toLower() != name.toLower()) {
                    continue;
                }
                if (mm->deleteTopicFile(header.scope, header.name)) {
                    deleted << QStringLiteral("%1 (%2)").arg(header.name, header.scope);
                }
            }
            emitOutput(
                deleted.isEmpty() ? QStringLiteral("No memory named '%1'.\n").arg(name)
                                  : QStringLiteral("Deleted %1.\n").arg(deleted.join(", ")),
                static_cast<int>(QSocAgentRuntimeStyle::Dim));
            return true;
        }
        /* Edit flow: needs the text-edit handler. */
        if (!d->textEdit) {
            emitOutput(
                QStringLiteral("Memory editing needs an interactive frontend.\n"),
                static_cast<int>(QSocAgentRuntimeStyle::Dim));
            return true;
        }
        QString canonical;
        QString scope;
        QString type;
        QString desc;
        for (const auto &header : mm->scanHeaders("all")) {
            if (QString(header.name).toLower() != rest.toLower()) {
                continue;
            }
            if (canonical.isEmpty()) {
                scope     = header.scope;
                type      = header.type;
                desc      = header.description;
                canonical = header.name;
            }
        }
        if (canonical.isEmpty()) {
            emitOutput(
                QStringLiteral("No memory named '%1'. Use '/memory' to list.\n").arg(rest),
                static_cast<int>(QSocAgentRuntimeStyle::Dim));
            return true;
        }
        const QString body = QSocMemoryRecall::stripFrontmatter(mm->readTopicFile(scope, canonical));
        QString edited;
        QString editErr;
        if (!d->textEdit(body, &edited, &editErr)) {
            emitOutput(
                QStringLiteral("Edit cancelled: %1\n").arg(editErr),
                static_cast<int>(QSocAgentRuntimeStyle::Dim));
            return true;
        }
        const bool saved = mm->writeTopicFile(scope, canonical, type, desc, edited);
        emitOutput(
            saved ? QStringLiteral("Updated memory '%1'.\n").arg(canonical)
                  : QStringLiteral("Failed to update '%1'.\n").arg(canonical),
            static_cast<int>(QSocAgentRuntimeStyle::Dim));
        return true;
    }

    if (cmd == QStringLiteral("/agents") || cmd == QStringLiteral("/agents-history")) {
        const bool history = cmd == QStringLiteral("/agents-history");
        if (history) {
            const auto runs = d->subAgentTaskSource->loadHistoricalRuns();
            if (runs.isEmpty()) {
                emitOutput(
                    QStringLiteral("No historical sub-agent runs on disk.\n"),
                    static_cast<int>(QSocAgentRuntimeStyle::Dim));
                return true;
            }
            emitOutput(QStringLiteral("Recent sub-agent runs:\n"));
            int shown = 0;
            for (const auto &run : runs) {
                if (shown >= 20) {
                    break;
                }
                ++shown;
                emitOutput(QStringLiteral("  %1 [%2] %3 (%4)%5\n")
                               .arg(run.id, run.subagentType, run.label, run.status)
                               .arg(run.legacy ? QStringLiteral(" legacy") : QString()));
                if (!run.error.isEmpty()) {
                    emitOutput(
                        QStringLiteral("    error: %1\n").arg(run.error),
                        static_cast<int>(QSocAgentRuntimeStyle::Dim));
                }
                if (!run.finalPreview.isEmpty()) {
                    emitOutput(
                        QStringLiteral("    final: %1\n").arg(run.finalPreview),
                        static_cast<int>(QSocAgentRuntimeStyle::Dim));
                }
            }
            return true;
        }
        QSocAgentDefinitionRegistry *defs = d->agentDefinitions;
        const QStringList            scopes
            = {QStringLiteral("builtin"), QStringLiteral("user"), QStringLiteral("project")};
        const QStringList scopeLabels
            = {QStringLiteral("Built-in"),
               QStringLiteral("User (~/.config/qsoc/agents/)"),
               QStringLiteral("Project (./.qsoc/agents/)")};
        int sectionsPrinted = 0;
        for (qsizetype si = 0; si < scopes.size(); ++si) {
            const QString &scope = scopes[si];
            QStringList    matching;
            for (const QString &name : defs->availableNames()) {
                const QSocAgentDefinition *def = defs->find(name);
                if (def != nullptr && def->scope == scope) {
                    matching.append(name);
                }
            }
            if (matching.isEmpty()) {
                continue;
            }
            if (sectionsPrinted > 0) {
                emitOutput(QStringLiteral("\n"));
            }
            emitOutput(scopeLabels[si] + QStringLiteral(":\n"));
            constexpr int kAgentDescCap = 120;
            for (const QString &name : matching) {
                const QSocAgentDefinition *def = defs->find(name);
                if (def == nullptr) {
                    continue;
                }
                QString desc = def->description;
                if (desc.size() > kAgentDescCap) {
                    desc = desc.left(kAgentDescCap - 3) + QStringLiteral("...");
                }
                emitOutput(
                    QStringLiteral("  ") + def->name + QStringLiteral(" - ") + desc
                    + QLatin1Char('\n'));
                if (!def->toolsAllow.isEmpty()) {
                    emitOutput(
                        QStringLiteral("    tools: ") + def->toolsAllow.join(QStringLiteral(", "))
                            + QLatin1Char('\n'),
                        static_cast<int>(QSocAgentRuntimeStyle::Dim));
                } else {
                    emitOutput(
                        QStringLiteral("    tools: (inherit parent set)\n"),
                        static_cast<int>(QSocAgentRuntimeStyle::Dim));
                }
            }
            ++sectionsPrinted;
        }
        if (sectionsPrinted == 0) {
            emitOutput(
                QStringLiteral("No sub-agent definitions registered.\n"),
                static_cast<int>(QSocAgentRuntimeStyle::Dim));
        }
        return true;
    }

    if (cmd == QStringLiteral("/mcp")) {
        if (d->mcpManager == nullptr || d->mcpManager->serverNames().isEmpty()) {
            emitOutput(
                QStringLiteral("(no MCP servers configured)\n"),
                static_cast<int>(QSocAgentRuntimeStyle::Dim));
            return true;
        }
        const QStringList parts = rest.isEmpty() ? QStringList()
                                                 : rest.split(QRegularExpression("\\s+"));
        if (parts.isEmpty() || parts.first() == "list") {
            emitOutput(
                QStringLiteral("\nMCP servers\n"), static_cast<int>(QSocAgentRuntimeStyle::Bold));
            for (const QString &name : d->mcpManager->serverNames()) {
                auto   *client = d->mcpManager->findClient(name);
                QString status;
                if (d->mcpManager->hasGivenUp(name)) {
                    status = QStringLiteral("failed");
                } else if (client == nullptr) {
                    status = QStringLiteral("unavailable");
                } else {
                    switch (client->state()) {
                    case QSocMcpClient::State::Disconnected:
                        status = QStringLiteral("disconnected");
                        break;
                    case QSocMcpClient::State::Connecting:
                        status = QStringLiteral("connecting");
                        break;
                    case QSocMcpClient::State::Initializing:
                        status = QStringLiteral("initializing");
                        break;
                    case QSocMcpClient::State::Ready:
                        status = QStringLiteral("ready");
                        break;
                    case QSocMcpClient::State::Failed:
                        status = QStringLiteral("failed");
                        break;
                    }
                }
                emitOutput(QStringLiteral("  %1  [%2]\n").arg(name, status));
            }
            emitOutput(QStringLiteral("\n"));
            return true;
        }
        if (parts.first() == "reconnect" && parts.size() >= 2) {
            const QString target = parts.mid(1).join(QChar(' '));
            if (!d->mcpManager->reconnectServer(target)) {
                emitOutput(
                    QStringLiteral("Unknown MCP server: %1\n").arg(target),
                    static_cast<int>(QSocAgentRuntimeStyle::Dim));
            } else {
                emitOutput(
                    QStringLiteral("Reconnecting %1 ...\n").arg(target),
                    static_cast<int>(QSocAgentRuntimeStyle::Dim));
            }
            return true;
        }
        emitOutput(
            QStringLiteral("Usage: /mcp [list|reconnect <name>]\n"),
            static_cast<int>(QSocAgentRuntimeStyle::Dim));
        return true;
    }

    if (cmd == QStringLiteral("/loop")) {
        const QString sub = rest.section(QRegularExpression("\\s+"), 0, 0).toLower();
        if (rest.isEmpty()) {
            emitOutput(QStringLiteral(
                "Usage: /loop [interval] <prompt>\n"
                "       /loop list\n"
                "       /loop stop <id>\n"
                "       /loop clear\n"
                "Intervals: Ns, Nm, Nh, Nd (e.g. 5m, 30m, 2h, 1d). Min 1 minute.\n"
                "If no interval is given, defaults to 10m. The parsed prompt also runs\n"
                "immediately once on creation.\n"));
            return true;
        }
        if (sub == "list") {
            const auto jobs = d->loopScheduler->listJobs();
            if (jobs.isEmpty()) {
                emitOutput(QStringLiteral("(no /loop jobs)\n"));
            } else {
                const qint64 now = QDateTime::currentMSecsSinceEpoch();
                for (const auto &job : jobs) {
                    const qint64 anchor = job.lastFiredAt > 0 ? job.lastFiredAt : job.createdAt;
                    const qint64 next   = QSocCron::nextRunMs(job.cron, anchor);
                    const qint64 due    = (next == 0) ? -1 : next - now;
                    QString      eta;
                    if (next == 0) {
                        eta = QStringLiteral("never");
                    } else if (due <= 0) {
                        eta = QStringLiteral("now");
                    } else if (due < 60000) {
                        eta = QStringLiteral("%1s").arg(due / 1000);
                    } else if (due < 3600000) {
                        eta = QStringLiteral("%1m").arg(due / 60000);
                    } else {
                        eta = QStringLiteral("%1h").arg(due / 3600000);
                    }
                    QString promptSummary = job.prompt;
                    promptSummary.replace(QLatin1Char('\n'), QLatin1Char(' '));
                    if (promptSummary.size() > 50) {
                        promptSummary = promptSummary.left(47) + QStringLiteral("...");
                    }
                    const QString kind  = job.recurring ? QStringLiteral("rec")
                                                        : QStringLiteral("once");
                    const QString where = job.durable ? QStringLiteral("disk")
                                                      : QStringLiteral("sess");
                    emitOutput(QStringLiteral("  %1  %2  %3 %4  next %5  %6\n")
                                   .arg(job.id, -8)
                                   .arg(QSocCron::cronToHuman(job.cron), -16)
                                   .arg(kind, -4)
                                   .arg(where, -4)
                                   .arg(eta, -5)
                                   .arg(promptSummary));
                }
            }
            return true;
        }
        const auto printNotOwner = [this]() {
            emitOutput(QStringLiteral(
                "Another qsoc session owns this project's /loop jobs. "
                "Run /loop add/stop/clear from that session.\n"));
        };
        const auto printPersistFailed = [this]() {
            emitOutput(QStringLiteral(
                "Failed to write .qsoc/loops.json. Check disk space and "
                "permissions; nothing was changed.\n"));
        };
        if (sub == "stop") {
            const QString id = rest.section(QRegularExpression("\\s+"), 1, 1);
            if (id.isEmpty()) {
                emitOutput(QStringLiteral("Usage: /loop stop <id>\n"));
                return true;
            }
            if (!d->loopScheduler->isOwner()) {
                printNotOwner();
                return true;
            }
            bool present = false;
            for (const auto &job : d->loopScheduler->listJobs()) {
                if (job.id == id) {
                    present = true;
                    break;
                }
            }
            if (!present) {
                emitOutput(QStringLiteral("No loop named %1.\n").arg(id));
            } else if (d->loopScheduler->removeJob(id)) {
                emitOutput(QStringLiteral("Removed loop %1.\n").arg(id));
            } else {
                printPersistFailed();
            }
            return true;
        }
        if (sub == "clear") {
            if (!d->loopScheduler->isOwner()) {
                printNotOwner();
                return true;
            }
            const int count = d->loopScheduler->listJobs().size();
            if (d->loopScheduler->clearJobs()) {
                emitOutput(QStringLiteral("Cleared %1 loop(s).\n").arg(count));
            } else {
                printPersistFailed();
            }
            return true;
        }
        if (!d->loopScheduler->isOwner()) {
            printNotOwner();
            return true;
        }
        QString cron;
        QString parsedPrompt;
        QString parseError;
        QSocLoopScheduler::parseLoopArgs(
            rest, QStringLiteral("*/10 * * * *"), cron, parsedPrompt, parseError);
        if (!parseError.isEmpty()) {
            emitOutput(parseError + QStringLiteral("\n"));
            return true;
        }
        if (cron.isEmpty() || parsedPrompt.isEmpty()) {
            emitOutput(QStringLiteral("Usage: /loop [interval] <prompt>\n"));
            return true;
        }
        const QString id = d->loopScheduler->addJob(cron, parsedPrompt, true, true);
        if (id.isEmpty()) {
            if (!d->loopScheduler->isOwner()) {
                printNotOwner();
            } else {
                printPersistFailed();
            }
            return true;
        }
        emitOutput(QStringLiteral("Scheduled loop %1 (%2): %3\n")
                       .arg(id)
                       .arg(QSocCron::cronToHuman(cron))
                       .arg(parsedPrompt));
        return true;
    }

    if (cmd == QStringLiteral("/branch")) {
        if (!d->currentSession) {
            emitOutput(QStringLiteral("(no active session to branch)\n"));
            return true;
        }
        if (!d->currentFileHistory || !d->currentFileHistory->storageIsBound()
            || QFileInfo(d->currentSession->filePath()).isSymLink()) {
            emitOutput(
                QStringLiteral("Session branch refused: the project storage binding changed.\n"));
            return true;
        }
        const json branchMessages = d->agent->getMessages();
        if (!branchMessages.is_array() || !d->currentSession->appendSnapshot(branchMessages)) {
            emitOutput(QStringLiteral(
                "Session branch refused: current history could not be persisted.\n"));
            return true;
        }
        d->persistedMessages       = branchMessages;
        d->lastPersistedIndex      = static_cast<int>(branchMessages.size());
        const QString projectPath  = sessionProjectPath(d->projectManager);
        const QString newId        = QSocSession::generateId();
        const QString sessions     = QSocSession::sessionsDir(projectPath);
        const QString newPath      = QDir(sessions).filePath(newId + ".jsonl");
        const QString sessionStage = QDir(sessions).filePath(
            QStringLiteral(".branch-") + newId + ".tmp");
        const QString srcHist = QSocFileHistory::historyDir(projectPath, d->currentSession->id());
        const QString dstHist = QSocFileHistory::historyDir(projectPath, newId);
        const QString dstRuns = newPath + QStringLiteral(".agents");

        auto branchLock = lockSession(newPath);
        if (!branchLock || QFileInfo::exists(newPath) || QFileInfo(newPath).isSymLink()
            || QFileInfo::exists(sessionStage) || QFileInfo(sessionStage).isSymLink()
            || QFileInfo::exists(dstHist) || QFileInfo(dstHist).isSymLink()
            || QFileInfo::exists(dstRuns) || QFileInfo(dstRuns).isSymLink()) {
            emitOutput(QStringLiteral("Session branch refused: staging is not private.\n"));
            return true;
        }

        bool branchOk = QFile::copy(d->currentSession->filePath(), sessionStage);
        if (branchOk) {
            QSocSession stagedSession(newId, sessionStage);
            branchOk
                = stagedSession.appendMeta(QStringLiteral("forkedFrom"), d->currentSession->id());
            if (branchOk && !rest.isEmpty()) {
                branchOk = stagedSession.appendMeta(QStringLiteral("title"), rest);
            }
        }
        if (branchOk) {
            branchOk = QSocSubAgentTaskSource::copyRunDirectory(
                d->currentSession->filePath() + QStringLiteral(".agents"), dstRuns);
        }
        if (branchOk) {
            branchOk = QDir().rename(sessionStage, newPath);
        }
        if (!branchOk) {
            QFile::remove(sessionStage);
            if (QFileInfo(dstRuns).isDir() && !QFileInfo(dstRuns).isSymLink()) {
                QDir(dstRuns).removeRecursively();
            }
            emitOutput(QStringLiteral("Session branch failed; no branch was published.\n"));
            return true;
        }
        const QString label = rest.isEmpty() ? newId.left(8) : rest;
        emitOutput(QStringLiteral("(Branched to %1. Resume this branch with: %2)\n")
                       .arg(
                           label,
                           QSocSession(newId, newPath)
                               .resumeCommand(
                                   d->options.clientProgram,
                                   projectPath,
                                   d->options.launchDirectory,
                                   d->remoteConn->target(),
                                   d->remoteConn->workspace())));
        return true;
    }

    if (cmd == QStringLiteral("/diff")) {
        showHistoryDiff();
        return true;
    }

    QSocToolSkillFind finder(nullptr, d->projectManager);
    for (const auto &skill : finder.scanAllSkills()) {
        if (!skill.userInvocable || cmd != "/" + skill.name.toLower())
            continue;
        const QString content = finder.readSkillContent(skill.path);
        if (content.isEmpty()) {
            emitOutput("Could not read skill.\n");
            return true;
        }
        const QString project  = isRemote() ? d->remoteConn->path()->root()
                                            : d->projectManager->getProjectPath();
        bool          consumed = false;
        QString       prompt   = QSocToolSkillFind::substitutePlaceholders(
            content, rest, workingDirectory(), project, &consumed);
        if (!rest.isEmpty() && !consumed)
            prompt += "\n\nArguments passed: " + rest;
        noteInvokedSkill(skill.name);
        d->pendingAutoInputs.append(prompt);
        emitOutput(QStringLiteral("(Running skill %1)\n").arg(skill.name));
        return true;
    }
    return false;
}

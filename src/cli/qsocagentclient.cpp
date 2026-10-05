// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

/* TUI input and rendering for the daemon client. */

#include "agent/client/qsocagentdaemonclient.h"
#include "agent/client/qsocdaemonconnection.h"
#include "cli/qsocagentinputhistory.h"
#include "cli/qsocagenttaskmodel.h"
#include "cli/qsoccliworker.h"
#include "cli/qsocresourceformat.h"
#include "common/qsocinterrupt.h"
#include "common/qsocmessageauthority.h"
#include <QSocketNotifier>

#include "agent/protocol/qsocagentoptions.h"
#include "agent/protocol/qsocagentprotocol.h"
#include "agent/protocol/qsocagentruntimeevent.h"
#include "cli/qagenthistorysearch.h"
#include "cli/qagentinputmonitor.h"
#include "cli/qsocexternaleditor.h"
#include "cli/qsoctranscriptrenderer.h"
#include "cli/qterminalcapability.h"
#include "common/qsocconsole.h"
#include "tui/qtuicompositor.h"
#include "tui/qtuilineinput.h"
#include "tui/qtuimenu.h"
#include "tui/qtuipathpicker.h"
#include "tui/qtuiscrollview.h"
#include "tui/qtuisecretprompt.h"
#include <QDir>
#include <QElapsedTimer>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QTimer>

#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

#include <cmath>

#ifdef Q_OS_WIN
#include <windows.h>
#elif defined(Q_OS_UNIX)
#include <unistd.h>
#endif

using json = nlohmann::json;

/* ---------------------------------------------------------------------- */
/* The TUI client loop. */

bool QSocCliWorker::runAgentClientLoop(
    const QString &requestedSocket, const QSocAgentRuntimeOptions &options)
{
    const bool          singleQuery   = parser.isSet("query");
    const bool          resourcesOnly = parser.isSet("resources");
    QTerminalCapability termCap;
    if (!singleQuery && !resourcesOnly && !termCap.useEnhancedMode())
        return showError(
            1, QStringLiteral("Error: interactive terminal required. Use -q for a single query."));
    if (!singleQuery && !resourcesOnly && (termCap.columns() < 40 || termCap.rows() < 10))
        return showError(1, QStringLiteral("Error: terminal too small (minimum 40x10)."));

    QSocDaemonConnection connection(requestedSocket);
    if (!connection.start())
        return showError(1, connection.error());
    auto      &client            = connection.client();
    QString    resourceWorkspace = options.sshTarget.isEmpty()
                                           && QDir::isAbsolutePath(options.workspace)
                                       ? options.workspace
                                       : options.projectDirectory;
    const auto resourceParams    = [&](bool remote) {
        QJsonArray paths;
        if (!remote && QDir::isAbsolutePath(resourceWorkspace))
            paths.append(QDir::cleanPath(resourceWorkspace));
        return QJsonObject{{"paths", paths}};
    };
    if (resourcesOnly) {
        if (!client.hasCapability("resources"))
            return showError(1, "Resource queries are unsupported by this daemon.");
        const auto reply
            = client.request("resources", resourceParams(!options.sshTarget.isEmpty()), 3500);
        if (reply.contains("error"))
            return showError(1, reply.value("error").toString());
        const auto result = reply.value("result").toObject();
        if (result.value("scope") != "local_daemon" || !result.value("status").isString())
            return showError(1, "Invalid resource response from daemon.");
        QSocConsole::out() << QJsonDocument(result).toJson(QJsonDocument::Indented) << Qt::flush;
        const auto status = result.value("status").toString();
        exitCode          = status == "ok" || status == "partial" || status == "unknown" ? 0 : 1;
        return exitCode == 0;
    }
    QJsonObject params{
        {"project_directory", options.projectDirectory},
        {"launch_directory", options.launchDirectory},
        {"client_program", options.clientProgram},
        {"project_name", options.projectName},
        {"workspace", options.workspace},
        {"ssh_target", options.sshTarget},
        {"single_query", singleQuery},
        {"resume_session_id", options.resumeSessionId},
        {"continue_latest_session", options.continueLatestSession},
        {"max_context_tokens", options.maxContextTokens},
        {"temperature", options.temperature},
        {"effort_level", options.effortLevel},
        {"tool_presentation", options.toolPresentation},
        {"streaming", options.streaming},
        {"streaming_from_config", options.streamingFromConfig},
        {"verbose", options.verbose}};

    QTuiCompositor     compositor;
    auto              &statusBarWidget = compositor.statusBar();
    auto              &inputWidget     = compositor.inputLine();
    QAgentInputMonitor inputMonitor;
    inputMonitor.setAtomicPattern(
        QRegularExpression(QStringLiteral(R"(\[Pasted text #\d+(?: \+\d+ lines)?\])")));
    QEventLoop mainLoop;
    bool       daemonLost        = false;
    bool       closing           = false;
    bool       running           = false;
    bool       planMode          = false;
    bool       remoteWorkspace   = !options.sshTarget.isEmpty();
    qint64     resourceRequestId = 0;
    QTimer     resourceDeadline;
    resourceDeadline.setSingleShot(true);
    QString               resumeHint;
    bool                  queryStreamed = false;
    QSocAgentInputHistory inputHistory;
    QStringList           history;
    QString               ghost;
    QString               searchQuery;
    QString               searchResult;
    bool                  searching = false;
    QAgentHistorySearch   historySearch(history);
    int                   historyPosition = 0;
    QString               draft;
    QStringList           commandNames;
    QString               completionQuery;
    int                   completionStart = -1;
    bool                  fileCompletion  = false;
    QTimer                completionTimer;
    completionTimer.setSingleShot(true);
    completionTimer.setInterval(100);
    const auto hideCompletion = [&] {
        compositor.completionPopup().setVisible(false);
        inputMonitor.setSubmitBlocked(false);
        completionStart = -1;
    };

    QSocTranscriptRenderer renderer(compositor);
    QElapsedTimer          lastInterrupt;
    const auto send = [&](const QString &method, const QJsonObject &values = QJsonObject()) {
        client.send({{"id", client.nextId()}, {"method", method}, {"params", values}});
    };
    const auto consumeQueryInterrupt = [&] {
        const int edges = QSocInterrupt::drainSignalPipe();
        for (int i = 0; i < edges; ++i) {
            send("abort");
            if (lastInterrupt.isValid() && lastInterrupt.elapsed() < 2000)
                client.disconnectFromDaemon();
            lastInterrupt.start();
        }
    };
#ifdef Q_OS_WIN
    QTimer interruptTimer;
    if (singleQuery && QSocInterrupt::handlerReady()) {
        connect(&interruptTimer, &QTimer::timeout, &client, consumeQueryInterrupt);
        interruptTimer.start(20);
    }
#else
    std::unique_ptr<QSocketNotifier> interrupt;
    if (singleQuery && QSocInterrupt::handlerReady() && QSocInterrupt::signalReadFd() >= 0) {
        interrupt
            = std::make_unique<QSocketNotifier>(QSocInterrupt::signalReadFd(), QSocketNotifier::Read);
        connect(interrupt.get(), &QSocketNotifier::activated, &client, consumeQueryInterrupt);
    }
#endif
    connect(&client, &QSocAgentDaemonClient::disconnected, &mainLoop, [&] {
        daemonLost = !closing;
        mainLoop.quit();
    });
    const auto applySnapshot = [&](const QJsonObject &state) {
        if (state.isEmpty())
            return;
        if (state.contains("commands")) {
            commandNames.clear();
            for (const auto &value : state.value("commands").toArray())
                commandNames.append(value.toString());
            if (client.hasCapability("resources") && !commandNames.contains("/resources"))
                commandNames.append("/resources");
        }
        if (state.contains("resume_command"))
            resumeHint = state.value("resume_command").toString();
        const QString model = state.value("model").toString();
        compositor.setTitle(QStringLiteral("QSoC Agent · ") + model);
        statusBarWidget.setModel(model);
        statusBarWidget.setEffortLevel(state.value("effort").toString());
        planMode = state.value("plan_mode").toBool();
        statusBarWidget.setPlanMode(planMode);
        const QString remote = state.value("remote").toString();
        remoteWorkspace      = !remote.isEmpty();
        const auto cwd       = state.value("cwd").toString();
        if (!remoteWorkspace && QDir::isAbsolutePath(cwd))
            resourceWorkspace = cwd;
        statusBarWidget.setRemoteState(remote, !remote.isEmpty());
        if (state.contains("messages")) {
            const auto messages = json::parse(QJsonDocument(state.value("messages").toArray())
                                                  .toJson(QJsonDocument::Compact)
                                                  .toStdString());
            renderer.replaceHistory(messages);
            history.clear();
            for (const auto &message : messages) {
                if (QSocMessageAuthority::isUserRequest(message) && message.contains("content")
                    && message["content"].is_string())
                    history.append(QString::fromStdString(message["content"].get<std::string>()));
            }
            historyPosition = history.size();
        }
        compositor.invalidate();
    };
    const auto handleInteraction = [&](const QSocAgentRuntimeEvent &event) {
        if (!event.json.is_object())
            return;
        const auto data
            = QJsonDocument::fromJson(QByteArray::fromStdString(event.json.dump())).object();
        QJsonObject answer{{"request_id", data.value("request_id")}, {"canceled", true}};
        if (singleQuery) {
            send("answer", answer);
            return;
        }
        inputMonitor.stop();
        const QString type = data.value("type").toString();
        if (type == "secret" || type == "edit") {
            compositor.pause();
            QString text, error;
            if (type == "secret") {
                text = QTuiSecretPrompt::exec(event.text);
                answer.insert("canceled", text.isEmpty());
            } else {
                answer.insert("canceled", !QSocExternalEditor::editText(event.text, text, error));
                answer.insert("error", error);
            }
            answer.insert("text", text);
            compositor.resume();
        } else if (type == "directory") {
            QTuiPathPicker picker;
            picker.setTitle(event.text);
            picker.setStartPath(data.value("start").toString());
            picker.setHomePath(data.value("home").toString());
            QString listError;
            picker.setListDirs([&](const QString &path) {
                const auto response = client.request("list_dirs", {{"path", path}});
                const auto result   = response.value("result").toObject();
                listError = response.value("error").toString(result.value("error").toString());
                QStringList dirs;
                for (const auto &value : result.value("directories").toArray())
                    dirs.append(value.toString());
                return dirs;
            });
            picker.setListError([&] { return listError; });
            const QString selected = picker.exec();
            answer.insert("text", selected);
            answer.insert("canceled", selected.isEmpty());
        } else {
            QTuiMenu menu;
            menu.setTitle(type == "plan" ? QStringLiteral("Approve this plan?") : event.text);
            QList<QTuiMenu::MenuItem> items;
            const auto                rows = data.value("options").toArray();
            for (const auto &row : rows) {
                const auto object = row.toObject();
                items.append(
                    {object.value("label").toString(),
                     object.value("description").toString(),
                     object.value("marked").toBool()});
            }
            if (type == "plan") {
                compositor.printContent(event.text + "\n");
                items = {{"Approve & execute", {}, false}, {"Keep planning", {}, false}};
            } else if (type == "ask")
                items.append({"Other…", "Type an answer", false});
            menu.setItems(items);
            menu.setSearchable(type == "menu");
            const int index = menu.exec();
            answer.insert("index", index);
            answer.insert("canceled", index < 0);
            if (type == "plan")
                answer.insert("approved", index == 0);
            else if (type == "ask" && index >= 0) {
                if (index < rows.size())
                    answer.insert("choice", items[index].label);
                else {
                    compositor.pause();
                    const auto text = QTuiLineInput::exec("Your answer: ");
                    answer.insert("text", text);
                    answer.insert("canceled", text.isEmpty());
                    compositor.resume();
                }
            }
        }
        send("answer", answer);
        inputMonitor.resetEscState();
        inputMonitor.start(!singleQuery);
        compositor.invalidate();
        compositor.render();
    };
    connect(
        &client,
        &QSocAgentDaemonClient::eventReceived,
        &compositor,
        [&](const QSocAgentRuntimeEvent &event) {
            if (event.kind == QSocAgentRuntimeEvent::Kind::AskUser) {
                handleInteraction(event);
                return;
            }
            if (singleQuery) {
                if (event.kind == QSocAgentRuntimeEvent::Kind::ContentChunk
                    || event.kind == QSocAgentRuntimeEvent::Kind::ReasoningChunk) {
                    queryStreamed = true;
                    renderer.apply(event);
                } else if (event.kind == QSocAgentRuntimeEvent::Kind::Output) {
                    QSocConsole::out() << event.text << Qt::flush;
                }
                return;
            }
            renderer.apply(event);
            switch (event.kind) {
            case QSocAgentRuntimeEvent::Kind::ContentChunk:
            case QSocAgentRuntimeEvent::Kind::ReasoningChunk:
            case QSocAgentRuntimeEvent::Kind::ToolOutput:
            case QSocAgentRuntimeEvent::Kind::ToolFinished:
            case QSocAgentRuntimeEvent::Kind::Output:
            case QSocAgentRuntimeEvent::Kind::TaskNotification:
            case QSocAgentRuntimeEvent::Kind::SessionStarted:
            case QSocAgentRuntimeEvent::Kind::SessionCleared:
                compositor.render();
                break;
            case QSocAgentRuntimeEvent::Kind::ToolStarted:
                statusBarWidget.toolCalled(event.secondary, event.text.left(60));
                compositor.render();
                break;
            case QSocAgentRuntimeEvent::Kind::RunComplete:
            case QSocAgentRuntimeEvent::Kind::RunError:
            case QSocAgentRuntimeEvent::Kind::RunAborted:
                statusBarWidget.setStatus(QStringLiteral("Ready"));
                compositor.render();
                break;
            case QSocAgentRuntimeEvent::Kind::UserStatusLine:
                statusBarWidget.setUserLine(event.text);
                compositor.invalidate();
                break;
            case QSocAgentRuntimeEvent::Kind::InputPrediction:
                if (!running && inputMonitor.getInputBuffer().isEmpty()
                    && compositor.currentFocus() == QTuiCompositor::FocusOwner::Input) {
                    ghost = event.text;
                    inputWidget.setGhostText(ghost);
                    compositor.invalidate();
                }
                break;
            case QSocAgentRuntimeEvent::Kind::Retrying:
                statusBarWidget.setStatus(
                    QString("Retrying (%1/%2)").arg(event.attempt).arg(event.maxAttempts));
                compositor.invalidate();
                break;
            case QSocAgentRuntimeEvent::Kind::Stuck:
                statusBarWidget.setStatus(
                    QString("Working [%1s no progress]").arg(event.elapsedSeconds));
                compositor.invalidate();
                break;
            case QSocAgentRuntimeEvent::Kind::QueuedRequest:
                compositor.queuedList().addRequest(event.text);
                compositor.invalidate();
                break;
            case QSocAgentRuntimeEvent::Kind::ProcessingQueued:
                compositor.queuedList().removeRequest(event.text);
                compositor.invalidate();
                break;
            case QSocAgentRuntimeEvent::Kind::Status:
                running = event.text != "Ready";
                if (running)
                    statusBarWidget.startTimers();
                else
                    statusBarWidget.stopTimers();
                statusBarWidget.setStatus(event.text);
                compositor.render();
                break;
            case QSocAgentRuntimeEvent::Kind::Tokens:
                statusBarWidget.updateTokens(event.inputTokens, event.outputTokens);
                compositor.render();
                break;
            case QSocAgentRuntimeEvent::Kind::ContextUsage:
                statusBarWidget
                    .setContextUsage(event.usedTokens, event.maxTokens, event.threshold, event.flag);
                compositor.render();
                break;
            case QSocAgentRuntimeEvent::Kind::SessionResumed:
                if (event.json.is_object() && event.json.contains("messages")
                    && event.json.contains("input") && event.json["input"].is_string())
                    inputMonitor.setInputBuffer(
                        QString::fromStdString(event.json["input"].get<std::string>()));
                compositor.render();
                break;
            case QSocAgentRuntimeEvent::Kind::ModelChanged:
                compositor.setTitle(QStringLiteral("QSoC Agent · ") + event.text);
                statusBarWidget.setModel(event.text);
                compositor.render();
                break;
            case QSocAgentRuntimeEvent::Kind::EffortChanged:
                statusBarWidget.setEffortLevel(event.text);
                compositor.render();
                break;
            case QSocAgentRuntimeEvent::Kind::PlanModeChanged:
                planMode = event.flag;
                statusBarWidget.setPlanMode(event.flag);
                compositor.render();
                break;
            case QSocAgentRuntimeEvent::Kind::WorkingDirChanged:
                if (!remoteWorkspace && QDir::isAbsolutePath(event.text))
                    resourceWorkspace = event.text;
                break;
            case QSocAgentRuntimeEvent::Kind::RemoteChanged:
                remoteWorkspace = !event.text.isEmpty();
                statusBarWidget.setRemoteState(event.text, event.flag);
                compositor.render();
                break;
            default:
                /* Unknown kinds are ignored, not fatal. */
                break;
            }
        });
    if (!singleQuery) {
        inputWidget.setPlaceholder(QStringLiteral("Type a prompt, /command, or !shell"));
        compositor.topBanner().setIntro({"QSoC Agent", "Type 'exit' to exit, '/help' for commands"});
        statusBarWidget.setStatus("Starting");
        compositor.start();
    }
    const auto cleanup = qScopeGuard([&] {
        closing = true;
        inputMonitor.stop();
        compositor.stop();
        client.disconnectFromDaemon();
    });
    const auto fail    = [&](const QString &message) {
        inputMonitor.stop();
        compositor.stop();
        return showError(1, message);
    };
    const auto opened     = client.request("open", params, 0);
    const auto openResult = opened.value("result").toObject();
    if (opened.contains("error") || !openResult.value("ok").toBool())
        return fail(opened.value("error").toString(openResult.value("error").toString()));
    applySnapshot(openResult.value("snapshot").toObject());
    if (singleQuery) {
        // The runtime sends exactly the same events as interactive mode; render
        // once for redirected output and report early/persistence errors too.
        compositor.contentView().clear();
        connect(&inputMonitor, &QAgentInputMonitor::escPressed, &client, [&] { send("abort"); });
        connect(&inputMonitor, &QAgentInputMonitor::ctrlCPressed, &client, [&] { send("abort"); });
        connect(&inputMonitor, &QAgentInputMonitor::inputReady, &client, [&](const QString &text) {
            send("turn", {{"input", text}});
        });
        if (termCap.useEnhancedMode())
            inputMonitor.start(!singleQuery);
        const auto reply  = client.request("turn", {{"input", parser.value("query")}}, 0);
        const auto result = reply.value("result").toObject();
        if (reply.contains("error") || result.value("error").toBool())
            return showError(1, reply.value("error").toString(result.value("error_text").toString()));
        if (result.value("aborted").toBool())
            return showInfo(
                0,
                result.value("stop_notice").toString().isEmpty()
                    ? QStringLiteral("(interrupted)")
                    : result.value("stop_notice").toString());
        if (result.contains("persisted") && !result.value("persisted").toBool())
            return showError(1, "Session persistence failed.");
        if (queryStreamed) {
            compositor.finishStream();
            QString content = compositor.contentView().toAnsi(
                qMax(20, compositor.getTerminalWidth() - 1));
            content.replace(QLatin1Char('\n'), QStringLiteral("\r\n"));
            QSocConsole::out() << content;
            if (!content.endsWith('\n'))
                QSocConsole::out() << "\r\n";
            QSocConsole::out().flush();
            return true;
        }
        return showInfo(0, result.value("final_text").toString());
    }
    if (options.resumeSessionId == "-") {
        const auto reply = client.request("command", {{"input", "/resume"}}, 0);
        applySnapshot(reply.value("result").toObject().value("snapshot").toObject());
    }
    const auto savedHistory = inputHistory.load(options.projectDirectory);
    if (!savedHistory.isEmpty())
        history = savedHistory;
    historyPosition = history.size();
    statusBarWidget.setStatus("Ready");
    connect(&resourceDeadline, &QTimer::timeout, &compositor, [&] {
        compositor.printContent("Resource query timed out. A reply is still pending.\n");
        compositor.render();
    });
    connect(&client, &QSocAgentDaemonClient::replyReceived, &compositor, [&](const QJsonObject &reply) {
        if (resourceRequestId && reply.value("id").toInteger() == resourceRequestId) {
            resourceRequestId = 0;
            resourceDeadline.stop();
            const auto result = reply.value("result").toObject();
            if (reply.contains("error"))
                compositor.printContent("Resources: " + reply.value("error").toString() + "\n");
            else if (result.value("scope") != "local_daemon")
                compositor.printContent("Invalid resource response from daemon.\n");
            else
                compositor.printContent(
                    QSocResourceFormat::summary(result, client.daemonProcessId()));
            compositor.render();
            return;
        }
        if (reply.contains("error"))
            compositor.printContent("Error: " + reply.value("error").toString() + "\n");
        const auto result = reply.value("result").toObject();
        if (result.contains("items") && fileCompletion && completionStart >= 0
            && result.value("query").toString() == completionQuery) {
            QStringList items;
            for (const auto &value : result.value("items").toArray())
                items.append(value.toString());
            auto &popup = compositor.completionPopup();
            popup.setTitle("@file");
            popup.setItems(items);
            popup.setVisible(!items.isEmpty());
            inputMonitor.setSubmitBlocked(!items.isEmpty());
            compositor.invalidate();
            return;
        }
        if (result.value("error").isBool() && result.value("error").toBool())
            compositor.printContent("Error: " + result.value("error_text").toString() + "\n");
        if (result.contains("handled") && !result.value("handled").toBool())
            compositor.printContent("Unknown command. Type /help for commands.\n");
        if (result.contains("persisted") && !result.value("persisted").toBool())
            compositor.printContent("Session persistence failed.\n");
        applySnapshot(result.value("snapshot").toObject());
        if (reply.contains("error") || result.contains("handled") || result.contains("final_text")) {
            running = false;
            statusBarWidget.setStatus("Ready");
            statusBarWidget.stopTimers();
        }
        compositor.render();
    });
    connect(&inputMonitor, &QAgentInputMonitor::inputChanged, &compositor, [&](const QString &text) {
        if (searching) {
            searchQuery = text;
            historySearch.rewind();
            const auto match = historySearch.findNext(searchQuery);
            searchResult     = match.text;
            inputWidget.setSearchMode(true, searchQuery, match.text, match.index < 0);
            compositor.invalidate();
            return;
        }
        ghost.clear();
        inputWidget.setGhostText({});
        inputWidget.setText(text);
        inputWidget.setCursorPos(inputMonitor.getCursorPos());
        completionTimer.stop();
        const QString prefix = text.left(inputMonitor.getCursorPos());
        if (prefix.startsWith('/') && !prefix.contains(QRegularExpression("\\s"))) {
            fileCompletion  = false;
            completionStart = 0;
            QStringList matches;
            for (const auto &name : commandNames)
                if (name.startsWith(prefix, Qt::CaseInsensitive))
                    matches.append(name);
            if (matches.size() == 1 && matches.first() == prefix)
                matches.clear();
            auto &popup = compositor.completionPopup();
            popup.setTitle("Commands");
            popup.setItems(matches);
            popup.setVisible(!matches.isEmpty());
            inputMonitor.setSubmitBlocked(!matches.isEmpty());
        } else {
            static const QRegularExpression reference(QStringLiteral(R"((?:^|\s)@([^\s]*)$)"));
            const auto                      match = reference.match(prefix);
            hideCompletion();
            if (match.hasMatch()) {
                fileCompletion  = true;
                completionStart = match.capturedStart(1) - 1;
                completionQuery = match.captured(1);
                completionTimer.start();
            }
        }
        compositor.invalidate();
    });
    connect(&completionTimer, &QTimer::timeout, &client, [&] {
        send("complete", {{"query", completionQuery}});
    });
    connect(&inputMonitor, &QAgentInputMonitor::submitBlockedKey, &inputMonitor, [&](int) {
        if (searching) {
            searching = false;
            inputMonitor.setSubmitBlocked(false);
            inputWidget.setSearchMode(false, {}, {}, false);
            inputMonitor.setInputBuffer(searchResult);
            return;
        }
        const auto &popup = compositor.completionPopup();
        if (!popup.isVisible() || completionStart < 0 || popup.getItems().isEmpty())
            return;
        const QString chosen      = popup.getItems().value(popup.getHighlight());
        const auto    buffer      = inputMonitor.getInputBuffer();
        const QString replacement = (fileCompletion ? "@" : "") + chosen + " ";
        const QString completed   = buffer.left(completionStart) + replacement
                                    + buffer.mid(inputMonitor.getCursorPos());
        hideCompletion();
        inputMonitor.setInputBuffer(completed);
    });
    connect(
        &inputMonitor, &QAgentInputMonitor::pastedReceived, &inputMonitor, [&](const QString &text) {
            inputMonitor.insertText(inputHistory.paste(text));
        });
    connect(&inputMonitor, &QAgentInputMonitor::inputReady, &inputMonitor, [&](const QString &text) {
        if (searching) {
            searching = false;
            inputMonitor.setSubmitBlocked(false);
            inputWidget.setSearchMode(false, {}, {}, false);
            inputMonitor.setInputBuffer(searchResult);
            return;
        }
        const QString trimmed = text.trimmed();
        if (trimmed.isEmpty())
            return;
        if (trimmed == "/exit" || trimmed == "/quit" || trimmed == "exit" || trimmed == "quit") {
            closing = true;
            mainLoop.quit();
            return;
        }
        static const QRegularExpression resourceCommand(QStringLiteral(R"(^/resources(?:\s|$))"));
        if (resourceCommand.match(trimmed).hasMatch()) {
            hideCompletion();
            compositor.dismissTopBanner();
            if (trimmed != "/resources")
                compositor.printContent("Usage: /resources\n");
            else if (!client.hasCapability("resources"))
                compositor.printContent("Resource queries are unsupported by this daemon.\n");
            else if (resourceRequestId)
                compositor.printContent("A resource query is already pending.\n");
            else {
                resourceRequestId = client.nextId();
                resourceDeadline.start(3500);
                client.send(
                    {{"id", resourceRequestId},
                     {"method", "resources"},
                     {"params", resourceParams(remoteWorkspace)}});
            }
            compositor.render();
            return;
        }
        if (history.isEmpty() || history.last() != text)
            history.append(text);
        inputHistory.append(
            text,
            options.projectDirectory,
            options.sshTarget.isEmpty() ? "local" : options.sshTarget);
        historyPosition = history.size();
        compositor.dismissTopBanner();
        compositor.appendUserMessage(text);
        const bool command = trimmed.startsWith('/') || trimmed.startsWith('!')
                             || trimmed.startsWith('#');
        statusBarWidget.setStatus(running ? "Queued" : "Reasoning");
        statusBarWidget.startTimers();
        send(command ? "command" : "turn", {{"input", inputHistory.expand(text)}});
        running = true;
        compositor.render();
    });
    connect(&inputMonitor, &QAgentInputMonitor::escPressed, &inputMonitor, [&] {
        if (searching) {
            searching = false;
            inputMonitor.setSubmitBlocked(false);
            inputWidget.setSearchMode(false, {}, {}, false);
            inputMonitor.setInputBuffer(draft);
        } else if (compositor.taskOverlay().mode() != QTuiTaskOverlay::Mode::Hidden)
            compositor.taskOverlay().handleKey(Qt::Key_Escape, false);
        else if (compositor.completionPopup().isVisible()) {
            hideCompletion();
            compositor.invalidate();
        } else
            send("abort");
    });
    connect(&inputMonitor, &QAgentInputMonitor::escEscPressed, &inputMonitor, [&] {
        if (!running && inputMonitor.getInputBuffer().isEmpty())
            send("command", {{"input", "/rewind"}});
    });
    connect(&inputMonitor, &QAgentInputMonitor::ctrlCPressed, &inputMonitor, [&](bool hadInput) {
        if (lastInterrupt.isValid() && lastInterrupt.elapsed() < 2000) {
            closing = true;
            mainLoop.quit();
            return;
        }
        lastInterrupt.start();
        if (running)
            send("abort");
        else if (!hadInput) {
            closing = true;
            mainLoop.quit();
        }
    });
    connect(&inputMonitor, &QAgentInputMonitor::arrowKey, &inputMonitor, [&](int key) {
        if (compositor.taskOverlay().mode() != QTuiTaskOverlay::Mode::Hidden) {
            compositor.taskOverlay().handleKey(
                key == 'A'   ? Qt::Key_Up
                : key == 'B' ? Qt::Key_Down
                : key == 'C' ? Qt::Key_Right
                             : Qt::Key_Left,
                false);
            return;
        }
        if (key != 'A' && key != 'B')
            return;
        if (compositor.completionPopup().isVisible()) {
            compositor.completionPopup().moveHighlight(key == 'A' ? -1 : 1);
            compositor.invalidate();
            return;
        }
        if (historyPosition == history.size())
            draft = inputMonitor.getInputBuffer();
        historyPosition = qBound(0, historyPosition + (key == 'A' ? -1 : 1), int(history.size()));
        inputMonitor.setInputBuffer(
            historyPosition == history.size() ? draft : history[historyPosition]);
    });
    connect(&inputMonitor, &QAgentInputMonitor::historySearchRequested, &inputMonitor, [&] {
        if (!searching) {
            draft       = inputMonitor.getInputBuffer();
            searchQuery = draft;
            historySearch.reset();
            hideCompletion();
            searching = true;
            inputMonitor.setSubmitBlocked(true);
        }
        const auto match = historySearch.findNext(searchQuery);
        searchResult     = match.text;
        inputWidget.setSearchMode(true, searchQuery, match.text, match.index < 0);
        compositor.invalidate();
    });
    connect(&inputMonitor, &QAgentInputMonitor::externalEditorRequested, &inputMonitor, [&] {
        inputMonitor.stop();
        compositor.pause();
        QString    result, error;
        const bool ok = QSocExternalEditor::editText(
            inputHistory.expand(inputMonitor.getInputBuffer()), result, error);
        compositor.resume();
        inputMonitor.start(!singleQuery);
        if (ok)
            inputMonitor.setInputBuffer(result);
        else
            compositor.printContent(error + "\n");
    });
    connect(&inputMonitor, &QAgentInputMonitor::mouseWheel, &compositor, [&](int direction) {
        if (direction == 0)
            compositor.scrollContentUp();
        else
            compositor.scrollContentDown();
    });
    connect(&inputMonitor, &QAgentInputMonitor::pageKey, &compositor, [&](int direction) {
        if (direction == 0)
            compositor.scrollContentUp(10);
        else
            compositor.scrollContentDown(10);
    });
    connect(
        &inputMonitor,
        &QAgentInputMonitor::mouseClick,
        &compositor,
        [&](int button, int col, int row, bool pressed) {
            if (button != 0)
                return;
            if (pressed)
                compositor.selectionStart(col - 1, row - 1);
            else
                compositor.selectionFinish(col - 1, row - 1);
        });
    connect(&inputMonitor, &QAgentInputMonitor::mouseDrag, &compositor, [&](int col, int row) {
        compositor.selectionUpdate(col - 1, row - 1);
    });
    connect(&inputMonitor, &QAgentInputMonitor::copyFocusedBlockRequested, &compositor, [&] {
        compositor.copyFocusedBlock();
    });
    connect(&inputMonitor, &QAgentInputMonitor::redrawRequested, &compositor, [&] {
        compositor.invalidate();
        compositor.render();
    });
    connect(&inputMonitor, &QAgentInputMonitor::toggleTodosRequested, &compositor, [&] {
        auto &todos = compositor.todoList();
        todos.setVisible(!todos.isVisible());
        compositor.invalidate();
    });
    connect(&inputMonitor, &QAgentInputMonitor::planModeToggleRequested, &compositor, [&] {
        send("plan_mode", {{"enabled", !planMode}});
    });
    connect(&inputMonitor, &QAgentInputMonitor::terminalFocusChanged, &compositor, [&](bool focused) {
        statusBarWidget.setUserWatching(focused);
        send("watching", {{"enabled", focused}});
    });
    QSocAgentTaskModel tasks([&](const QString &method, const QJsonObject &values) {
        return client.request(method, values, 2000);
    });
    compositor.taskOverlay().setRegistry(&tasks);
    const auto detachTasks = qScopeGuard([&] { compositor.taskOverlay().setRegistry(nullptr); });
    QTimer     taskTimer;
    connect(&taskTimer, &QTimer::timeout, &compositor, [&] {
        tasks.refresh();
        statusBarWidget.setTaskCount(tasks.activeCount());
    });
    taskTimer.start(1000);
    connect(&inputMonitor, &QAgentInputMonitor::taskOverlayRequested, &compositor, [&] {
        auto &overlay = compositor.taskOverlay();
        if (overlay.mode() != QTuiTaskOverlay::Mode::Hidden) {
            overlay.close();
            return;
        }
        tasks.refresh();
        overlay.open();
    });
    inputMonitor.setExternalKeyConsumer([&](unsigned char key) {
        if (compositor.taskOverlay().mode() != QTuiTaskOverlay::Mode::Hidden) {
            compositor.taskOverlay().handleKey(
                key == '\r' || key == '\n' ? Qt::Key_Return
                                           : QChar::fromLatin1(char(key)).toUpper().unicode(),
                false);
            return true;
        }
        if ((key == '\r' || key == '\n') && inputMonitor.getInputBuffer().isEmpty()
            && !ghost.isEmpty() && !searching) {
            QTimer::singleShot(0, &inputMonitor, [&, accepted = ghost] {
                inputMonitor.insertText(accepted);
                inputMonitor.submitNow();
            });
            return true;
        }
        return false;
    });
    connect(&inputMonitor, &QAgentInputMonitor::tabOnEmptyBuffer, &inputMonitor, [&] {
        if (!ghost.isEmpty())
            inputMonitor.insertText(ghost);
    });
    inputMonitor.start(!singleQuery);
    if (!inputMonitor.isActive())
        return fail("Terminal input setup failed.");
    compositor.render();
    if (client.isConnected())
        mainLoop.exec();
    if (daemonLost)
        return fail("The daemon connection closed.");
    inputMonitor.stop();
    compositor.stop();
    if (!resumeHint.isEmpty())
        QSocConsole::out() << "\nResume this session with:\n" << resumeHint << Qt::endl;
    return true;
}

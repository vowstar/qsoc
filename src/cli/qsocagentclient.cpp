// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

/**
 * @file qsocagentclient.cpp
 * @brief TUI frontend that drives a qsoc-agentd daemon.
 * @details The pure-frontend half of the decoupled architecture: this file
 *          renders the daemon's event stream with the same compositor the
 *          in-process REPL uses, and forwards user input as protocol
 *          requests. Agent infrastructure is hosted by the daemon.
 */

#include "cli/qsocagentclient.h"
#include "agent/qsocmessageauthority.h"
#include "agent/remote/qsocinterrupt.h"
#include "cli/qsocagentinputhistory.h"
#include "cli/qsocagenttaskmodel.h"
#include "cli/qsoccliworker.h"
#include "tui/qtuiimagepreviewblock.h"
#include <QSocketNotifier>

#include "agent/runtime/qsocagentprotocol.h"
#include "agent/runtime/qsocagentruntime.h"
#include "agent/runtime/qsocagentruntimeevent.h"
#include "cli/qagenthistorysearch.h"
#include "cli/qagentinputmonitor.h"
#include "cli/qsocexternaleditor.h"
#include "cli/qsocsessiontranscript.h"
#include "cli/qterminalcapability.h"
#include "common/qsocconsole.h"
#include "common/qsoclinediff.h"
#include "tui/qtuicompositor.h"
#include "tui/qtuidiffblock.h"
#include "tui/qtuilineinput.h"
#include "tui/qtuimenu.h"
#include "tui/qtuipathpicker.h"
#include "tui/qtuiscrollview.h"
#include "tui/qtuisecretprompt.h"
#include "tui/qtuitodoblock.h"
#include <QDir>
#include <QElapsedTimer>
#include <QProcess>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTimer>

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QStringList>

#include <cmath>

using json = nlohmann::json;

namespace {

constexpr int kHeaderBytes = QSocAgentProtocol::headerBytes;
using QSocAgentProtocol::frame;

QList<QTuiTodoList::TodoItem> parseTodoListResult(const QString &result)
{
    QList<QTuiTodoList::TodoItem> items;

    /* Match pattern: [x] or [ ] followed by ID. Title (priority) */
    QRegularExpression regex(R"(\[([ x])\]\s*(\d+)\.\s*(.+?)\s*\((\w+)\))");

    const QStringList lines = result.split('\n');
    for (const QString &line : lines) {
        QRegularExpressionMatch match = regex.match(line);
        if (match.hasMatch()) {
            QTuiTodoList::TodoItem item;
            item.status   = (match.captured(1) == "x") ? "done" : "pending";
            item.id       = match.captured(2).toInt();
            item.title    = match.captured(3).trimmed();
            item.priority = match.captured(4);
            items.append(item);
        }
    }

    return items;
}

/**
 * @brief Parse todo_add result into a single TodoItem
 * @param result The result string from todo_add tool
 *        Format: "Added todo #37: Title here (priority)"
 * @return TodoItem if parsed successfully, empty item if not
 */
QTuiTodoList::TodoItem parseTodoAddResult(const QString &result)
{
    QTuiTodoList::TodoItem item;
    item.id = -1; /* Invalid by default */

    /* Match: "Added todo #ID: Title (priority)" */
    QRegularExpression      regex(R"(Added todo #(\d+):\s*(.+?)\s*\((\w+)(?:\s+priority)?\))");
    QRegularExpressionMatch match = regex.match(result);

    if (match.hasMatch()) {
        item.id       = match.captured(1).toInt();
        item.title    = match.captured(2).trimmed();
        item.priority = match.captured(3);
        item.status   = "pending";
    }

    return item;
}

/**
 * @brief Parse todo_update result to extract ID and new status
 * @param result The result string from todo_update tool
 *        Format: "Updated todo #37 status to: done"
 * @return Pair of (todoId, newStatus), todoId=-1 if parse failed
 */
QPair<int, QString> parseTodoUpdateResult(const QString &result)
{
    /* Match: "Updated todo #ID: Title (status: STATUS)" */
    QRegularExpression      regex(R"(Updated todo #(\d+):\s*.+?\(status:\s*(\w+)\))");
    QRegularExpressionMatch match = regex.match(result);

    if (match.hasMatch()) {
        return qMakePair(match.captured(1).toInt(), match.captured(2));
    }

    return qMakePair(-1, QString());
}

/**
 * @brief Run a shell command and return its combined stdout+stderr output.
 * @details Uses popen() so the output can be captured and displayed in the
 *          scrollView after the compositor resumes the alt-screen. Without
 *          capture, any output written to stdout during compositor.pause()
 *          would be erased the moment resume() switches back to the
 *          alt-screen buffer.
 */

QSocAgentRuntimeEvent eventFromJson(const QJsonObject &value)
{
    return QSocAgentRuntimeEvent::fromJson(
        json::parse(QJsonDocument(value).toJson(QJsonDocument::Compact).toStdString()));
}

QTuiScrollView::LineStyle styleFor(QSocAgentRuntimeStyle style)
{
    switch (style) {
    case QSocAgentRuntimeStyle::Dim:
        return QTuiScrollView::Dim;
    case QSocAgentRuntimeStyle::Bold:
        return QTuiScrollView::Bold;
    case QSocAgentRuntimeStyle::Warning:
        return QTuiScrollView::DiffHunk;
    case QSocAgentRuntimeStyle::Normal:
        break;
    }
    return QTuiScrollView::Normal;
}

} // namespace

QSocAgentDaemonClient::QSocAgentDaemonClient(const QString &socketPath, QObject *parent)
    : QObject(parent)
    , m_socketPath(socketPath)
{
    connect(&m_socket, &QLocalSocket::readyRead, this, &QSocAgentDaemonClient::onReadyRead);
    connect(&m_socket, &QLocalSocket::disconnected, this, [this]() { emit disconnected(); });
}

QSocAgentDaemonClient::~QSocAgentDaemonClient() = default;

bool QSocAgentDaemonClient::connectToDaemon(int timeoutMs)
{
    m_buffer.clear();
    m_greeting = {};
    m_socket.abort();
    m_socket.connectToServer(m_socketPath);
    if (!m_socket.waitForConnected(timeoutMs)) {
        m_error = m_socket.errorString();
        return false;
    }
    QEventLoop loop;
    QTimer     timer;
    timer.setSingleShot(true);
    connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    auto received = connect(this, &QSocAgentDaemonClient::replyReceived, &loop, [&] {
        if (!m_greeting.isEmpty())
            loop.quit();
    });
    auto lost     = connect(this, &QSocAgentDaemonClient::disconnected, &loop, &QEventLoop::quit);
    timer.start(timeoutMs);
    if (m_greeting.isEmpty() && isConnected())
        loop.exec();
    disconnect(received);
    disconnect(lost);
    if (m_greeting.value("protocol").toInt() != QSocAgentProtocol::version) {
        m_error = QStringLiteral("missing or incompatible daemon protocol greeting");
        m_socket.abort();
        return false;
    }
    m_daemonVersion = m_greeting.value("version").toString();
    return true;
}

QJsonObject QSocAgentDaemonClient::request(
    const QString &method, const QJsonObject &params, int timeoutMs)
{
    const auto id = nextId();
    QEventLoop loop;
    QTimer     timer;
    timer.setSingleShot(true);
    QJsonObject reply;
    auto        received
        = connect(this, &QSocAgentDaemonClient::replyReceived, &loop, [&](const QJsonObject &value) {
              if (value.value("id").toInteger() == id) {
                  reply = value;
                  loop.quit();
              }
          });
    auto lost = connect(this, &QSocAgentDaemonClient::disconnected, &loop, &QEventLoop::quit);
    connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    if (timeoutMs > 0)
        timer.start(timeoutMs);
    send({{"id", id}, {"method", method}, {"params", params}});
    if (isConnected())
        loop.exec();
    disconnect(received);
    disconnect(lost);
    if (reply.isEmpty())
        reply.insert(
            "error",
            isConnected() ? QStringLiteral("daemon request timed out")
                          : QStringLiteral("daemon disconnected"));
    return reply;
}

void QSocAgentDaemonClient::send(const QJsonObject &request)
{
    m_socket.write(frame(request));
    m_socket.flush();
}

qint64 QSocAgentDaemonClient::nextId()
{
    return ++m_requestCounter;
}

void QSocAgentDaemonClient::onReadyRead()
{
    m_buffer += m_socket.readAll();
    while (true) {
        if (m_buffer.size() < kHeaderBytes) {
            return;
        }
        const int length = QSocAgentProtocol::payloadLength(m_buffer);
        if (length < 0) {
            m_socket.disconnectFromServer();
            return;
        }
        if (m_buffer.size() < kHeaderBytes + length) {
            return;
        }
        const QByteArray payload = m_buffer.mid(kHeaderBytes, length);
        m_buffer.remove(0, kHeaderBytes + length);
        const QJsonDocument doc = QJsonDocument::fromJson(payload);
        if (!doc.isObject()) {
            continue;
        }
        const QJsonObject frameObject = doc.object();
        if (frameObject.contains(QStringLiteral("event"))) {
            const auto event = eventFromJson(frameObject.value(QStringLiteral("event")).toObject());
            QTimer::singleShot(0, this, [this, event] { emit eventReceived(event); });
        } else {
            if (frameObject.contains("daemon"))
                m_greeting = frameObject;
            QTimer::singleShot(0, this, [this, frameObject] { emit replyReceived(frameObject); });
        }
    }
}

/* ---------------------------------------------------------------------- */
/* The TUI client loop. */

bool QSocCliWorker::runAgentClientLoop(
    const QString &requestedSocket, const QSocAgentRuntimeOptions &options)
{
    const bool          singleQuery = parser.isSet("query");
    QTerminalCapability termCap;
    if (!singleQuery && !termCap.useEnhancedMode())
        return showError(
            1, QStringLiteral("Error: interactive terminal required. Use -q for a single query."));
    if (!singleQuery && (termCap.columns() < 40 || termCap.rows() < 10))
        return showError(1, QStringLiteral("Error: terminal too small (minimum 40x10)."));

    // A private endpoint distinguishes an owned child from an attached daemon.
    // QProcess uses fork/exec (or the platform equivalent), so the child starts
    // with a clean Qt event loop and never shares a terminal with the TUI.
    QTemporaryDir privateDirectory;
    QProcess      child;
    QString       socketPath = requestedSocket;
    const bool    owned      = socketPath.isEmpty();
    const auto    stopChild  = qScopeGuard([&] {
        if (owned && child.state() != QProcess::NotRunning) {
            child.terminate();
            if (!child.waitForFinished(3000)) {
                child.kill();
                child.waitForFinished(1000);
            }
        }
    });
    if (owned) {
        if (!privateDirectory.isValid())
            return showError(1, "Could not create daemon socket directory.");
        socketPath = privateDirectory.filePath("agent.sock");
        child.setStandardInputFile(QProcess::nullDevice());
        child.setStandardOutputFile(QProcess::nullDevice());
        child.setStandardErrorFile(privateDirectory.filePath("daemon.log"));
        child.start(
            QCoreApplication::applicationDirPath() + "/qsoc-agentd",
            {"--socket",
             socketPath,
             "--parent-pid",
             QString::number(QCoreApplication::applicationPid())});
        if (!child.waitForStarted(5000))
            return showError(1, child.errorString());
    }
    QSocAgentDaemonClient client(socketPath);
    QDeadlineTimer        startup(5000);
    while (!client.connectToDaemon(owned ? 100 : 5000)) {
        if (!owned || startup.hasExpired() || child.state() == QProcess::NotRunning)
            return showError(
                1, QStringLiteral("Could not connect to agent daemon: %1").arg(client.error()));
        QEventLoop wait;
        QTimer::singleShot(20, &wait, &QEventLoop::quit);
        wait.exec();
    }
    QJsonObject params{
        {"project_directory", options.projectDirectory},
        {"launch_directory", options.launchDirectory},
        {"client_program", options.clientProgram},
        {"project_name", options.projectName},
        {"workspace", options.workspace},
        {"ssh_target", options.sshTarget},
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
    QEventLoop            mainLoop;
    bool                  daemonLost = false;
    bool                  closing    = false;
    bool                  running    = false;
    bool                  planMode   = false;
    QString               resumeHint;
    bool                  streamedContent = false;
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

    QHash<QString, QJsonObject> pendingTools;
    QElapsedTimer               lastInterrupt;
    const auto send = [&](const QString &method, const QJsonObject &values = QJsonObject()) {
        client.send({{"id", client.nextId()}, {"method", method}, {"params", values}});
    };
    std::unique_ptr<QSocketNotifier> interrupt;
    if (QSocInterrupt::handlerReady()) {
        interrupt
            = std::make_unique<QSocketNotifier>(QSocInterrupt::signalReadFd(), QSocketNotifier::Read);
        connect(interrupt.get(), &QSocketNotifier::activated, &client, [&] {
            const int edges = QSocInterrupt::drainSignalPipe();
            if (edges <= 0)
                return;
            if (!singleQuery && inputMonitor.isActive()) {
                emit inputMonitor.ctrlCPressed();
            } else {
                send("abort");
                if (lastInterrupt.isValid() && lastInterrupt.elapsed() < 2000)
                    client.disconnectFromDaemon();
                lastInterrupt.start();
            }
        });
    }
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
        statusBarWidget.setRemoteState(remote, !remote.isEmpty());
        if (state.contains("messages")) {
            compositor.contentView().clear();
            const auto messages = json::parse(QJsonDocument(state.value("messages").toArray())
                                                  .toJson(QJsonDocument::Compact)
                                                  .toStdString());
            QSocSessionTranscript::appendTo(messages, compositor.contentView());
            history.clear();
            for (const auto &message : messages) {
                if (message.value("role", std::string()) == "user" && message.contains("content")
                    && message["content"].is_string()
                    && !QSocMessageAuthority::isRuntimeReminder(message))
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
        inputMonitor.start();
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
                if (event.kind == QSocAgentRuntimeEvent::Kind::ContentChunk) {
                    streamedContent = true;
                    compositor.appendAssistantChunk(event.text);
                } else if (event.kind == QSocAgentRuntimeEvent::Kind::ReasoningChunk) {
                    streamedContent = true;
                    compositor.appendReasoningChunk(event.text);
                } else if (event.kind == QSocAgentRuntimeEvent::Kind::Output) {
                    QSocConsole::out() << event.text << Qt::flush;
                }
                return;
            }
            switch (event.kind) {
            case QSocAgentRuntimeEvent::Kind::ContentChunk:
                streamedContent = true;
                compositor.appendAssistantChunk(event.text);
                compositor.render();
                break;
            case QSocAgentRuntimeEvent::Kind::ReasoningChunk:
                compositor.appendReasoningChunk(event.text);
                compositor.render();
                break;
            case QSocAgentRuntimeEvent::Kind::ToolStarted:
                pendingTools
                    .insert(event.callId, QJsonDocument::fromJson(event.text.toUtf8()).object());
                statusBarWidget.toolCalled(event.secondary, event.text.left(60));
                compositor.beginToolUse(event.secondary, event.text.left(60), event.callId);
                compositor.render();
                break;
            case QSocAgentRuntimeEvent::Kind::ToolOutput:
                compositor.appendToolUseBody(event.text, event.callId);
                compositor.render();
                break;
            case QSocAgentRuntimeEvent::Kind::ToolFinished: {
                const auto    args = pendingTools.take(event.callId);
                const QString name = event.secondary;
                if (name == "todo_list") {
                    const auto items = parseTodoListResult(event.text);
                    compositor.todoList().setItems(items);
                    compositor.contentView().appendBlock(std::make_unique<QTuiTodoBlock>(items));
                } else if (name == "todo_add") {
                    const auto item = parseTodoAddResult(event.text);
                    if (item.id >= 0)
                        compositor.todoList().addItem(item);
                } else if (name == "todo_update" || name == "todo_delete") {
                    const auto [id, status] = parseTodoUpdateResult(event.text);
                    if (id >= 0) {
                        if (name == "todo_delete")
                            compositor.todoList().removeItem(id);
                        else
                            compositor.todoList().updateStatus(id, status);
                    }
                }
                if (event.ok && (name == "edit_file" || name == "write_file")) {
                    const QString path  = args.value("file_path").toString();
                    const auto    lines = QSocLineDiff::computeLineDiff(
                        args.value("old_string").toString(),
                        args.value(name == "edit_file" ? "new_string" : "content").toString());
                    auto block = std::make_unique<QTuiDiffBlock>("--- a/" + path, "+++ b/" + path);
                    for (const auto &line : lines) {
                        const auto kind = line.kind == QSocLineDiff::Kind::Add
                                              ? QTuiDiffBlock::Kind::Add
                                          : line.kind == QSocLineDiff::Kind::Del
                                              ? QTuiDiffBlock::Kind::Del
                                          : line.kind == QSocLineDiff::Kind::Hunk
                                              ? QTuiDiffBlock::Kind::Hunk
                                              : QTuiDiffBlock::Kind::Context;
                        block->addRow(kind, line.text);
                    }
                    compositor.contentView().appendBlock(std::move(block));
                }
                compositor.replaceToolUseBody(event.text, event.callId);
                compositor.finishToolUse(
                    event.ok ? QTuiToolBlock::Status::Success : QTuiToolBlock::Status::Failure,
                    {},
                    event.callId);
                compositor.render();
                break;
            }
            case QSocAgentRuntimeEvent::Kind::RunComplete:
                if (!streamedContent && !event.text.isEmpty())
                    compositor.appendAssistantChunk(event.text);
                streamedContent = false;
                compositor.finishStream();
                compositor.resetExecution();
                compositor.printContent(QStringLiteral("\n"));
                statusBarWidget.setStatus(QStringLiteral("Ready"));
                compositor.render();
                break;
            case QSocAgentRuntimeEvent::Kind::RunError:
                streamedContent = false;
                compositor.finishStream();
                compositor.resetExecution();
                compositor.printContent(QStringLiteral("\nError: %1\n").arg(event.text));
                statusBarWidget.setStatus(QStringLiteral("Ready"));
                compositor.render();
                break;
            case QSocAgentRuntimeEvent::Kind::RunAborted:
                compositor.resetExecution();
                compositor.printContent(QStringLiteral("\n%1\n").arg(
                    event.text.isEmpty() ? QStringLiteral("(interrupted)") : event.text));
                statusBarWidget.setStatus(QStringLiteral("Ready"));
                compositor.render();
                break;
            case QSocAgentRuntimeEvent::Kind::ImagePreview:
                if (event.json.is_object()) {
                    compositor.contentView().appendBlock(
                        std::make_unique<QTuiImagePreviewBlock>(
                            event.text,
                            QString::fromStdString(event.json.value("mime", std::string())),
                            event.json.value("width", 0),
                            event.json.value("height", 0),
                            QByteArray::fromBase64(
                                QByteArray::fromStdString(
                                    event.json.value("data", std::string())))));
                    compositor.invalidate();
                }
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
            case QSocAgentRuntimeEvent::Kind::Diff: {
                const QString before = QString::fromStdString(
                    event.json.value("before", std::string()));
                const QString after = QString::fromStdString(
                    event.json.value("after", std::string()));
                auto block
                    = std::make_unique<QTuiDiffBlock>("--- a/" + event.text, "+++ b/" + event.text);
                for (const auto &line : QSocLineDiff::computeLineDiff(before, after)) {
                    const auto kind = line.kind == QSocLineDiff::Kind::Add
                                          ? QTuiDiffBlock::Kind::Add
                                      : line.kind == QSocLineDiff::Kind::Del
                                          ? QTuiDiffBlock::Kind::Del
                                      : line.kind == QSocLineDiff::Kind::Hunk
                                          ? QTuiDiffBlock::Kind::Hunk
                                          : QTuiDiffBlock::Kind::Context;
                    block->addRow(kind, line.text);
                }
                compositor.contentView().appendBlock(std::move(block));
                compositor.invalidate();
                break;
            }
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
            case QSocAgentRuntimeEvent::Kind::Output:
                compositor.printContent(event.text, styleFor(event.style));
                compositor.render();
                break;
            case QSocAgentRuntimeEvent::Kind::Tokens:
                statusBarWidget.updateTokens(event.inputTokens, event.outputTokens);
                compositor.render();
                break;
            case QSocAgentRuntimeEvent::Kind::ContextUsage:
                statusBarWidget.setContextUsage(event.usedTokens, event.maxTokens, event.threshold);
                compositor.render();
                break;
            case QSocAgentRuntimeEvent::Kind::SessionResumed:
                if (event.json.is_object() && event.json.contains("messages")) {
                    compositor.contentView().clear();
                    QSocSessionTranscript::appendTo(event.json["messages"], compositor.contentView());
                    if (event.json.contains("input") && event.json["input"].is_string())
                        inputMonitor.setInputBuffer(
                            QString::fromStdString(event.json["input"].get<std::string>()));
                }
                [[fallthrough]];
            case QSocAgentRuntimeEvent::Kind::SessionStarted:
            case QSocAgentRuntimeEvent::Kind::SessionCleared:
                compositor.printContent(event.text);
                compositor.render();
                break;
            case QSocAgentRuntimeEvent::Kind::TaskNotification:
                compositor.printContent(
                    QStringLiteral("(task: %1)\n").arg(event.text.left(80)), QTuiScrollView::Dim);
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
            case QSocAgentRuntimeEvent::Kind::RemoteChanged:
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
            inputMonitor.start();
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
        if (streamedContent) {
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
    connect(&client, &QSocAgentDaemonClient::replyReceived, &compositor, [&](const QJsonObject &reply) {
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
        auto &popup = compositor.completionPopup();
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
    connect(&inputMonitor, &QAgentInputMonitor::ctrlCPressed, &inputMonitor, [&] {
        if (lastInterrupt.isValid() && lastInterrupt.elapsed() < 2000) {
            closing = true;
            mainLoop.quit();
            return;
        }
        lastInterrupt.start();
        if (running)
            send("abort");
        else if (!inputMonitor.getInputBuffer().isEmpty())
            inputMonitor.setInputBuffer({});
        else {
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
        inputMonitor.start();
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
            const auto accepted = ghost;
            QTimer::singleShot(0, &inputMonitor, [&, accepted] {
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
    inputMonitor.start();
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

#include "moc_qsocagentclient.cpp"

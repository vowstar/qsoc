// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/daemon/qsocagentdaemon.h"
#include "agent/daemon/qsocdaemonresources.h"

#include "agent/protocol/qsocagentprotocol.h"
#include "agent/protocol/qsocagentruntimeevent.h"
#include "agent/remote/qsocagentremote.h"
#include "agent/services/qagentcompletion.h"
#include "agent/tool/qsoctoolaskuser.h"
#include "agent/tool/qsoctoolplanmode.h"
#include "common/config.h"
#include "common/qsocipc.h"
#include "common/qsoclocalendpoint.h"
#include "common/qsoclocalpeer.h"
#include "common/qsoctaskregistry.h"
#include "smt/qsocsmtbroker.h"
#include <QScopeGuard>
#include <QScopedValueRollback>

#include <cmath>
#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <utility>
#include <QTimer>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

using json = nlohmann::json;

namespace {

using QSocAgentProtocol::frame;
constexpr int kHeaderBytes = QSocAgentProtocol::headerBytes;

QSocAgentRuntimeOptions optionsFromParams(const QJsonObject &params)
{
    QSocAgentRuntimeOptions options;
    options.deferRemoteConnection = true;
    options.launchDirectory       = params.value("launch_directory").toString();
    options.clientProgram         = params.value("client_program").toString(QStringLiteral("qsoc"));
    options.projectDirectory      = params.value(QStringLiteral("project_directory")).toString();
    options.projectName           = params.value(QStringLiteral("project_name")).toString();
    options.workspace             = params.value(QStringLiteral("workspace")).toString();
    options.sshTarget             = params.value(QStringLiteral("ssh_target")).toString();
    options.resumeSessionId       = params.value(QStringLiteral("resume_session_id")).toString();
    options.continueLatestSession
        = params.value(QStringLiteral("continue_latest_session")).toBool(false);
    options.maxContextTokens    = params.value(QStringLiteral("max_context_tokens")).toInt(0);
    options.temperature         = params.value(QStringLiteral("temperature")).toDouble(-1.0);
    options.effortLevel         = params.value(QStringLiteral("effort_level")).toString();
    options.toolPresentation    = params.value(QStringLiteral("tool_presentation")).toString();
    options.streaming           = params.value(QStringLiteral("streaming")).toBool(true);
    options.streamingFromConfig = params.value("streaming_from_config").toBool(false);
    options.verbose             = params.value(QStringLiteral("verbose")).toBool(false);
    return options;
}

} // namespace

/* ---------------------------------------------------------------------- */
/* One client connection. */

class QSocAgentDaemonConnection : public QObject
{
    Q_OBJECT

public:
    explicit QSocAgentDaemonConnection(QSocAgentDaemon *daemon, QLocalSocket *socket)
        : QObject(daemon)
        , daemon_(daemon)
        , socket_(socket)
    {
        socket_->setParent(this);
        socket_->setReadBufferSize(64 * 1024);
        connect(socket_, &QLocalSocket::readyRead, this, &QSocAgentDaemonConnection::handleReadyRead);
        connect(socket_, &QLocalSocket::disconnected, this, [this]() {
            disconnected_ = true;
            if (pendingAsk_)
                pendingAsk_->quit();
            if (runtime_)
                runtime_->abort();
            if (dispatchDepth_ == 0)
                daemon_->removeConnection(this);
        });
    }

    ~QSocAgentDaemonConnection() override = default;

    /** Ask the socket to disconnect (used by daemon shutdown). */
    void close() { socket_->disconnectFromServer(); }

    void greet()
    {
        QJsonObject hello;
        hello.insert(QStringLiteral("daemon"), QStringLiteral("qsoc-agentd"));
        hello.insert(QStringLiteral("version"), QString::fromLatin1(QSOC_VERSION));
        hello.insert(QStringLiteral("protocol"), QSocAgentProtocol::version);
        hello.insert(QStringLiteral("pid"), static_cast<double>(QCoreApplication::applicationPid()));
        sendJson(QJsonDocument(hello).toJson(QJsonDocument::Compact));
    }

private slots:

    void handleReadyRead()
    {
        for (int count = 0; count < 16; ++count) {
            buffer_.append(socket_->read(QSocIpc::maxPayloadBytes + kHeaderBytes - buffer_.size()));
            if (buffer_.size() < kHeaderBytes) {
                return;
            }
            const int length = QSocAgentProtocol::payloadLength(buffer_);
            if (length < 0) {
                socket_->disconnectFromServer();
                return;
            }
            if (buffer_.size() < kHeaderBytes + length) {
                return;
            }
            const QByteArray payload = buffer_.mid(kHeaderBytes, length);
            buffer_.remove(0, kHeaderBytes + length);
            if (pendingFrames_ >= 64 || pendingBytes_ + payload.size() > QSocIpc::maxPayloadBytes) {
                socket_->abort();
                return;
            }
            ++pendingFrames_;
            pendingBytes_ += payload.size();
            QTimer::singleShot(0, this, [this, payload] {
                --pendingFrames_;
                pendingBytes_ -= payload.size();
                if (disconnected_)
                    return;
                ++dispatchDepth_;
                handleFrame(payload);
                --dispatchDepth_;
                if (disconnected_ && dispatchDepth_ == 0)
                    daemon_->removeConnection(this);
            });
        }
        QTimer::singleShot(0, this, &QSocAgentDaemonConnection::handleReadyRead);
    }

private:
    void sendJson(const QByteArray &payload)
    {
        if (socket_->state() != QLocalSocket::ConnectedState) {
            return;
        }
        if (payload.size() > QSocIpc::maxPayloadBytes
            || socket_->bytesToWrite() + payload.size() + kHeaderBytes
                   > 2 * QSocIpc::maxPayloadBytes) {
            socket_->abort();
            return;
        }
        socket_->write(frame(payload));
        socket_->flush();
    }

    void sendReply(qint64 id, const QJsonObject &result)
    {
        QJsonObject reply;
        reply.insert(QStringLiteral("id"), static_cast<double>(id));
        reply.insert(QStringLiteral("result"), result);
        sendJson(QJsonDocument(reply).toJson(QJsonDocument::Compact));
    }

    void sendError(qint64 id, const QString &message)
    {
        QJsonObject reply;
        reply.insert(QStringLiteral("id"), static_cast<double>(id));
        reply.insert(QStringLiteral("error"), message);
        sendJson(QJsonDocument(reply).toJson(QJsonDocument::Compact));
    }

    void sendEvent(const QSocAgentRuntimeEvent &event)
    {
        const json    wire = event.toJson();
        QJsonDocument doc  = QJsonDocument::fromJson(QByteArray::fromStdString(wire.dump()));
        QJsonObject   envelope;
        envelope.insert(QStringLiteral("event"), doc.object());
        sendJson(QJsonDocument(envelope).toJson(QJsonDocument::Compact));
    }

    QJsonObject snapshot(bool transcript = false) const
    {
        QJsonObject result{
            {"model", runtime_->currentModelId()},
            {"effort", runtime_->effortLevel()},
            {"plan_mode", runtime_->planMode()},
            {"cwd", runtime_->workingDirectory()},
            {"remote", runtime_->remoteTarget()},
            {"session_id", runtime_->sessionId()},
            {"resume_command", runtime_->resumeCommand()}};
        result.insert("commands", QJsonArray::fromStringList(runtime_->availableCommands()));
        if (transcript)
            result.insert(
                "messages",
                QJsonDocument::fromJson(
                    QByteArray::fromStdString(runtime_->persistedMessages().dump()))
                    .array());
        return result;
    }

    void ensureRuntime(const QSocAgentRuntimeOptions &options)
    {
        if (runtime_) {
            return;
        }
        runtime_ = std::make_unique<QSocAgentRuntime>(options, this);
        connect(runtime_.get(), &QSocAgentRuntime::sessionChanged, this, [this] {
            deferred_.clear();
            deferredBytes_ = 0;
        });
        connect(
            runtime_.get(),
            &QSocAgentRuntime::eventRaised,
            this,
            [this](const QSocAgentRuntimeEvent &event) { sendEvent(event); });

        /* Frontend interaction: the daemon has no terminal of its own, so
         * interactive requests are surfaced as events and answered through
         * the pending-request queue. */
        runtime_->setAskUserHandler([this](
                                        const QString                  &question,
                                        const QString                  &header,
                                        const QList<QSocAskUserOption> &options) {
            QSocAskUserResult result;
            result.canceled = true;
            if (!interactive_) {
                return result;
            }
            return askInteractive(question, header, options);
        });
        runtime_->setPlanApprovalHandler([this](const QString &plan) {
            const auto       answer = askFrontend("plan", plan);
            QSocPlanApproval result;
            result.approved = answer.value("approved").toBool();
            result.feedback = answer.value("text").toString();
            return result;
        });
        runtime_->setSecretHandler([this](const QString &prompt) {
            return askFrontend("secret", prompt).value("text").toString();
        });
        runtime_->setMenuHandler([this](
                                     const QString     &title,
                                     const QStringList &items,
                                     const QStringList &hints,
                                     const QList<bool> &marked) {
            json rows = json::array();
            for (int i = 0; i < items.size(); ++i)
                rows.push_back(
                    {{"label", items[i].toStdString()},
                     {"description", hints.value(i).toStdString()},
                     {"marked", marked.value(i)}});
            return askFrontend("menu", title, {{"options", rows}}).value("index").toInt(-1);
        });
        runtime_->setTextEditHandler(
            [this](const QString &current, QString *result, QString *error) {
                const auto answer = askFrontend("edit", current);
                if (answer.value("canceled").toBool(true)) {
                    if (error)
                        *error = answer.value("error").toString();
                    return false;
                }
                if (result)
                    *result = answer.value("text").toString();
                return true;
            });
        runtime_->setDirectoryPicker([this](
                                         const QString                       &title,
                                         const QString                       &start,
                                         const QString                       &home,
                                         const QSocAgentRuntime::ListDirsFn  &list,
                                         const QSocAgentRuntime::ListErrorFn &error) {
            listDirs_         = list;
            listError_        = error;
            const auto answer = askFrontend(
                "directory", title, {{"start", start.toStdString()}, {"home", home.toStdString()}});
            listDirs_  = {};
            listError_ = {};
            return answer.value("text").toString();
        });
        runtime_->setUserWatchingProbe([this] { return watching_; });
        auto *timer = new QTimer(this);
        timer->setInterval(100);
        connect(timer, &QTimer::timeout, this, [this] {
            if (busy_ || disconnected_)
                return;
            if (runtime_->hasPendingRecovery()) {
                ++dispatchDepth_;
                handleFrame(QJsonDocument(QJsonObject{{"id", 0}, {"method", "recover"}}).toJson());
                --dispatchDepth_;
                if (disconnected_ && dispatchDepth_ == 0)
                    daemon_->removeConnection(this);
                return;
            }
            if (!deferred_.isEmpty()) {
                const auto payload = deferred_.takeFirst();
                deferredBytes_ -= payload.size();
                ++dispatchDepth_;
                handleFrame(payload);
                --dispatchDepth_;
                if (disconnected_ && dispatchDepth_ == 0)
                    daemon_->removeConnection(this);
                return;
            }
            if (!runtime_->hasPendingAutoInputs())
                return;
            const auto inputs = runtime_->takePendingAutoInputs();
            for (const auto &input : inputs) {
                if (disconnected_)
                    break;
                const auto payload = QJsonDocument(
                                         QJsonObject{
                                             {"id", 0},
                                             {"method",
                                              runtime_->handlesCommand(input) ? "command" : "turn"},
                                             {"params", QJsonObject{{"input", input}}}})
                                         .toJson(QJsonDocument::Compact);
                ++dispatchDepth_;
                handleFrame(payload);
                --dispatchDepth_;
            }
            if (disconnected_ && dispatchDepth_ == 0)
                daemon_->removeConnection(this);
        });
        timer->start();
    }

    QJsonObject askFrontend(const QString &type, const QString &text, json data = json::object())
    {
        if (!interactive_ || disconnected_)
            return {};
        QEventLoop loop;
        pendingAsk_            = &loop;
        askAnswer_             = {};
        const qint64 requestId = ++interactionId_;
        data["type"]           = type.toStdString();
        data["request_id"]     = requestId;
        QSocAgentRuntimeEvent event;
        event.kind = QSocAgentRuntimeEvent::Kind::AskUser;
        event.text = text;
        event.json = std::move(data);
        sendEvent(event);
        loop.exec();
        pendingAsk_ = nullptr;
        return std::exchange(askAnswer_, QJsonObject());
    }

    QSocAskUserResult askInteractive(
        const QString &question, const QString &header, const QList<QSocAskUserOption> &options)
    {
        json rows = json::array();
        for (const auto &option : options)
            rows.push_back(
                {{"label", option.label.toStdString()},
                 {"description", option.description.toStdString()}});
        const auto answer
            = askFrontend("ask", question, {{"header", header.toStdString()}, {"options", rows}});
        QSocAskUserResult result;
        result.canceled = answer.value("canceled").toBool(true);
        result.choice   = answer.value("choice").toString();
        result.text     = answer.value("text").toString();
        return result;
    }

    void handleFrame(const QByteArray &payload)
    {
        QJsonParseError     err{};
        const QJsonDocument doc = QJsonDocument::fromJson(payload, &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) {
            sendError(-1, QStringLiteral("malformed request frame"));
            return;
        }
        const QJsonObject request = doc.object();
        const qint64      id = static_cast<qint64>(request.value(QStringLiteral("id")).toDouble());
        const QString     method = request.value(QStringLiteral("method")).toString();
        const QJsonObject params = request.value(QStringLiteral("params")).toObject();

        if (method == QStringLiteral("answer")) {
            if (pendingAsk_ && params.value("request_id").toInteger() == interactionId_) {
                askAnswer_ = params;
                pendingAsk_->quit();
                sendReply(id, {{"ok", true}});
            } else {
                sendError(id, QStringLiteral("no matching interaction"));
            }
            return;
        }

        if (method == QStringLiteral("watching")) {
            watching_ = params.value("enabled").toBool(true);
            sendReply(id, {{"ok", true}});
            return;
        }
        if (method == QStringLiteral("list_dirs") && listDirs_) {
            const auto dirs = listDirs_(params.value("path").toString());
            sendReply(
                id,
                {{"directories", QJsonArray::fromStringList(dirs)},
                 {"error", listError_ ? listError_() : QString()}});
            return;
        }
        if (method == QStringLiteral("abort")) {
            if (runtime_)
                runtime_->abort();
            if (pendingAsk_)
                pendingAsk_->quit();
            sendReply(id, {{"ok", true}});
            return;
        }
        if (runtime_ && method == "complete") {
            const QString query = params.value("query").toString();
            const auto    paths = runtime_->isRemote()
                                      ? completion_.completeRemote(
                                            runtime_->remoteConnection()->session(),
                                            runtime_->workingDirectory(),
                                            query)
                                      : completion_.complete(runtime_->workingDirectory(), query);
            sendReply(id, {{"items", QJsonArray::fromStringList(paths)}, {"query", query}});
            return;
        }
        if (runtime_ && method == "tasks") {
            QJsonArray rows;
            for (const auto &item : runtime_->taskRegistry()->listAll()) {
                const auto &row = item.row;
                const auto &e   = item.estimate;
                QJsonObject estimate{
                    {"summary", e.summary},
                    {"reason", e.reason},
                    {"remaining", QJsonArray::fromStringList(e.remaining)},
                    {"evidence", QJsonArray::fromStringList(e.evidence)},
                    {"unknowns", QJsonArray::fromStringList(e.unknowns)},
                    {"updated_at", e.updatedAtMs},
                    {"progress_low", e.progressLow},
                    {"progress_high", e.progressHigh},
                    {"seconds_low", e.secondsLow},
                    {"seconds_high", e.secondsHigh}};
                rows.append(
                    QJsonObject{
                        {"source", item.sourceTag},
                        {"id", row.id},
                        {"label", row.label},
                        {"summary", row.summary},
                        {"objective", row.objective},
                        {"kind", static_cast<int>(row.kind)},
                        {"status", static_cast<int>(row.status)},
                        {"started_at", row.startedAtMs},
                        {"can_kill", row.canKill},
                        {"waiting", row.waitingForPeer},
                        {"estimate", estimate}});
            }
            sendReply(id, {{"rows", rows}});
            return;
        }
        if (runtime_ && method == "task_tail") {
            sendReply(
                id,
                {{"text",
                  runtime_->taskRegistry()->tailFor(
                      params.value("source").toString(),
                      params.value("id").toString(),
                      qBound(1, params.value("max_bytes").toInt(8192), 65536))}});
            return;
        }
        if (runtime_ && method == "task_kill") {
            sendReply(
                id,
                {{"ok",
                  runtime_->taskRegistry()
                      ->killTask(params.value("source").toString(), params.value("id").toString())}});
            return;
        }
        if (method == QStringLiteral("shutdown")) {
            sendReply(id, {{"bye", true}});
            daemon_->requestStop();
            return;
        }
        if (busy_) {
            if (method == "turn" || method == "queue" || (method == "command" && runtime_)) {
                const QString input = params.value("input").toString();
                if (input.trimmed().isEmpty()) {
                    sendError(id, "input must not be empty");
                } else if (method != "command" && runtime_ && runtime_->isRunning()) {
                    if (!runtime_->queueRequest(input)) {
                        sendError(id, "input queue is full");
                        return;
                    }
                    QSocAgentRuntimeEvent event;
                    event.kind = QSocAgentRuntimeEvent::Kind::QueuedRequest;
                    event.text = input;
                    sendEvent(event);
                    sendReply(id, {{"queued", true}, {"ok", true}});
                } else if (
                    deferred_.size() < 64
                    && deferredBytes_ + payload.size() <= QSocIpc::maxPayloadBytes) {
                    // Maintenance and commands also enter nested event loops.
                    // Preserve the request until the current operation unwinds.
                    auto queued = request;
                    queued.insert("id", 0);
                    queued.insert("method", method == "command" ? "command" : "turn");
                    const auto encoded = QJsonDocument(queued).toJson(QJsonDocument::Compact);
                    deferredBytes_ += encoded.size();
                    deferred_.append(encoded);
                    sendReply(id, {{"queued", true}, {"ok", true}});
                } else
                    sendError(id, "input queue is full");
            } else
                sendError(id, "session is busy");
            return;
        }
        QScopedValueRollback<bool> busyGuard(busy_, true);

        if (method == QStringLiteral("open")) {
            if (runtime_) {
                sendError(id, QStringLiteral("session already open"));
                return;
            }
            const auto options = optionsFromParams(params);
            ensureRuntime(options);
            QString startupError = runtime_->lastError();
            if (startupError.isEmpty() && !options.sshTarget.isEmpty())
                runtime_->connectRemote(
                    {.target = options.sshTarget, .workspace = options.workspace, .remember = false},
                    &startupError);
            const bool  ok = startupError.isEmpty() && runtime_->openSession();
            QJsonObject result;
            result.insert(QStringLiteral("ok"), ok);
            result.insert(QStringLiteral("snapshot"), snapshot(true));
            result.insert(QStringLiteral("session_id"), runtime_->sessionId());
            result.insert(
                QStringLiteral("error"),
                startupError.isEmpty() ? runtime_->lastError() : startupError);
            sendReply(id, result);
            /* A client that does not say it is interactive never auto-connects. */
            if (ok && options.sshTarget.isEmpty() && options.workspace.isEmpty()
                && !params.value(QStringLiteral("single_query")).toBool(true))
                runtime_->connectRememberedRemote();
            return;
        }

        if (runtime_ == nullptr) {
            sendError(id, QStringLiteral("no session: send {\"method\":\"open\"} first"));
            return;
        }

        if (method == QStringLiteral("turn") || method == "recover") {
            const QString             input = params.value(QStringLiteral("input")).toString();
            const QSocAgentTurnResult turn  = method == "recover" ? runtime_->recoverPendingTurn()
                                                                  : runtime_->runTurn(input);
            QJsonObject               result;
            result.insert(QStringLiteral("final_text"), turn.finalText);
            result.insert(QStringLiteral("stop_notice"), turn.stopNotice);
            result.insert(QStringLiteral("error"), turn.error);
            result.insert(QStringLiteral("error_text"), turn.errorText);
            result.insert(QStringLiteral("aborted"), turn.aborted);
            result.insert(QStringLiteral("persisted"), turn.persistedOk);
            result.insert("snapshot", snapshot());
            sendReply(id, result);
            if (!disconnected_ && !turn.aborted && !turn.error)
                runtime_->finishTurnMaintenance();
            return;
        }

        if (method == QStringLiteral("command")) {
            const QString input           = params.value(QStringLiteral("input")).toString();
            const QString previousSession = runtime_->sessionId();
            const bool    handled         = runtime_->executeCommand(input);
            QJsonObject   result;
            result.insert(QStringLiteral("handled"), handled);
            result.insert(
                QStringLiteral("snapshot"), snapshot(previousSession != runtime_->sessionId()));
            sendReply(id, result);
            return;
        }

        if (method == QStringLiteral("abort")) {
            runtime_->abort();
            QJsonObject result;
            result.insert(QStringLiteral("ok"), true);
            sendReply(id, result);
            return;
        }

        if (method == QStringLiteral("queue")) {
            const QString input = params.value(QStringLiteral("input")).toString();
            QJsonObject   result;
            result.insert(QStringLiteral("ok"), runtime_->queueRequest(input));
            sendReply(id, result);
            return;
        }

        if (method == QStringLiteral("status")) {
            const json  statusLine = runtime_->statusLinePayload();
            QJsonObject result
                = QJsonDocument::fromJson(QByteArray::fromStdString(statusLine.dump())).object();
            result.insert(QStringLiteral("running"), runtime_->isRunning());
            result.insert(QStringLiteral("session_id"), runtime_->sessionId());
            sendReply(id, result);
            return;
        }

        if (method == QStringLiteral("sessions")) {
            QJsonArray rows;
            for (const QSocAgentSessionInfo &info : runtime_->listSessions()) {
                QJsonObject row;
                row.insert(QStringLiteral("id"), info.id);
                row.insert(QStringLiteral("title"), info.title);
                row.insert(QStringLiteral("first_prompt"), info.firstPrompt);
                row.insert(QStringLiteral("branch"), info.branch);
                row.insert(QStringLiteral("message_count"), info.messageCount);
                row.insert(QStringLiteral("last_modified"), info.lastModified.toSecsSinceEpoch());
                rows.append(row);
            }
            QJsonObject result;
            result.insert(QStringLiteral("sessions"), rows);
            sendReply(id, result);
            return;
        }

        if (method == QStringLiteral("open_session")) {
            const bool ok = runtime_->openSessionById(params.value(QStringLiteral("id")).toString());
            QJsonObject result;
            result.insert(QStringLiteral("ok"), ok);
            result.insert(QStringLiteral("session_id"), runtime_->sessionId());
            result.insert(QStringLiteral("error"), runtime_->lastError());
            sendReply(id, result);
            return;
        }

        if (method == QStringLiteral("model")) {
            const bool ok = runtime_->setCurrentModel(params.value(QStringLiteral("id")).toString());
            QJsonObject result;
            result.insert(QStringLiteral("ok"), ok);
            sendReply(id, result);
            return;
        }

        if (method == QStringLiteral("effort")) {
            runtime_->setEffortLevel(params.value(QStringLiteral("level")).toString());
            QJsonObject result;
            result.insert(QStringLiteral("ok"), true);
            sendReply(id, result);
            return;
        }

        if (method == QStringLiteral("plan_mode")) {
            runtime_->setPlanMode(params.value(QStringLiteral("enabled")).toBool(false));
            QJsonObject result;
            result.insert(QStringLiteral("ok"), true);
            sendReply(id, result);
            return;
        }

        if (method == QStringLiteral("remote")) {
            QString    error;
            const bool ok
                = runtime_->connectRemote(params.value(QStringLiteral("target")).toString(), &error);
            QJsonObject result;
            result.insert(QStringLiteral("ok"), ok);
            result.insert(QStringLiteral("error"), error);
            sendReply(id, result);
            return;
        }

        if (method == QStringLiteral("local")) {
            runtime_->disconnectRemote();
            QJsonObject result;
            result.insert(QStringLiteral("ok"), true);
            sendReply(id, result);
            return;
        }

        if (method == QStringLiteral("cwd")) {
            QString    error;
            const bool ok = runtime_->setWorkingDirectory(
                params.value(QStringLiteral("path")).toString(), &error);
            QJsonObject result;
            result.insert(QStringLiteral("ok"), ok);
            result.insert(QStringLiteral("error"), error);
            result.insert(QStringLiteral("cwd"), runtime_->workingDirectory());
            sendReply(id, result);
            return;
        }

        if (method == QStringLiteral("project")) {
            QString    error;
            const bool ok
                = runtime_->switchProject(params.value(QStringLiteral("path")).toString(), &error);
            QJsonObject result;
            result.insert(QStringLiteral("ok"), ok);
            result.insert(QStringLiteral("error"), error);
            sendReply(id, result);
            return;
        }

        if (method == QStringLiteral("interactive")) {
            interactive_ = params.value(QStringLiteral("enabled")).toBool(true);
            QJsonObject result;
            result.insert(QStringLiteral("ok"), true);
            sendReply(id, result);
            return;
        }

        sendError(id, QStringLiteral("unknown method: %1").arg(method));
    }

    QSocAgentDaemon                  *daemon_ = nullptr;
    QLocalSocket                     *socket_ = nullptr;
    QByteArray                        buffer_;
    QList<QByteArray>                 deferred_;
    qsizetype                         deferredBytes_ = 0;
    qsizetype                         pendingBytes_  = 0;
    int                               pendingFrames_ = 0;
    std::unique_ptr<QSocAgentRuntime> runtime_;
    bool                              disconnected_  = false;
    int                               dispatchDepth_ = 0;
    bool                              busy_          = false;
    bool                              interactive_   = true;
    QEventLoop                       *pendingAsk_    = nullptr;
    QJsonObject                       askAnswer_;
    QAgentCompletionEngine            completion_;
    qint64                            interactionId_ = 0;
    bool                              watching_      = true;
    QSocAgentRuntime::ListDirsFn      listDirs_;
    QSocAgentRuntime::ListErrorFn     listError_;
};

/* ---------------------------------------------------------------------- */
/* Daemon. */

/* ---------------------------------------------------------------------- */
/* Relay between one client and its session process. */

class QSocAgentSessionProxy : public QObject
{
    Q_OBJECT

public:
    QSocAgentSessionProxy(QSocAgentDaemon *daemon, QLocalSocket *client)
        : QObject(daemon)
        , daemon_(daemon)
        , client_(client)
    {
        client_->setParent(this);
        const auto peer      = QSocLocalPeer::processId(*client_);
        const auto inherited = daemon_->sessionOwners_.value(peer);
        owner_               = inherited ? inherited : ++daemon_->nextOwner_;
        ownsOwner_           = !inherited;
        client_->setReadBufferSize(64 * 1024);
        worker_.setReadBufferSize(64 * 1024);
        connect(client_, &QLocalSocket::readyRead, this, &QSocAgentSessionProxy::handleClientRead);
        connect(client_, &QLocalSocket::disconnected, this, &QSocAgentSessionProxy::finish);
        connect(&worker_, &QLocalSocket::connected, this, [this] {
            if (!QSocLocalPeer::sameUser(worker_)
                || QSocLocalPeer::processId(worker_) != process_.processId()) {
                finish();
                return;
            }
        });
        connect(&worker_, &QLocalSocket::readyRead, this, &QSocAgentSessionProxy::handleWorkerRead);
        connect(&worker_, &QLocalSocket::disconnected, this, &QSocAgentSessionProxy::finish);
        connect(&worker_, &QLocalSocket::errorOccurred, this, [this] {
            if (workerReady_)
                finish();
            else
                QTimer::singleShot(20, this, &QSocAgentSessionProxy::connectWorker);
        });
        connect(&process_, &QProcess::started, this, [this] {
            if (finished_) {
                process_.kill();
                return;
            }
            sessionPid_ = process_.processId();
            daemon_->sessionOwners_.insert(sessionPid_, owner_);
            connectWorker();
        });
        connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart)
                finish();
        });
        connect(&process_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
            if (status == QProcess::NormalExit && code == QSocAgentDaemon::stopDaemonExitCode)
                QTimer::singleShot(0, daemon_, &QSocAgentDaemon::requestStop);
            finish();
        });
        QTimer::singleShot(0, this, [this] {
            write(
                client_,
                QSocIpc::frame(
                    QJsonObject{
                        {"daemon", "qsoc-agentd"},
                        {"version", QSOC_VERSION},
                        {"protocol", QSocAgentProtocol::version},
                        {"pid", static_cast<double>(QCoreApplication::applicationPid())},
                        {"capabilities", QJsonArray{"smt", "resources"}}}));
        });
        QTimer::singleShot(0, this, &QSocAgentSessionProxy::handleClientRead);
    }

    ~QSocAgentSessionProxy() override
    {
        disconnect(&process_, nullptr, this, nullptr);
        disconnect(&worker_, nullptr, this, nullptr);
        disconnect(client_, nullptr, this, nullptr);
        worker_.abort();
        client_->abort();
        if (process_.state() != QProcess::NotRunning) {
            process_.terminate();
            if (!process_.waitForFinished(1000)) {
                process_.kill();
                process_.waitForFinished(1000);
            }
        }
    }

    void finish()
    {
        if (finished_)
            return;
        finished_ = true;
        daemon_->resources_->cancel(this);
        daemon_->sessionOwners_.remove(sessionPid_);
        if (ownsOwner_)
            daemon_->smtBroker_->removeOwner(owner_);
        else {
            const auto tasks = tasks_.values();
            for (auto task : tasks)
                daemon_->smtBroker_->cancel(task);
        }
        tasks_.clear();
        pending_.clear();
        clientBuffer_.clear();
        workerBuffer_.clear();
        client_->disconnectFromServer();
        worker_.abort();
        if (process_.state() == QProcess::NotRunning) {
            daemon_->removeProxy(this);
            return;
        }
        connect(&process_, &QProcess::finished, this, [this] { daemon_->removeProxy(this); });
        process_.terminate();
        QTimer::singleShot(3000, this, [this] { process_.kill(); });
    }

private:
    static constexpr int       bufferLimit   = QSocIpc::maxPayloadBytes + QSocIpc::headerBytes;
    static constexpr int       writeLimit    = 2 * bufferLimit;
    static constexpr int       dispatchLimit = 16;
    static constexpr qsizetype transitLimit  = 64 * 1024 * 1024;

    bool withinBudget(qsizetype additional = 0) const
    {
        qsizetype bytes = additional;
        for (const auto *proxy : std::as_const(daemon_->proxies_)) {
            bytes += proxy->pending_.size() + proxy->clientBuffer_.size()
                     + proxy->workerBuffer_.size();
            bytes += proxy->client_->bytesAvailable() + proxy->client_->bytesToWrite();
            bytes += proxy->worker_.bytesAvailable() + proxy->worker_.bytesToWrite();
        }
        return bytes <= transitLimit;
    }

    bool write(QLocalSocket *socket, const QByteArray &bytes)
    {
        if (finished_)
            return false;
        if (bytes.isEmpty() || socket->bytesToWrite() + bytes.size() > writeLimit
            || !withinBudget(bytes.size())) {
            finish();
            return false;
        }
        if (socket->write(bytes) < 0) {
            finish();
            return false;
        }
        socket->flush();
        return true;
    }

    void handleClientRead()
    {
        if (finished_)
            return;
        for (int count = 0; count < dispatchLimit; ++count) {
            clientBuffer_ += client_->read(bufferLimit - clientBuffer_.size());
            if (!withinBudget()) {
                finish();
                return;
            }
            QJsonObject request;
            const auto  decoded = QSocIpc::decode(clientBuffer_, request);
            if (decoded == QSocIpc::DecodeResult::Invalid) {
                finish();
                return;
            }
            if (decoded == QSocIpc::DecodeResult::Incomplete)
                return;
            handleRequest(request);
            if (finished_)
                return;
        }
        QTimer::singleShot(0, this, &QSocAgentSessionProxy::handleClientRead);
    }

    void handleRequest(const QJsonObject &request)
    {
        if (!ownsOwner_ && !daemon_->sessionOwners_.values().contains(owner_)) {
            finish();
            return;
        }
        const auto   method = request.value("method").toString();
        const auto   params = request.value("params").toObject();
        const auto   value  = request.value("id");
        const double number = value.toDouble(-1);
        if (!value.isDouble() || number < 0 || number > 9007199254740991.0
            || std::floor(number) != number) {
            finish();
            return;
        }
        const auto id = static_cast<qint64>(number);
        if (tasks_.contains(id) || resourceRequests_.contains(id)) {
            finish();
            return;
        }
        if (method == "resources") {
            QJsonArray paths;
            if ((request.contains("params") && !request.value("params").isObject())
                || !QSocDaemonResources::validatePaths(params, paths)) {
                write(
                    client_,
                    QSocIpc::frame(
                        QJsonObject{
                            {"id", number},
                            {"error", "Expected at most eight absolute local paths"}}));
                return;
            }
            resourceRequests_.insert(id);
            const QPointer<QSocAgentSessionProxy> self(this);
            daemon_->resources_->request(this, paths, [self, id](QJsonObject result) {
                if (!self || self->finished_)
                    return;
                self->resourceRequests_.remove(id);
                result.insert("smt", self->daemon_->smtBroker_->resourceStatus());
                self->write(
                    self->client_,
                    QSocIpc::frame(
                        QJsonObject{{"id", static_cast<double>(id)}, {"result", result}}));
            });
            return;
        }
        if (method == "smt.solve") {
            const QPointer<QSocAgentSessionProxy> self(this);
            const auto                            task
                = daemon_->smtBroker_->submit(owner_, params, [self, id](const QJsonObject &result) {
                      if (!self || self->finished_)
                          return;
                      self->tasks_.remove(id);
                      self->write(
                          self->client_,
                          QSocIpc::frame(
                              QJsonObject{{"id", static_cast<double>(id)}, {"result", result}}));
                  });
            if (task)
                tasks_.insert(id, task);
            return;
        }
        if (method == "smt.cancel") {
            const auto target   = params.value("request_id").toDouble(-1);
            const bool valid    = target >= 0 && target <= 9007199254740991.0
                                  && std::floor(target) == target;
            const auto task     = valid ? tasks_.value(static_cast<qint64>(target)) : 0;
            const bool canceled = task && daemon_->smtBroker_->cancel(task);
            write(
                client_,
                QSocIpc::frame(
                    QJsonObject{{"id", number}, {"result", QJsonObject{{"canceled", canceled}}}}));
            return;
        }
        if (!ownsOwner_) {
            write(
                client_,
                QSocIpc::frame(
                    QJsonObject{
                        {"id", number},
                        {"error", "session service connection only accepts service requests"}}));
            return;
        }
        if (method == "shutdown") {
            write(
                client_,
                QSocIpc::frame(QJsonObject{{"id", number}, {"result", QJsonObject{{"bye", true}}}}));
            daemon_->requestStop();
            return;
        }
        const auto bytes = QSocIpc::frame(request);
        if (workerReady_) {
            write(&worker_, bytes);
            return;
        }
        if (pending_.size() + bytes.size() > bufferLimit || !withinBudget(bytes.size())) {
            finish();
            return;
        }
        pending_ += bytes;
        if (!directory_)
            startSession();
    }

    void startSession()
    {
        directory_ = std::make_unique<QTemporaryDir>();
        if (!directory_->isValid()) {
            finish();
            return;
        }
        startup_         = QDeadlineTimer(5000);
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("QSOC_SMT_SOCKET", daemon_->socketPath_);
        environment.insert("QSOC_AGENT_SOCKET", daemon_->socketPath_);
        process_.setProcessEnvironment(environment);
        process_.setStandardInputFile(QProcess::nullDevice());
        process_.setStandardOutputFile(QProcess::nullDevice());
        process_.setProcessChannelMode(QProcess::ForwardedErrorChannel);
        process_.start(
            QCoreApplication::applicationFilePath(),
            {QStringLiteral("--session-worker"),
             QStringLiteral("--socket"),
             workerPath(),
             QStringLiteral("--parent-pid"),
             QString::number(QCoreApplication::applicationPid())});
        QTimer::singleShot(5000, this, [this] {
            if (!workerReady_)
                finish();
        });
    }

    void handleWorkerRead()
    {
        if (finished_)
            return;
        for (int count = 0; count < dispatchLimit; ++count) {
            workerBuffer_ += worker_.read(bufferLimit - workerBuffer_.size());
            if (!withinBudget()) {
                finish();
                return;
            }
            QJsonObject message;
            const auto  decoded = QSocIpc::decode(workerBuffer_, message);
            if (decoded == QSocIpc::DecodeResult::Invalid) {
                finish();
                return;
            }
            if (decoded == QSocIpc::DecodeResult::Incomplete)
                return;
            if (!workerReady_) {
                if (message.value("daemon") != "qsoc-agentd"
                    || message.value("protocol").toInt() != QSocAgentProtocol::version) {
                    finish();
                    return;
                }
                workerReady_ = true;
                if (!pending_.isEmpty()) {
                    const auto pending = std::exchange(pending_, QByteArray());
                    write(&worker_, pending);
                }
            } else {
                write(client_, QSocIpc::frame(message));
            }
            if (finished_)
                return;
        }
        QTimer::singleShot(0, this, &QSocAgentSessionProxy::handleWorkerRead);
    }

    QString workerPath() const
    {
        return QSocLocalEndpoint::resolve(directory_->filePath(QStringLiteral("session.sock")));
    }

    void connectWorker()
    {
        if (finished_ || worker_.state() != QLocalSocket::UnconnectedState)
            return;
        if (process_.state() == QProcess::NotRunning || startup_.hasExpired()) {
            finish();
            return;
        }
        worker_.connectToServer(workerPath());
    }

    QSocAgentDaemon               *daemon_;
    QLocalSocket                  *client_;
    QLocalSocket                   worker_;
    QProcess                       process_;
    std::unique_ptr<QTemporaryDir> directory_;
    QByteArray                     pending_;
    QByteArray                     clientBuffer_;
    QByteArray                     workerBuffer_;
    QHash<qint64, quint64>         tasks_;
    QDeadlineTimer                 startup_;
    qint64                         sessionPid_ = 0;
    quint64                        owner_      = 0;
    QSet<qint64>                   resourceRequests_;
    bool                           ownsOwner_   = false;
    bool                           workerReady_ = false;
    bool                           finished_    = false;
};

QSocAgentDaemon::QSocAgentDaemon(
    const QString &socketPath, QObject *parent, QSocMemoryBudget::Policy memoryPolicy)
    : QObject(parent)
    , socketPath_(
          QSocLocalEndpoint::resolve(socketPath.isEmpty() ? defaultSocketPath() : socketPath))
{
    smtBroker_
        = std::make_unique<QSocSmtBroker>(this, QSocSmtBroker::Solver{}, 120000, memoryPolicy);
    resources_ = std::make_unique<QSocDaemonResources>(this);
    connect(&server_, &QLocalServer::newConnection, this, &QSocAgentDaemon::handleNewConnection);
}

QSocAgentDaemon::~QSocAgentDaemon()
{
    shutdown();
    proxies_.clear();
    qDeleteAll(findChildren<QSocAgentSessionProxy *>(QString(), Qt::FindDirectChildrenOnly));
    connections_.clear();
    qDeleteAll(findChildren<QSocAgentDaemonConnection *>(QString(), Qt::FindDirectChildrenOnly));
}

QString QSocAgentDaemon::defaultSocketPath()
{
    QString base      = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    QString directory = QStringLiteral("qsoc");
    if (base.isEmpty()) {
        base = QDir::tempPath();
#ifdef Q_OS_UNIX
        directory += QStringLiteral("-%1").arg(static_cast<qulonglong>(::geteuid()));
#endif
    }
    return QSocLocalEndpoint::resolve(
        QDir(base).filePath(directory + QStringLiteral("/agentd.sock")));
}

bool QSocAgentDaemon::start()
{
    const QFileInfo info(socketPath_);
    if (!QSocLocalEndpoint::prepareDirectory(socketPath_, &error_)) {
        return false;
    }
    socketLock_ = std::make_unique<QLockFile>(socketPath_ + QStringLiteral(".lock"));
    socketLock_->setStaleLockTime(0);
    if (!socketLock_->tryLock(0)) {
        error_ = QStringLiteral("another daemon owns %1").arg(socketPath_);
        return false;
    }
    // Also refuse listeners from older versions that did not use a lock.
    QLocalSocket probe;
    probe.connectToServer(socketPath_);
    if (probe.waitForConnected(100)) {
        error_ = QStringLiteral("another daemon is listening at %1").arg(socketPath_);
        socketLock_.reset();
        return false;
    }
    server_.setSocketOptions(QLocalServer::UserAccessOption);
    if (info.exists()) {
        QLocalServer::removeServer(socketPath_);
    }
    if (!server_.listen(socketPath_)) {
        error_
            = QStringLiteral("could not listen on %1: %2").arg(socketPath_, server_.errorString());
        return false;
    }
    return true;
}

int QSocAgentDaemon::connectionCount() const
{
    return static_cast<int>(connections_.size() + proxies_.size());
}

void QSocAgentDaemon::requestStop()
{
    stopRequested_ = true;
    shutdown();
    const int code = singleSession_ ? stopDaemonExitCode : 0;
    QTimer::singleShot(0, qApp, [code] { QCoreApplication::exit(code); });
}

void QSocAgentDaemon::shutdown()
{
    resources_->shutdown();
    const auto proxies = proxies_;
    for (QSocAgentSessionProxy *proxy : proxies) {
        proxy->finish();
    }
    const auto connections = connections_;
    for (QSocAgentDaemonConnection *connection : connections) {
        connection->close();
    }
    if (server_.isListening()) {
        server_.close();
    }
    smtBroker_->shutdown();
    socketLock_.reset();
}

void QSocAgentDaemon::handleNewConnection()
{
    while (server_.hasPendingConnections()) {
        QLocalSocket *socket = server_.nextPendingConnection();
        if (stopRequested_ || connectionCount() >= 64 || !QSocLocalPeer::sameUser(*socket)
            || QSocLocalPeer::processId(*socket) <= 0) {
            socket->abort();
            socket->deleteLater();
            continue;
        }
        if (!singleSession_) {
            proxies_.append(new QSocAgentSessionProxy(this, socket));
            emit connectionCountChanged(connectionCount());
            continue;
        }
        if (!connections_.isEmpty()) {
            socket->abort();
            socket->deleteLater();
            continue;
        }
        auto *connection = new QSocAgentDaemonConnection(this, socket);
        connections_.append(connection);
        connection->greet();
        emit connectionCountChanged(connectionCount());
    }
}

void QSocAgentDaemon::removeConnection(QSocAgentDaemonConnection *connection)
{
    if (connections_.removeOne(connection)) {
        connection->deleteLater();
        emit connectionCountChanged(connectionCount());
        if (singleSession_) {
            const int code = stopRequested_ ? stopDaemonExitCode : 0;
            QTimer::singleShot(0, qApp, [code] { QCoreApplication::exit(code); });
        }
    }
}

void QSocAgentDaemon::removeProxy(QSocAgentSessionProxy *proxy)
{
    if (proxies_.removeOne(proxy)) {
        proxy->deleteLater();
        emit connectionCountChanged(connectionCount());
    }
}

#include "qsocagentdaemon.moc"

#include "moc_qsocagentdaemon.cpp"

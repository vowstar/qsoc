// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/daemon/qsocagentdaemon.h"

#include "agent/qsoctaskregistry.h"
#include "agent/remote/qsocagentremote.h"
#include "agent/runtime/qsocagentprotocol.h"
#include "agent/runtime/qsocagentruntimeevent.h"
#include "agent/services/qagentcompletion.h"
#include "agent/tool/qsoctoolaskuser.h"
#include "agent/tool/qsoctoolplanmode.h"
#include "common/config.h"
#include <QScopeGuard>
#include <QScopedValueRollback>

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

#include <utility>
#include <QTimer>

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
        connect(socket_, &QLocalSocket::readyRead, this, &QSocAgentDaemonConnection::onReadyRead);
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

    void onReadyRead()
    {
        buffer_.append(socket_->readAll());
        while (true) {
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
            QTimer::singleShot(0, this, [this, payload] {
                if (disconnected_)
                    return;
                ++dispatchDepth_;
                handleFrame(payload);
                --dispatchDepth_;
                if (disconnected_ && dispatchDepth_ == 0)
                    daemon_->removeConnection(this);
            });
        }
    }

private:
    void sendJson(const QByteArray &payload)
    {
        if (socket_->state() != QLocalSocket::ConnectedState) {
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
            daemon_->shutdown();
            QTimer::singleShot(0, qApp, &QCoreApplication::quit);
            return;
        }
        if (busy_) {
            if (method == "turn" || method == "queue" || (method == "command" && runtime_)) {
                const QString input = params.value("input").toString();
                if (input.trimmed().isEmpty()) {
                    sendError(id, "input must not be empty");
                } else if (method != "command" && runtime_ && runtime_->queueRequest(input)) {
                    QSocAgentRuntimeEvent event;
                    event.kind = QSocAgentRuntimeEvent::Kind::QueuedRequest;
                    event.text = input;
                    sendEvent(event);
                    sendReply(id, {{"queued", true}, {"ok", true}});
                } else if (deferred_.size() < 64) {
                    // Maintenance and commands also enter nested event loops.
                    // Preserve the request until the current operation unwinds.
                    auto queued = request;
                    queued.insert("id", 0);
                    queued.insert("method", method == "command" ? "command" : "turn");
                    deferred_.append(QJsonDocument(queued).toJson(QJsonDocument::Compact));
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
                runtime_->connectRemote(options.sshTarget, &startupError);
            const bool  ok = startupError.isEmpty() && runtime_->openSession();
            QJsonObject result;
            result.insert(QStringLiteral("ok"), ok);
            result.insert(QStringLiteral("snapshot"), snapshot(true));
            result.insert(QStringLiteral("session_id"), runtime_->sessionId());
            result.insert(
                QStringLiteral("error"),
                startupError.isEmpty() ? runtime_->lastError() : startupError);
            sendReply(id, result);
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

QSocAgentDaemon::QSocAgentDaemon(const QString &socketPath, QObject *parent)
    : QObject(parent)
    , socketPath_(socketPath.isEmpty() ? defaultSocketPath() : socketPath)
{
    connect(&server_, &QLocalServer::newConnection, this, &QSocAgentDaemon::onNewConnection);
}

QSocAgentDaemon::~QSocAgentDaemon()
{
    shutdown();
}

QString QSocAgentDaemon::defaultSocketPath()
{
    const QString runtimeDir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    const QString base       = runtimeDir.isEmpty() ? QDir::tempPath() : runtimeDir;
    return QDir(base).filePath(QStringLiteral("qsoc/agentd.sock"));
}

bool QSocAgentDaemon::start()
{
    const QFileInfo info(socketPath_);
    if (!QDir().mkpath(info.absolutePath())) {
        error_ = QStringLiteral("could not create socket directory %1").arg(info.absolutePath());
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
    return connections_.size();
}

void QSocAgentDaemon::shutdown()
{
    const auto connections = connections_;
    for (QSocAgentDaemonConnection *connection : connections) {
        connection->close();
    }
    if (server_.isListening()) {
        server_.close();
    }
    socketLock_.reset();
}

void QSocAgentDaemon::onNewConnection()
{
    while (server_.hasPendingConnections()) {
        QLocalSocket *socket     = server_.nextPendingConnection();
        auto         *connection = new QSocAgentDaemonConnection(this, socket);
        connections_.append(connection);
        connection->greet();
        emit connectionCountChanged(connections_.size());
    }
}

void QSocAgentDaemon::removeConnection(QSocAgentDaemonConnection *connection)
{
    if (connections_.removeOne(connection)) {
        connection->deleteLater();
        emit connectionCountChanged(connections_.size());
    }
}

#include "qsocagentdaemon.moc"

#include "moc_qsocagentdaemon.cpp"

// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/mcp/qsocmcpclient.h"

#include "agent/mcp/qsocmcptransport.h"

#include <QTimer>

namespace {

constexpr auto kProtocolVersion = "2025-03-26";
constexpr auto kClientName      = "qsoc-agent";
constexpr auto kClientVersion   = "0.1.0";

constexpr int kJsonRpcParseError     = -32700;
constexpr int kJsonRpcInvalidRequest = -32600;
constexpr int kJsonRpcInternalError  = -32603;
constexpr int kClientErrorTransport  = -32000;
constexpr int kClientErrorTimeout    = -32001;
constexpr int kClientErrorNotReady   = -32002;

bool isJsonRpcResponse(const nlohmann::json &message)
{
    return message.is_object() && message.contains("id")
           && (message.contains("result") || message.contains("error"));
}

bool isJsonRpcNotification(const nlohmann::json &message)
{
    return message.is_object() && message.contains("method") && !message.contains("id");
}

} // namespace

QSocMcpClient::QSocMcpClient(McpServerConfig config, QSocMcpTransport *transport, QObject *parent)
    : QObject(parent)
    , config_(std::move(config))
    , transport_(transport)
{
    if (transport_ != nullptr) {
        transport_->setParent(this);
        connect(transport_, &QSocMcpTransport::started, this, &QSocMcpClient::onTransportStarted);
        connect(transport_, &QSocMcpTransport::closed, this, &QSocMcpClient::onTransportClosed);
        connect(transport_, &QSocMcpTransport::errorOccurred, this, &QSocMcpClient::onTransportError);
        connect(transport_, &QSocMcpTransport::messageSent, this, &QSocMcpClient::onMessageSent);
        connect(
            transport_, &QSocMcpTransport::messageReceived, this, &QSocMcpClient::onMessageReceived);
    }
}

QSocMcpClient::~QSocMcpClient() = default;

const McpServerConfig &QSocMcpClient::config() const
{
    return config_;
}

QString QSocMcpClient::name() const
{
    return config_.name;
}

QSocMcpClient::State QSocMcpClient::state() const
{
    return state_;
}

const nlohmann::json &QSocMcpClient::serverCapabilities() const
{
    return serverCapabilities_;
}

void QSocMcpClient::start()
{
    if (transport_ == nullptr) {
        setState(State::Failed);
        return;
    }
    if (lifecycle_ != Lifecycle::Idle) {
        return;
    }

    lifecycle_                               = Lifecycle::Active;
    const quint64                 generation = ++lifecycleGeneration_;
    const QPointer<QSocMcpClient> guard(this);
    serverCapabilities_ = nlohmann::json::object();
    clearInitializedSend();
    setState(State::Connecting);
    if (guard.isNull() || !isCurrentLifecycle(generation, Lifecycle::Active)
        || state_ != State::Connecting) {
        return;
    }
    transport_->start();
}

void QSocMcpClient::stop()
{
    if (lifecycle_ != Lifecycle::Active) {
        return;
    }

    lifecycle_                               = Lifecycle::Stopping;
    const quint64                 generation = lifecycleGeneration_;
    const QPointer<QSocMcpClient> guard(this);
    clearInitializedSend();
    setState(State::Disconnected);
    if (guard.isNull() || !isCurrentLifecycle(generation, Lifecycle::Stopping)) {
        return;
    }
    if (!cancelAllPending(kClientErrorTransport, QStringLiteral("Client stopping"))
        || guard.isNull() || !isCurrentLifecycle(generation, Lifecycle::Stopping)) {
        return;
    }
    stopTransportOrFinish(generation);
}

int QSocMcpClient::request(
    const QString &method, const nlohmann::json &params, int timeoutMs, int *assignedId)
{
    if (assignedId != nullptr) {
        *assignedId = -1;
    }
    if (lifecycle_ != Lifecycle::Active || state_ != State::Ready) {
        return -1;
    }

    const int id = allocateId();
    if (assignedId != nullptr) {
        *assignedId = id;
    }

    nlohmann::json msg;
    msg["jsonrpc"] = "2.0";
    msg["id"]      = id;
    msg["method"]  = method.toStdString();
    msg["params"]  = params;

    Pending pending;
    pending.method = method;

    const int effective = effectiveTimeoutMs(timeoutMs);
    if (effective > 0) {
        auto *timer = new QTimer(this);
        timer->setSingleShot(true);
        const quint64 generation = lifecycleGeneration_;
        connect(timer, &QTimer::timeout, this, [this, id, generation]() {
            if (!isCurrentLifecycle(generation, Lifecycle::Active) || !pending_.contains(id)) {
                return;
            }
            Pending pending = pending_.take(id);
            if (!pending.timer.isNull()) {
                pending.timer->deleteLater();
            }
            emit requestFailed(
                id,
                kClientErrorTimeout,
                QStringLiteral("Request timed out: %1").arg(pending.method));
        });
        timer->start(effective);
        pending.timer = timer;
    }
    pending_.insert(id, pending);

    writeMessage(msg);
    return id;
}

void QSocMcpClient::notify(const QString &method, const nlohmann::json &params)
{
    if (transport_ == nullptr || lifecycle_ != Lifecycle::Active || state_ != State::Ready) {
        return;
    }
    nlohmann::json msg;
    msg["jsonrpc"] = "2.0";
    msg["method"]  = method.toStdString();
    msg["params"]  = params;
    writeMessage(msg);
}

void QSocMcpClient::onTransportStarted()
{
    if (lifecycle_ != Lifecycle::Active || state_ != State::Connecting) {
        return;
    }
    if (transport_ == nullptr || transport_->state() != QSocMcpTransport::State::Running) {
        return;
    }
    const quint64                 generation = lifecycleGeneration_;
    const QPointer<QSocMcpClient> guard(this);
    setState(State::Initializing);
    if (guard.isNull() || !isCurrentLifecycle(generation, Lifecycle::Active)
        || state_ != State::Initializing) {
        return;
    }
    sendInitialize();
}

void QSocMcpClient::onTransportClosed()
{
    if (transport_ != nullptr && transport_->state() != QSocMcpTransport::State::Stopped) {
        return;
    }
    finishLifecycle(lifecycleGeneration_);
}

void QSocMcpClient::onTransportError(const QString &message)
{
    if (lifecycle_ != Lifecycle::Active) {
        return;
    }

    const bool    recoverable = state_ == State::Ready && transport_ != nullptr
                                && transport_->state() == QSocMcpTransport::State::Running;
    const quint64 generation  = lifecycleGeneration_;
    const QPointer<QSocMcpClient> guard(this);
    if (recoverable) {
        cancelAllPending(kClientErrorTransport, message);
        return;
    }

    lifecycle_ = Lifecycle::Stopping;
    clearInitializedSend();
    setState(State::Failed);
    if (guard.isNull() || !isCurrentLifecycle(generation, Lifecycle::Stopping)) {
        return;
    }
    if (!cancelAllPending(kClientErrorTransport, message) || guard.isNull()
        || !isCurrentLifecycle(generation, Lifecycle::Stopping)) {
        return;
    }
    stopTransportOrFinish(generation);
}

void QSocMcpClient::onMessageSent(quint64 token)
{
    if (token == 0 || token != initializedSendToken_ || lifecycle_ != Lifecycle::Active
        || state_ != State::Initializing) {
        return;
    }

    clearInitializedSend();
    const quint64                 generation = lifecycleGeneration_;
    const QPointer<QSocMcpClient> guard(this);
    setState(State::Ready);
    if (guard.isNull() || !isCurrentLifecycle(generation, Lifecycle::Active)
        || state_ != State::Ready) {
        return;
    }
    emit ready();
}

void QSocMcpClient::onMessageReceived(const nlohmann::json &message)
{
    if (lifecycle_ != Lifecycle::Active
        || (state_ != State::Initializing && state_ != State::Ready)) {
        return;
    }
    const quint64 generation = lifecycleGeneration_;

    if (isJsonRpcResponse(message)) {
        if (!message.contains("id") || !message["id"].is_number_integer()) {
            return;
        }
        const int id = message["id"].get<int>();

        if (!pending_.contains(id)) {
            return;
        }
        Pending pending = pending_.take(id);
        if (!pending.timer.isNull()) {
            pending.timer->stop();
            pending.timer->deleteLater();
        }

        if (id == initializeId_) {
            initializeId_ = -1;
            if (message.contains("error")) {
                lifecycle_ = Lifecycle::Stopping;
                const QPointer<QSocMcpClient> guard(this);
                setState(State::Failed);
                if (guard.isNull() || !isCurrentLifecycle(generation, Lifecycle::Stopping)) {
                    return;
                }
                const auto &err  = message["error"];
                const int   code = err.value("code", kJsonRpcInternalError);
                emit        requestFailed(
                    id, code, QString::fromStdString(err.value("message", "Initialize failed")));
                if (guard.isNull() || !isCurrentLifecycle(generation, Lifecycle::Stopping)) {
                    return;
                }
                stopTransportOrFinish(generation);
                return;
            }
            const auto &result = message["result"];
            if (result.is_object() && result.contains("capabilities")) {
                serverCapabilities_ = result["capabilities"];
            } else {
                serverCapabilities_ = nlohmann::json::object();
            }
            sendInitializedNotification(id);
            return;
        }

        if (message.contains("error")) {
            const auto &err  = message["error"];
            const int   code = err.value("code", kJsonRpcInternalError);
            emit        requestFailed(
                id, code, QString::fromStdString(err.value("message", "Unknown RPC error")));
            return;
        }
        emit responseReceived(id, message["result"]);
        return;
    }

    if (isJsonRpcNotification(message)) {
        const QString  method = QString::fromStdString(message["method"].get<std::string>());
        nlohmann::json params = message.contains("params") ? message["params"]
                                                           : nlohmann::json::object();
        emit           notificationReceived(method, params);
        return;
    }

    emit requestFailed(-1, kJsonRpcInvalidRequest, QStringLiteral("Unrecognized message"));
}

void QSocMcpClient::setState(State newState)
{
    if (state_ == newState) {
        return;
    }
    state_ = newState;
    emit stateChanged(state_);
}

void QSocMcpClient::sendInitialize()
{
    if (lifecycle_ != Lifecycle::Active || state_ != State::Initializing) {
        return;
    }
    initializeId_ = allocateId();

    nlohmann::json msg;
    msg["jsonrpc"]                         = "2.0";
    msg["id"]                              = initializeId_;
    msg["method"]                          = "initialize";
    msg["params"]["protocolVersion"]       = kProtocolVersion;
    msg["params"]["capabilities"]          = nlohmann::json::object();
    msg["params"]["clientInfo"]["name"]    = kClientName;
    msg["params"]["clientInfo"]["version"] = kClientVersion;

    Pending pending;
    pending.method = QStringLiteral("initialize");

    const int effective = effectiveTimeoutMs(-1);
    if (effective > 0) {
        auto *timer = new QTimer(this);
        timer->setSingleShot(true);
        const int     id         = initializeId_;
        const quint64 generation = lifecycleGeneration_;
        connect(timer, &QTimer::timeout, this, [this, id, generation]() {
            if (!isCurrentLifecycle(generation, Lifecycle::Active) || initializeId_ != id
                || !pending_.contains(id)) {
                return;
            }
            Pending pending = pending_.take(id);
            if (!pending.timer.isNull()) {
                pending.timer->deleteLater();
            }
            initializeId_ = -1;
            lifecycle_    = Lifecycle::Stopping;
            const QPointer<QSocMcpClient> guard(this);
            setState(State::Failed);
            if (guard.isNull() || !isCurrentLifecycle(generation, Lifecycle::Stopping)) {
                return;
            }
            emit requestFailed(id, kClientErrorTimeout, QStringLiteral("Initialize timed out"));
            if (guard.isNull() || !isCurrentLifecycle(generation, Lifecycle::Stopping)) {
                return;
            }
            stopTransportOrFinish(generation);
        });
        timer->start(effective);
        pending.timer = timer;
    }
    pending_.insert(initializeId_, pending);

    writeMessage(msg);
}

void QSocMcpClient::sendInitializedNotification(int requestId)
{
    nlohmann::json msg;
    msg["jsonrpc"]        = "2.0";
    msg["method"]         = "notifications/initialized";
    msg["params"]         = nlohmann::json::object();
    initializedSendToken_ = lifecycleGeneration_;
    initializedRequestId_ = requestId;

    const int effective = effectiveTimeoutMs(-1);
    if (effective > 0) {
        auto *timer = new QTimer(this);
        timer->setSingleShot(true);
        initializedSendTimer_    = timer;
        const quint64 generation = lifecycleGeneration_;
        const quint64 token      = initializedSendToken_;
        connect(timer, &QTimer::timeout, this, [this, generation, token]() {
            if (!isCurrentLifecycle(generation, Lifecycle::Active) || state_ != State::Initializing
                || initializedSendToken_ != token) {
                return;
            }
            const int requestId = initializedRequestId_;
            clearInitializedSend();
            lifecycle_ = Lifecycle::Stopping;
            const QPointer<QSocMcpClient> guard(this);
            setState(State::Failed);
            if (guard.isNull() || !isCurrentLifecycle(generation, Lifecycle::Stopping)) {
                return;
            }
            emit requestFailed(
                requestId,
                kClientErrorTimeout,
                QStringLiteral("Initialized notification timed out"));
            if (guard.isNull() || !isCurrentLifecycle(generation, Lifecycle::Stopping)) {
                return;
            }
            stopTransportOrFinish(generation);
        });
        timer->start(effective);
    }
    transport_->sendTrackedMessage(msg, initializedSendToken_);
}

void QSocMcpClient::clearInitializedSend()
{
    initializedSendToken_ = 0;
    initializedRequestId_ = -1;
    QTimer *timer         = initializedSendTimer_.data();
    initializedSendTimer_ = nullptr;
    if (timer != nullptr) {
        timer->stop();
        timer->deleteLater();
    }
}

int QSocMcpClient::allocateId()
{
    return nextId_++;
}

void QSocMcpClient::writeMessage(const nlohmann::json &message)
{
    if (transport_ == nullptr || lifecycle_ != Lifecycle::Active) {
        return;
    }
    transport_->sendMessage(message);
    Q_UNUSED(kJsonRpcParseError);
    Q_UNUSED(kClientErrorNotReady);
}

bool QSocMcpClient::cancelAllPending(int code, const QString &message)
{
    QHash<int, Pending> pending;
    pending.swap(pending_);
    initializeId_ = -1;

    const auto ids = pending.keys();
    for (int id : ids) {
        const Pending &entry = pending[id];
        if (!entry.timer.isNull()) {
            entry.timer->stop();
            entry.timer->deleteLater();
        }
    }

    const QPointer<QSocMcpClient> guard(this);
    for (int id : ids) {
        emit requestFailed(id, code, message);
        if (guard.isNull()) {
            return false;
        }
    }
    return true;
}

bool QSocMcpClient::isCurrentLifecycle(quint64 generation, Lifecycle lifecycle) const
{
    return lifecycleGeneration_ == generation && lifecycle_ == lifecycle;
}

void QSocMcpClient::stopTransportOrFinish(quint64 generation)
{
    if (!isCurrentLifecycle(generation, Lifecycle::Stopping)) {
        return;
    }
    if (transport_ == nullptr || transport_->state() == QSocMcpTransport::State::Idle
        || transport_->state() == QSocMcpTransport::State::Stopped) {
        finishLifecycle(generation);
        return;
    }
    transport_->stop();
}

void QSocMcpClient::finishLifecycle(quint64 generation)
{
    if (lifecycleGeneration_ != generation
        || (lifecycle_ != Lifecycle::Active && lifecycle_ != Lifecycle::Stopping)) {
        return;
    }

    lifecycle_ = Lifecycle::Finishing;
    const QPointer<QSocMcpClient> guard(this);
    serverCapabilities_ = nlohmann::json::object();
    clearInitializedSend();
    setState(State::Disconnected);
    if (guard.isNull() || !isCurrentLifecycle(generation, Lifecycle::Finishing)) {
        return;
    }
    if (!cancelAllPending(kClientErrorTransport, QStringLiteral("Transport closed"))
        || guard.isNull() || !isCurrentLifecycle(generation, Lifecycle::Finishing)) {
        return;
    }
    lifecycle_ = Lifecycle::Idle;
    emit closed();
}

int QSocMcpClient::effectiveTimeoutMs(int requested) const
{
    if (requested >= 0) {
        return requested;
    }
    return config_.requestTimeoutMs;
}

// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/mcp/qsocmcpclient.h"

#include "agent/mcp/qsocmcptransport.h"

#include <limits>

#include <QTimer>

namespace {

constexpr auto kProtocolVersion = "2025-03-26";
constexpr auto kClientName      = "qsoc-agent";
constexpr auto kClientVersion   = "0.1.0";

constexpr int kJsonRpcParseError     = -32700;
constexpr int kJsonRpcInvalidRequest = -32600;
constexpr int kJsonRpcMethodNotFound = -32601;
constexpr int kClientErrorTransport  = -32000;
constexpr int kClientErrorTimeout    = -32001;
constexpr int kClientErrorNotReady   = -32002;

bool hasJsonRpcVersion(const nlohmann::json &message)
{
    return message.is_object() && message.contains("jsonrpc") && message["jsonrpc"].is_string()
           && message["jsonrpc"] == "2.0";
}

bool readInteger(const nlohmann::json &value, int *result)
{
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        if (number > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
            return false;
        }
        *result = static_cast<int>(number);
        return true;
    }
    if (!value.is_number_integer()) {
        return false;
    }
    const auto number = value.get<std::int64_t>();
    if (number < std::numeric_limits<int>::min() || number > std::numeric_limits<int>::max()) {
        return false;
    }
    *result = static_cast<int>(number);
    return true;
}

bool readJsonRpcError(const nlohmann::json &error, int *code, QString *message)
{
    if (!error.is_object() || !error.contains("code") || !error.contains("message")
        || !error["message"].is_string() || !readInteger(error["code"], code)) {
        return false;
    }
    *message = QString::fromStdString(error["message"].get<std::string>());
    return true;
}

bool isRequestId(const nlohmann::json &value)
{
    return value.is_string() || value.is_number();
}

nlohmann::json jsonRpcErrorResponse(const nlohmann::json &id, int code, const char *message)
{
    return {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"error", {{"code", code}, {"message", message}}},
    };
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
        connect(transport_, &QSocMcpTransport::messageFailed, this, &QSocMcpClient::onMessageFailed);
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

    const int initializedRequestId = initializedRequestId_;
    lifecycle_                     = Lifecycle::Stopping;
    clearInitializedSend();
    setState(State::Failed);
    if (guard.isNull() || !isCurrentLifecycle(generation, Lifecycle::Stopping)) {
        return;
    }
    if (initializedRequestId >= 0) {
        emit requestFailed(initializedRequestId, kClientErrorTransport, message);
        if (guard.isNull() || !isCurrentLifecycle(generation, Lifecycle::Stopping)) {
            return;
        }
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

void QSocMcpClient::onMessageFailed(
    quint64 token, const QList<int> &requestIds, const QString &message)
{
    if (lifecycle_ != Lifecycle::Active) {
        return;
    }
    if ((state_ == State::Initializing && token != 0 && token == initializedSendToken_)
        || (state_ == State::Initializing && initializeId_ >= 0 && pending_.contains(initializeId_)
            && requestIds.contains(initializeId_))) {
        onTransportError(message);
        return;
    }
    if (state_ != State::Ready) {
        return;
    }

    const quint64                 generation = lifecycleGeneration_;
    const QPointer<QSocMcpClient> guard(this);
    for (int id : requestIds) {
        auto pendingIt = pending_.find(id);
        if (pendingIt == pending_.end()) {
            continue;
        }
        Pending pending = pendingIt.value();
        pending_.erase(pendingIt);
        if (!pending.timer.isNull()) {
            pending.timer->stop();
            pending.timer->deleteLater();
        }
        emit requestFailed(id, kClientErrorTransport, message);
        if (guard.isNull() || !isCurrentLifecycle(generation, Lifecycle::Active)
            || state_ != State::Ready) {
            return;
        }
    }
}

void QSocMcpClient::onMessageReceived(const nlohmann::json &message)
{
    if (lifecycle_ != Lifecycle::Active
        || (state_ != State::Initializing && state_ != State::Ready)) {
        return;
    }
    const quint64 generation = lifecycleGeneration_;

    if (!message.is_array()) {
        const QPointer<QSocMcpClient> guard(this);
        const auto                    response = handleMessage(message, generation, nullptr);
        if (!response.has_value() || guard.isNull()
            || !isCurrentLifecycle(generation, Lifecycle::Active)
            || (state_ != State::Initializing && state_ != State::Ready)) {
            return;
        }
        writeMessage(*response);
        return;
    }
    if (message.empty()) {
        writeMessage(jsonRpcErrorResponse(nullptr, kJsonRpcInvalidRequest, "Invalid Request"));
        return;
    }

    QSet<int> eligiblePendingIds;
    eligiblePendingIds.reserve(pending_.size());
    for (auto it = pending_.cbegin(); it != pending_.cend(); ++it) {
        eligiblePendingIds.insert(it.key());
    }
    const QPointer<QSocMcpClient> guard(this);
    nlohmann::json                responses = nlohmann::json::array();
    for (const auto &entry : message) {
        if (guard.isNull() || !isCurrentLifecycle(generation, Lifecycle::Active)
            || (state_ != State::Initializing && state_ != State::Ready)) {
            return;
        }
        if (!entry.is_object()) {
            responses.push_back(
                jsonRpcErrorResponse(nullptr, kJsonRpcInvalidRequest, "Invalid Request"));
            continue;
        }
        const auto response = handleMessage(entry, generation, &eligiblePendingIds);
        if (response.has_value()) {
            responses.push_back(*response);
        }
    }
    if (responses.empty() || guard.isNull() || !isCurrentLifecycle(generation, Lifecycle::Active)
        || (state_ != State::Initializing && state_ != State::Ready)) {
        return;
    }
    writeMessage(responses);
}

std::optional<nlohmann::json> QSocMcpClient::handleMessage(
    const nlohmann::json &message, quint64 generation, const QSet<int> *eligiblePendingIds)
{
    if (!message.is_object()) {
        return jsonRpcErrorResponse(nullptr, kJsonRpcInvalidRequest, "Invalid Request");
    }

    if (message.contains("id") && message.contains("method")) {
        const nlohmann::json id = isRequestId(message["id"]) ? message["id"]
                                                             : nlohmann::json(nullptr);
        if (!hasJsonRpcVersion(message) || !isRequestId(message["id"])
            || !message["method"].is_string() || message.contains("result")
            || message.contains("error")
            || (message.contains("params") && !message["params"].is_object()
                && !message["params"].is_array())) {
            return jsonRpcErrorResponse(id, kJsonRpcInvalidRequest, "Invalid Request");
        }

        const std::string method = message["method"].get<std::string>();
        if (method == "ping") {
            return nlohmann::json{
                {"jsonrpc", "2.0"}, {"id", id}, {"result", nlohmann::json::object()}};
        }
        return jsonRpcErrorResponse(id, kJsonRpcMethodNotFound, "Method not found");
    }

    const bool hasId             = message.contains("id");
    const bool hasMethod         = message.contains("method");
    const bool hasResult         = message.contains("result");
    const bool hasError          = message.contains("error");
    const bool responseCandidate = !hasMethod && (hasId || hasResult || hasError);

    if (responseCandidate && hasJsonRpcVersion(message) && hasId && hasResult != hasError) {
        int     errorCode = 0;
        QString errorMessage;
        if (hasError && !readJsonRpcError(message["error"], &errorCode, &errorMessage)) {
            emit requestFailed(-1, kJsonRpcInvalidRequest, QStringLiteral("Unrecognized message"));
            return std::nullopt;
        }
        int id = -1;
        if (!readInteger(message["id"], &id)) {
            return std::nullopt;
        }

        if ((eligiblePendingIds != nullptr && !eligiblePendingIds->contains(id))
            || !pending_.contains(id)) {
            return std::nullopt;
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
                    return std::nullopt;
                }
                emit requestFailed(id, errorCode, errorMessage);
                if (guard.isNull() || !isCurrentLifecycle(generation, Lifecycle::Stopping)) {
                    return std::nullopt;
                }
                stopTransportOrFinish(generation);
                return std::nullopt;
            }
            const auto &result = message["result"];
            if (result.is_object() && result.contains("capabilities")) {
                serverCapabilities_ = result["capabilities"];
            } else {
                serverCapabilities_ = nlohmann::json::object();
            }
            sendInitializedNotification(id);
            return std::nullopt;
        }

        if (hasError) {
            emit requestFailed(id, errorCode, errorMessage);
            return std::nullopt;
        }
        emit responseReceived(id, message["result"]);
        return std::nullopt;
    }

    if (responseCandidate) {
        emit requestFailed(-1, kJsonRpcInvalidRequest, QStringLiteral("Unrecognized message"));
        return std::nullopt;
    }

    if (hasMethod && !hasId && !hasResult && !hasError) {
        if (!hasJsonRpcVersion(message) || !message["method"].is_string()
            || (message.contains("params") && !message["params"].is_object()
                && !message["params"].is_array())) {
            return jsonRpcErrorResponse(nullptr, kJsonRpcInvalidRequest, "Invalid Request");
        }
        const QString  method = QString::fromStdString(message["method"].get<std::string>());
        nlohmann::json params = message.contains("params") ? message["params"]
                                                           : nlohmann::json::object();
        emit           notificationReceived(method, params);
        return std::nullopt;
    }

    return jsonRpcErrorResponse(nullptr, kJsonRpcInvalidRequest, "Invalid Request");
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

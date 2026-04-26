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
    if (state_ == State::Connecting || state_ == State::Initializing || state_ == State::Ready) {
        return;
    }
    setState(State::Connecting);
    transport_->start();
}

void QSocMcpClient::stop()
{
    cancelAllPending(kClientErrorTransport, QStringLiteral("Client stopping"));
    if (transport_ != nullptr) {
        transport_->stop();
    }
    setState(State::Disconnected);
}

int QSocMcpClient::request(const QString &method, const nlohmann::json &params, int timeoutMs)
{
    if (state_ != State::Ready) {
        return -1;
    }

    const int id = allocateId();

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
        connect(timer, &QTimer::timeout, this, [this, id]() {
            if (!pending_.contains(id)) {
                return;
            }
            const QString method = pending_.value(id).method;
            pending_.remove(id);
            emit requestFailed(
                id, kClientErrorTimeout, QStringLiteral("Request timed out: %1").arg(method));
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
    if (transport_ == nullptr) {
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
    setState(State::Initializing);
    sendInitialize();
}

void QSocMcpClient::onTransportClosed()
{
    cancelAllPending(kClientErrorTransport, QStringLiteral("Transport closed"));
    setState(State::Disconnected);
    emit closed();
}

void QSocMcpClient::onTransportError(const QString &message)
{
    cancelAllPending(kClientErrorTransport, message);
    /* During handshake the transport error is fatal: there is no
     * recovery path for an unanswered initialize. Emit closed so the
     * manager can trigger a reconnect with backoff, just like a
     * process crash. After Ready the error is per-request; tool-call
     * code paths surface it via requestFailed and the client stays
     * usable for the next request. */
    if (state_ == State::Connecting || state_ == State::Initializing) {
        setState(State::Disconnected);
        emit closed();
        return;
    }
    setState(State::Failed);
}

void QSocMcpClient::onMessageReceived(const nlohmann::json &message)
{
    if (isJsonRpcResponse(message)) {
        if (!message.contains("id") || !message["id"].is_number_integer()) {
            return;
        }
        const int id = message["id"].get<int>();

        Pending pending;
        if (pending_.contains(id)) {
            pending = pending_.take(id);
            if (!pending.timer.isNull()) {
                pending.timer->stop();
                pending.timer->deleteLater();
            }
        }

        if (id == initializeId_) {
            initializeId_ = -1;
            if (message.contains("error")) {
                setState(State::Failed);
                const auto &err  = message["error"];
                const int   code = err.value("code", kJsonRpcInternalError);
                emit        requestFailed(
                    id, code, QString::fromStdString(err.value("message", "Initialize failed")));
                return;
            }
            const auto &result = message["result"];
            if (result.is_object() && result.contains("capabilities")) {
                serverCapabilities_ = result["capabilities"];
            } else {
                serverCapabilities_ = nlohmann::json::object();
            }
            setState(State::Ready);
            sendInitializedNotification();
            emit ready();
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
        const int id = initializeId_;
        connect(timer, &QTimer::timeout, this, [this, id]() {
            if (!pending_.contains(id)) {
                return;
            }
            pending_.remove(id);
            initializeId_ = -1;
            setState(State::Failed);
            emit requestFailed(id, kClientErrorTimeout, QStringLiteral("Initialize timed out"));
        });
        timer->start(effective);
        pending.timer = timer;
    }
    pending_.insert(initializeId_, pending);

    writeMessage(msg);
}

void QSocMcpClient::sendInitializedNotification()
{
    notify(QStringLiteral("notifications/initialized"));
}

int QSocMcpClient::allocateId()
{
    return nextId_++;
}

void QSocMcpClient::writeMessage(const nlohmann::json &message)
{
    if (transport_ == nullptr) {
        return;
    }
    transport_->sendMessage(message);
    Q_UNUSED(kJsonRpcParseError);
    Q_UNUSED(kClientErrorNotReady);
}

void QSocMcpClient::cancelAllPending(int code, const QString &message)
{
    const auto ids = pending_.keys();
    for (int id : ids) {
        Pending pending = pending_.take(id);
        if (!pending.timer.isNull()) {
            pending.timer->stop();
            pending.timer->deleteLater();
        }
        emit requestFailed(id, code, message);
    }
    initializeId_ = -1;
}

int QSocMcpClient::effectiveTimeoutMs(int requested) const
{
    if (requested >= 0) {
        return requested;
    }
    return config_.requestTimeoutMs;
}

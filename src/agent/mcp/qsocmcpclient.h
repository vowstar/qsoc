// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCMCPCLIENT_H
#define QSOCMCPCLIENT_H

#include "agent/mcp/qsocmcptypes.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>

class QSocMcpTransport;
class QTimer;

/**
 * @brief JSON-RPC client for one MCP server.
 * @details Owns the transport and the protocol layer above it. Drives
 *          the initialize handshake on start, then exposes generic
 *          request() / notify() primitives for callers (the manager,
 *          tool wrappers). Responses and notifications are delivered
 *          via signals so call sites can adapt them to either sync or
 *          async APIs.
 */
class QSocMcpClient : public QObject
{
    Q_OBJECT

public:
    enum class State : std::uint8_t {
        Disconnected,
        Connecting,
        Initializing,
        Ready,
        /** The current lifecycle failed and is being torn down. */
        Failed,
    };
    Q_ENUM(State)

    /**
     * @param config Server configuration; only `name` is used at this layer.
     * @param transport Owned transport instance. Ownership transfers to the
     *                  client (it is reparented).
     */
    QSocMcpClient(McpServerConfig config, QSocMcpTransport *transport, QObject *parent = nullptr);
    ~QSocMcpClient() override;

    const McpServerConfig &config() const;
    QString                name() const;
    State                  state() const;
    const nlohmann::json  &serverCapabilities() const;

    /**
     * @brief Start an idle client lifecycle and initiate the handshake.
     * @details Calls made before the previous closed() signal are ignored.
     */
    void start();

    /**
     * @brief Tear down the underlying transport.
     * @details Repeated calls are ignored; closed() confirms completion.
     */
    void stop();

    /**
     * @brief Send a JSON-RPC request and return its id.
     * @details The response (or failure) is emitted via responseReceived or
     *          requestFailed; the id is the correlation key.
     * @param method JSON-RPC method, e.g. "tools/list".
     * @param params Request parameters; defaults to an empty object.
     * @param timeoutMs Per-request timeout. Negative means no timeout
     *                  beyond the server config's default.
     * @param assignedId Optional destination written before transport send.
     * @return Allocated request id, or -1 if the client is not Ready.
     */
    int request(
        const QString        &method,
        const nlohmann::json &params     = nlohmann::json::object(),
        int                   timeoutMs  = -1,
        int                  *assignedId = nullptr);

    /**
     * @brief Send a JSON-RPC notification (no response expected).
     * @details Notifications sent while the client is not Ready are ignored.
     */
    void notify(const QString &method, const nlohmann::json &params = nlohmann::json::object());

signals:
    /** State machine transition. */
    void stateChanged(QSocMcpClient::State newState);
    /** Initialize handshake and initialized delivery completed. */
    void ready();
    /** A response correlated to a previously issued request id. */
    void responseReceived(int id, const nlohmann::json &result);
    /** A request failed (RPC error, timeout, or transport closed). */
    void requestFailed(int id, int code, const QString &message);
    /** A server-pushed notification (e.g. notifications/tools/list_changed). */
    void notificationReceived(const QString &method, const nlohmann::json &params);
    /** The current lifecycle ended; start() may begin another one. */
    void closed();

private slots:
    void onTransportStarted();
    void onTransportClosed();
    void onTransportError(const QString &message);
    void onMessageSent(quint64 token);
    void onMessageReceived(const nlohmann::json &message);

private:
    enum class Lifecycle : std::uint8_t { Idle, Active, Stopping, Finishing };

    struct Pending
    {
        QString          method;
        QPointer<QTimer> timer;
    };

    void                          setState(State newState);
    void                          sendInitialize();
    void                          sendInitializedNotification(int requestId);
    void                          clearInitializedSend();
    std::optional<nlohmann::json> handleMessage(
        const nlohmann::json &message, quint64 generation, const QSet<int> *eligiblePendingIds);
    int  allocateId();
    void writeMessage(const nlohmann::json &message);
    bool cancelAllPending(int code, const QString &message);
    bool isCurrentLifecycle(quint64 generation, Lifecycle lifecycle) const;
    void stopTransportOrFinish(quint64 generation);
    void finishLifecycle(quint64 generation);
    int  effectiveTimeoutMs(int requested) const;

    McpServerConfig     config_;
    QSocMcpTransport   *transport_    = nullptr;
    State               state_        = State::Disconnected;
    int                 nextId_       = 1;
    int                 initializeId_ = -1;
    nlohmann::json      serverCapabilities_;
    QHash<int, Pending> pending_;
    QPointer<QTimer>    initializedSendTimer_;
    Lifecycle           lifecycle_            = Lifecycle::Idle;
    quint64             lifecycleGeneration_  = 0;
    quint64             initializedSendToken_ = 0;
    int                 initializedRequestId_ = -1;
};

#endif // QSOCMCPCLIENT_H

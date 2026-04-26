// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCMCPCLIENT_H
#define QSOCMCPCLIENT_H

#include "agent/mcp/qsocmcptypes.h"

#include <nlohmann/json.hpp>

#include <cstdint>

#include <QHash>
#include <QObject>
#include <QPointer>
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
     * @brief Start the underlying transport and initiate the handshake.
     */
    void start();

    /**
     * @brief Tear down the underlying transport.
     */
    void stop();

    /**
     * @brief Send a JSON-RPC request and return its id.
     * @details The response (or failure) arrives later via responseReceived
     *          or requestFailed; the id is the correlation key.
     * @param method JSON-RPC method, e.g. "tools/list".
     * @param params Request parameters; defaults to an empty object.
     * @param timeoutMs Per-request timeout. Negative means no timeout
     *                  beyond the server config's default.
     * @return Allocated request id, or -1 if the client is not Ready.
     */
    int request(
        const QString        &method,
        const nlohmann::json &params    = nlohmann::json::object(),
        int                   timeoutMs = -1);

    /**
     * @brief Send a JSON-RPC notification (no response expected).
     */
    void notify(const QString &method, const nlohmann::json &params = nlohmann::json::object());

signals:
    /** State machine transition. */
    void stateChanged(QSocMcpClient::State newState);
    /** Initialize handshake completed and the server is Ready. */
    void ready();
    /** A response correlated to a previously issued request id. */
    void responseReceived(int id, const nlohmann::json &result);
    /** A request failed (RPC error, timeout, or transport closed). */
    void requestFailed(int id, int code, const QString &message);
    /** A server-pushed notification (e.g. notifications/tools/list_changed). */
    void notificationReceived(const QString &method, const nlohmann::json &params);
    /** Transport closed. The client is no longer usable. */
    void closed();

private slots:
    void onTransportStarted();
    void onTransportClosed();
    void onTransportError(const QString &message);
    void onMessageReceived(const nlohmann::json &message);

private:
    struct Pending
    {
        QString          method;
        QPointer<QTimer> timer;
    };

    void setState(State newState);
    void sendInitialize();
    void sendInitializedNotification();
    int  allocateId();
    void writeMessage(const nlohmann::json &message);
    void cancelAllPending(int code, const QString &message);
    int  effectiveTimeoutMs(int requested) const;

    McpServerConfig     config_;
    QSocMcpTransport   *transport_    = nullptr;
    State               state_        = State::Disconnected;
    int                 nextId_       = 1;
    int                 initializeId_ = -1;
    nlohmann::json      serverCapabilities_;
    QHash<int, Pending> pending_;
};

#endif // QSOCMCPCLIENT_H

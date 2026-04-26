// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCMCPTRANSPORT_H
#define QSOCMCPTRANSPORT_H

#include <nlohmann/json.hpp>

#include <cstdint>

#include <QObject>
#include <QString>

/**
 * @brief Abstract transport for MCP JSON-RPC traffic.
 * @details A transport is a bidirectional message pipe. Concrete
 *          subclasses (stdio child process, HTTP/SSE) own the actual
 *          plumbing; the JSON-RPC client treats them all the same way.
 *
 *          Lifecycle: start() moves the transport from Idle to Starting.
 *          When the underlying connection is established the subclass
 *          emits started(); failures emit errorOccurred() and the
 *          transport returns to Idle. sendMessage() is only valid
 *          between started() and stop()/closed().
 */
class QSocMcpTransport : public QObject
{
    Q_OBJECT

public:
    enum class State : std::uint8_t { Idle, Starting, Running, Stopping, Stopped };
    Q_ENUM(State)

    explicit QSocMcpTransport(QObject *parent = nullptr);
    ~QSocMcpTransport() override;

    State state() const;

    /**
     * @brief Begin the underlying connection (spawn process, open socket, ...).
     */
    virtual void start() = 0;

    /**
     * @brief Tear down the underlying connection.
     */
    virtual void stop() = 0;

    /**
     * @brief Send one JSON-RPC message.
     * @details Subclasses are responsible for the wire encoding (e.g. the
     *          stdio implementation prepends a Content-Length header).
     * @param message JSON message to send.
     */
    virtual void sendMessage(const nlohmann::json &message) = 0;

signals:
    /** Connection is up; sendMessage() is valid from now on. */
    void started();
    /** Connection has terminated, gracefully or otherwise. */
    void closed();
    /** A complete JSON message has been parsed off the wire. */
    void messageReceived(const nlohmann::json &message);
    /** Non-recoverable error. The transport is no longer usable. */
    void errorOccurred(const QString &message);

protected:
    void setState(State newState);

private:
    State state_ = State::Idle;
};

#endif // QSOCMCPTRANSPORT_H

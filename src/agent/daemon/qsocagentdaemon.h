// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

/**
 * @file qsocagentdaemon.h
 * @brief Unix-socket front end for the agent runtime library.
 * @details One QSocAgentRuntime per connection. The wire protocol is
 *          length-prefixed JSON: a request object in, a stream of event
 *          objects out, terminated by a reply object carrying the
 *          request's id.
 *
 *          Requests:
 *            {"id":N,"method":"open",   "params":{...QSocAgentRuntimeOptions}}
 *            {"id":N,"method":"turn",   "params":{"input":"..."}}
 *            {"id":N,"method":"command","params":{"input":"..."}}
 *            {"id":N,"method":"abort"}
 *            {"id":N,"method":"status"}
 *            {"id":N,"method":"sessions"}
 *            {"id":N,"method":"open_session","params":{"id":"..."}}
 *            {"id":N,"method":"model","params":{"id":"..."}}
 *            {"id":N,"method":"effort","params":{"level":"..."}}
 *            {"id":N,"method":"plan_mode","params":{"enabled":true}}
 *            {"id":N,"method":"remote","params":{"target":"..."}}
 *            {"id":N,"method":"local"}
 *            {"id":N,"method":"cwd","params":{"path":"..."}}
 *            {"id":N,"method":"project","params":{"path":"..."}}
 *            {"id":N,"method":"shutdown"}
 *
 *          Replies carry {"id":N,"result":...} or {"id":N,"error":"..."}.
 *          Events arrive as {"event":{...QSocAgentRuntimeEvent}}.
 */

#ifndef QSOCAGENTDAEMON_H
#define QSOCAGENTDAEMON_H

#include "agent/runtime/qsocagentruntime.h"

#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QObject>
#include <QString>

#include <memory>

class QSocAgentDaemonConnection;
class QSocAgentSessionProxy;

/**
 * @brief The qsoc-agentd daemon: a QLocalServer hosting agent sessions.
 */
class QSocAgentDaemon : public QObject
{
    Q_OBJECT

public:
    /**
     * @brief Construct the daemon.
     * @param socketPath Unix socket path; empty = default runtime path.
     * @param parent QObject parent.
     */
    explicit QSocAgentDaemon(const QString &socketPath = QString(), QObject *parent = nullptr);
    ~QSocAgentDaemon() override;

    /** Default socket path ($XDG_RUNTIME_DIR/qsoc/agent.sock, temp fallback). */
    static QString defaultSocketPath();

    /** Bind + listen. Returns false with error() set on failure. */
    bool start();

    /** Last error text. */
    [[nodiscard]] QString error() const { return error_; }

    /** Number of live connections. */
    [[nodiscard]] int connectionCount() const;

    /** Stop accepting and close every connection. */
    void shutdown();

    /**
     * @brief Serve one connection in this process, then exit.
     * @details Without it the daemon only accepts and relays: each
     *          connection gets its own session process, so sessions never
     *          share an event loop.
     */
    void setSingleSession(bool single) { singleSession_ = single; }

    /** Exit code of a session process whose client asked the daemon to stop. */
    static constexpr int stopDaemonExitCode = 3;

    /** Shut down and leave the event loop, as asked by a client. */
    void requestStop();

signals:
    /** A connection opened or closed. */
    void connectionCountChanged(int count);

private slots:
    void onNewConnection();

private:
    void removeConnection(QSocAgentDaemonConnection *connection);
    void removeProxy(QSocAgentSessionProxy *proxy);

    QString                            socketPath_;
    QString                            error_;
    QLocalServer                       server_;
    std::unique_ptr<QLockFile>         socketLock_;
    QList<QSocAgentDaemonConnection *> connections_;
    QList<QSocAgentSessionProxy *>     proxies_;
    bool                               singleSession_ = false;
    bool                               stopRequested_ = false;

    friend class QSocAgentDaemonConnection;
    friend class QSocAgentSessionProxy;
};

#endif /* QSOCAGENTDAEMON_H */

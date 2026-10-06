// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

/**
 * @file qsocagentdaemon.h
 * @brief Local-socket front end for agent sessions and SMT tasks.
 * @details Agent requests start a separate session process. The wire protocol is
 *          length-prefixed JSON: a request object in, a stream of event
 *          objects out, terminated by a reply object carrying the
 *          request's id.
 *
 *          Requests:
 *            {"id":N,"method":"open",   "params":{...QSocAgentRuntimeOptions,
 *                                                 "single_query":false}}
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
 *            {"id":N,"method":"tasks"}
 *            {"id":N,"method":"task_tail","params":{"source":"agent","id":"a1",
 *                                                    "offset":0,"format":"history"}}
 *            {"id":N,"method":"task_send","params":{"id":"a1","message":"..."}}
 *            {"id":N,"method":"task_kill","params":{"source":"agent","id":"a1"}}
 *            {"id":N,"method":"shutdown"}
 *            {"id":N,"method":"smt.solve","params":{...}}
 *            {"id":N,"method":"smt.cancel","params":{"request_id":N}}
 *
 *          Replies carry {"id":N,"result":...} or {"id":N,"error":"..."}.
 *          Events arrive as {"event":{...QSocAgentRuntimeEvent}}. A turn the
 *          session starts by itself (scheduled input, background wake)
 *          replies with id 0.
 */

#ifndef QSOCAGENTDAEMON_H
#define QSOCAGENTDAEMON_H

#include "agent/runtime/qsocagentruntime.h"
#include "smt/qsocmemorybudget.h"

#include <QHash>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QObject>
#include <QString>

#include <memory>

class QSocSmtBroker;
class QSocDaemonResources;
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
     * @param socketPath Local endpoint; empty = default runtime path.
     * @param parent QObject parent.
     */
    explicit QSocAgentDaemon(
        const QString           &socketPath   = QString(),
        QObject                 *parent       = nullptr,
        QSocMemoryBudget::Policy memoryPolicy = {false, 512 * 1024 * 1024});
    ~QSocAgentDaemon() override;

    /** Default socket path ($XDG_RUNTIME_DIR/qsoc/agentd.sock, temp fallback). */
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
     * @details The supervisor schedules SMT tasks and starts one isolated
     *          process for each connection that requests an agent session.
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
    void handleNewConnection();

private:
    void removeConnection(QSocAgentDaemonConnection *connection);
    void removeProxy(QSocAgentSessionProxy *proxy);

    QString                              socketPath_;
    QString                              error_;
    QLocalServer                         server_;
    std::unique_ptr<QLockFile>           socketLock_;
    QList<QSocAgentDaemonConnection *>   connections_;
    QList<QSocAgentSessionProxy *>       proxies_;
    std::unique_ptr<QSocSmtBroker>       smtBroker_;
    std::unique_ptr<QSocDaemonResources> resources_;
    QHash<qint64, quint64>               sessionOwners_;
    quint64                              nextOwner_     = 0;
    bool                                 singleSession_ = false;
    bool                                 stopRequested_ = false;

    friend class QSocAgentDaemonConnection;
    friend class QSocAgentSessionProxy;
};

#endif /* QSOCAGENTDAEMON_H */

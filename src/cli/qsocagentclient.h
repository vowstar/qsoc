// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCAGENTCLIENT_H
#define QSOCAGENTCLIENT_H

#include <QJsonObject>
#include <QLocalSocket>
#include <QObject>
#include <QString>

#include "agent/runtime/qsocagentruntimeevent.h"

/**
 * @brief Wire client for the qsoc-agentd daemon.
 * @details Frames are 8 hex digits of payload length + JSON payload, the
 *          same format the daemon writes. Events arrive as
 *          {"event":{...}} frames; request replies as {"id":N,...} frames.
 */
class QSocAgentDaemonClient : public QObject
{
    Q_OBJECT

public:
    explicit QSocAgentDaemonClient(const QString &socketPath, QObject *parent = nullptr);
    ~QSocAgentDaemonClient() override;

    /** Connect + read the greeting. */
    bool connectToDaemon(int timeoutMs = 5000);

    /** Send one request object. */
    void send(const QJsonObject &request);

    /** Monotonic request id source. */
    qint64 nextId();

    /** Last error text. */
    [[nodiscard]] QString error() const { return m_error; }

    /** Daemon version from the greeting. */
    [[nodiscard]] QString daemonVersion() const { return m_daemonVersion; }
    /** Process id of the daemon end, or -1 when the platform cannot tell. */
    [[nodiscard]] qint64 daemonProcessId() const;

    QJsonObject request(const QString &method, const QJsonObject &params = {}, int timeoutMs = 30000);
    bool isConnected() const { return m_socket.state() == QLocalSocket::ConnectedState; }
    void disconnectFromDaemon() { m_socket.disconnectFromServer(); }

signals:
    /** An event frame arrived. */
    void eventReceived(const QSocAgentRuntimeEvent &event);
    /** A reply frame arrived. */
    void replyReceived(const QJsonObject &reply);
    /** The socket dropped. */
    void disconnected();

private slots:
    void onReadyRead();

private:
    QString      m_socketPath;
    QLocalSocket m_socket;
    QByteArray   m_buffer;
    QString      m_error;
    QString      m_daemonVersion;
    qint64       m_requestCounter = 0;
    QJsonObject  m_greeting;
};

#endif /* QSOCAGENTCLIENT_H */

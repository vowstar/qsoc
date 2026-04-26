// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCMCPMANAGER_H
#define QSOCMCPMANAGER_H

#include "agent/mcp/qsocmcptypes.h"

#include <functional>

#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>

class QSocMcpClient;
class QSocMcpTool;
class QSocMcpTransport;
class QSocToolRegistry;
class QTimer;

/**
 * @brief Owns the set of MCP clients configured for an agent session.
 * @details Construction registers one QSocMcpClient per enabled config
 *          entry. Once a client transitions to Ready the manager pulls
 *          tools/list and registers each remote tool as a QSocMcpTool
 *          inside the supplied QSocToolRegistry. The registry is shared
 *          with the rest of the agent, so MCP tools become visible to
 *          the LLM the same way the built-in tools are.
 *
 *          When a client closes unexpectedly, the manager rebuilds the
 *          client (fresh transport + fresh client object) on an
 *          exponential backoff schedule, capped at kMaxReconnectAttempts.
 *          tools/list_changed notifications trigger an immediate refresh
 *          of the registered tools.
 */
class QSocMcpManager : public QObject
{
    Q_OBJECT

public:
    using TransportFactory = std::function<QSocMcpTransport *(const McpServerConfig &)>;

    static constexpr int kMaxReconnectAttempts    = 3;
    static constexpr int kReconnectInitialDelayMs = 1000;
    static constexpr int kReconnectMaxDelayMs     = 30000;

    explicit QSocMcpManager(
        const QList<McpServerConfig> &configs,
        QSocToolRegistry             *toolRegistry = nullptr,
        QObject                      *parent       = nullptr);
    ~QSocMcpManager() override;

    /**
     * @brief Override the transport factory before constructing servers.
     * @details Must be called before startAll() / before any client is
     *          built. Used by tests to inject in-process fakes.
     */
    void setTransportFactory(TransportFactory factory);

    /**
     * @brief Override the reconnect backoff base + cap (test hook).
     */
    void setReconnectDelays(int initialMs, int maxMs);

    /**
     * @brief Drive every owned client's start() in one shot.
     */
    void startAll();

    qsizetype            clientCount() const;
    QStringList          serverNames() const;
    QSocMcpClient       *findClient(const QString &name) const;
    QList<QSocMcpTool *> toolsForClient(const QString &name) const;
    qsizetype            totalToolCount() const;
    int                  reconnectAttempts(const QString &name) const;
    bool                 hasGivenUp(const QString &name) const;

    /**
     * @brief Reset state and rebuild the named server immediately.
     * @details Used by `/mcp reconnect`. Clears the reconnect counter
     *          and the given-up flag, so a server that has exhausted
     *          its automatic retries can still be revived on demand.
     * @return true if the server name is known.
     */
    bool reconnectServer(const QString &name);

signals:
    /** Tools have been registered (or refreshed) for one server. */
    void toolsRegistered(const QString &serverName, qsizetype count);
    /** A reconnect attempt has been scheduled. */
    void reconnectScheduled(const QString &serverName, int attempt, int delayMs);
    /** Reconnect attempts exhausted; the server is dropped. */
    void serverGaveUp(const QString &serverName);

private slots:
    void onClientReady();
    void onClientResponse(int id, const nlohmann::json &result);
    void onClientNotification(const QString &method, const nlohmann::json &params);
    void onClientClosed();

private:
    struct ServerState
    {
        McpServerConfig      config;
        QSocMcpClient       *client            = nullptr;
        int                  pendingListId     = -1;
        int                  reconnectAttempts = 0;
        bool                 givenUp           = false;
        QPointer<QTimer>     reconnectTimer;
        QList<QSocMcpTool *> registeredTools;
    };

    void           buildServer(const McpServerConfig &cfg);
    void           rebuildServer(const QString &name);
    void           wireClientSignals(QSocMcpClient *client);
    void           requestToolsList(QSocMcpClient *client);
    void           registerToolsFromResult(QSocMcpClient *client, const nlohmann::json &result);
    void           unregisterToolsFor(const QString &name);
    QSocMcpClient *senderClient();

    int     backoffDelayMs(int attempt) const;
    QString nameForClient(const QSocMcpClient *client) const;

    QSocToolRegistry           *toolRegistry_ = nullptr;
    TransportFactory            factory_;
    int                         reconnectInitialDelayMs_ = kReconnectInitialDelayMs;
    int                         reconnectMaxDelayMs_     = kReconnectMaxDelayMs;
    QStringList                 order_;
    QHash<QString, ServerState> servers_;
};

#endif // QSOCMCPMANAGER_H

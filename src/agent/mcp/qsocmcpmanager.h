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
     * @brief Set the transport factory.
     * @details A non-empty factory cancels pending reconnects, clears failure
     *          state, and rebuilds stopped clients for the next startAll()
     *          when transport creation succeeds.
     *          An empty factory selects the default for future rebuilds.
     */
    void setTransportFactory(TransportFactory factory);

    /**
     * @brief Override the reconnect backoff base + cap (test hook).
     */
    void setReconnectDelays(int initialMs, int maxMs);

    /**
     * @brief Drive every owned client's start() in one shot.
     * @details Cancels each current client's pending reconnect before start.
     */
    void startAll();

    qsizetype   clientCount() const;
    QStringList serverNames() const;
    /** Borrowed pointer invalidated by rebuild, drop, or manager destruction.
     *  Use QPointer when retaining it across events. */
    QSocMcpClient *findClient(const QString &name) const;
    /** Borrowed pointers invalidated by refresh, disconnect, or rebuild.
     *  Use QPointer when retaining them across events. */
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
        McpServerConfig              config;
        QSocMcpClient               *client            = nullptr;
        int                          pendingListId     = -1;
        int                          reconnectAttempts = 0;
        bool                         givenUp           = false;
        QPointer<QTimer>             reconnectTimer;
        QPointer<QSocMcpClient>      reconnectClient;
        quint64                      replacementRevision = 0;
        QList<QPointer<QSocMcpTool>> registeredTools;
    };

    void           buildServer(const McpServerConfig &cfg, quint64 revision);
    void           rebuildServer(const QString &name, bool start);
    void           cancelReconnect(ServerState &state);
    void           retireClient(ServerState &state);
    void           wireClientSignals(QSocMcpClient *client);
    void           requestToolsList(QSocMcpClient *client);
    void           registerToolsFromResult(QSocMcpClient *client, const nlohmann::json &result);
    void           unregisterToolsFor(const QString &name);
    QSocMcpClient *senderClient();

    int     backoffDelayMs(int attempt) const;
    QString nameForClient(const QSocMcpClient *client) const;

    QPointer<QSocToolRegistry>  toolRegistry_;
    TransportFactory            factory_;
    int                         reconnectInitialDelayMs_ = kReconnectInitialDelayMs;
    int                         reconnectMaxDelayMs_     = kReconnectMaxDelayMs;
    quint64                     factoryRevision_         = 0;
    QStringList                 order_;
    QHash<QString, ServerState> servers_;
};

#endif // QSOCMCPMANAGER_H

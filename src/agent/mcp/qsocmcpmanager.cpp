// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/mcp/qsocmcpmanager.h"

#include "agent/mcp/qsocmcpclient.h"
#include "agent/mcp/qsocmcphttp.h"
#include "agent/mcp/qsocmcpstdio.h"
#include "agent/mcp/qsocmcptool.h"
#include "agent/mcp/qsocmcptransport.h"
#include "agent/qsoctool.h"
#include "common/qsocconsole.h"

#include <algorithm>
#include <utility>

#include <QTimer>

namespace {

QSocMcpTransport *defaultTransportFactory(const McpServerConfig &cfg)
{
    if (cfg.type == QSocMcp::kTransportStdio) {
        return new QSocMcpStdioTransport(cfg);
    }
    if (cfg.type == QSocMcp::kTransportHttp) {
        return new QSocMcpHttpTransport(cfg);
    }
    QSocConsole::warn() << "MCP transport not yet supported:" << cfg.type << "(server:" << cfg.name
                        << ")";
    return nullptr;
}

McpToolDescriptor descriptorFromJson(const QString &serverName, const nlohmann::json &tool)
{
    McpToolDescriptor desc;
    desc.serverName  = serverName;
    desc.toolName    = QString::fromStdString(tool.value("name", std::string()));
    desc.description = QString::fromStdString(tool.value("description", std::string()));
    if (tool.contains("inputSchema") && tool["inputSchema"].is_object()) {
        desc.inputSchema = tool["inputSchema"];
    } else {
        desc.inputSchema         = nlohmann::json::object();
        desc.inputSchema["type"] = "object";
    }
    if (tool.contains("annotations") && tool["annotations"].is_object()) {
        const auto &ann  = tool["annotations"];
        desc.readOnly    = ann.value("readOnlyHint", false);
        desc.destructive = ann.value("destructiveHint", false);
    }
    return desc;
}

} // namespace

QSocMcpManager::QSocMcpManager(
    const QList<McpServerConfig> &configs, QSocToolRegistry *toolRegistry, QObject *parent)
    : QObject(parent)
    , toolRegistry_(toolRegistry)
    , factory_(&defaultTransportFactory)
{
    for (const auto &cfg : configs) {
        if (!cfg.enabled || !cfg.isValid()) {
            continue;
        }
        if (servers_.contains(cfg.name)) {
            QSocConsole::warn() << "Duplicate MCP server name, ignoring later entry:" << cfg.name;
            continue;
        }
        order_ << cfg.name;
        ServerState state;
        state.config = cfg;
        servers_.insert(cfg.name, state);
        auto &stored = servers_[cfg.name];
        buildServer(stored.config, ++stored.replacementRevision);
    }
}

QSocMcpManager::~QSocMcpManager()
{
    for (const QString &name : std::as_const(order_)) {
        unregisterToolsFor(name);
    }
}

void QSocMcpManager::setTransportFactory(TransportFactory factory)
{
    const quint64 revision = ++factoryRevision_;
    if (!factory) {
        factory_ = &defaultTransportFactory;
        return;
    }
    factory_                = std::move(factory);
    const QStringList names = order_;
    for (const QString &name : names) {
        if (!servers_.contains(name)) {
            continue;
        }
        auto &state             = servers_[name];
        state.reconnectAttempts = 0;
        state.givenUp           = false;
        QPointer<QSocMcpManager> guard(this);
        rebuildServer(name, false);
        if (guard.isNull()) {
            return;
        }
        if (factoryRevision_ != revision) {
            return;
        }
    }
}

void QSocMcpManager::startAll()
{
    const quint64                  factoryRevision = factoryRevision_;
    const QStringList              names           = order_;
    const QPointer<QSocMcpManager> guard(this);
    for (const QString &name : names) {
        if (!servers_.contains(name)) {
            continue;
        }
        auto &state = servers_[name];
        cancelReconnect(state);
        if (state.client != nullptr) {
            state.client->start();
            if (guard.isNull()) {
                return;
            }
            if (factoryRevision_ != factoryRevision) {
                return;
            }
        }
    }
}

qsizetype QSocMcpManager::clientCount() const
{
    qsizetype count = 0;
    for (const auto &state : servers_) {
        if (state.client != nullptr) {
            ++count;
        }
    }
    return count;
}

QStringList QSocMcpManager::serverNames() const
{
    return order_;
}

QSocMcpClient *QSocMcpManager::findClient(const QString &name) const
{
    if (!servers_.contains(name)) {
        return nullptr;
    }
    return servers_.value(name).client;
}

QList<QSocMcpTool *> QSocMcpManager::toolsForClient(const QString &name) const
{
    if (!servers_.contains(name)) {
        return {};
    }
    QList<QSocMcpTool *> tools;
    for (const auto &tool : servers_.value(name).registeredTools) {
        if (!tool.isNull()) {
            tools.append(tool.data());
        }
    }
    return tools;
}

qsizetype QSocMcpManager::totalToolCount() const
{
    qsizetype total = 0;
    for (const auto &state : servers_) {
        for (const auto &tool : state.registeredTools) {
            if (!tool.isNull()) {
                ++total;
            }
        }
    }
    return total;
}

int QSocMcpManager::reconnectAttempts(const QString &name) const
{
    if (!servers_.contains(name)) {
        return 0;
    }
    return servers_.value(name).reconnectAttempts;
}

bool QSocMcpManager::hasGivenUp(const QString &name) const
{
    if (!servers_.contains(name)) {
        return false;
    }
    return servers_.value(name).givenUp;
}

bool QSocMcpManager::reconnectServer(const QString &name)
{
    if (!servers_.contains(name)) {
        return false;
    }
    auto &state             = servers_[name];
    state.reconnectAttempts = 0;
    state.givenUp           = false;
    cancelReconnect(state);
    rebuildServer(name, true);
    return true;
}

void QSocMcpManager::onClientReady()
{
    auto *client = senderClient();
    if (client == nullptr) {
        return;
    }
    const QString name = nameForClient(client);
    if (name.isEmpty()) {
        return;
    }
    auto &state             = servers_[name];
    state.reconnectAttempts = 0;
    cancelReconnect(state);
    requestToolsList(client);
}

void QSocMcpManager::onClientResponse(int id, const nlohmann::json &result)
{
    auto *client = senderClient();
    if (client == nullptr) {
        return;
    }
    const QString name = nameForClient(client);
    if (name.isEmpty()) {
        return;
    }
    auto &state = servers_[name];
    if (id != state.pendingListId) {
        return;
    }
    state.pendingListId = -1;
    registerToolsFromResult(client, result);
}

void QSocMcpManager::onClientNotification(const QString &method, const nlohmann::json &params)
{
    Q_UNUSED(params);
    if (method != QStringLiteral("notifications/tools/list_changed")) {
        return;
    }
    auto *client = senderClient();
    if (client == nullptr) {
        return;
    }
    requestToolsList(client);
}

void QSocMcpManager::onClientClosed()
{
    auto *client = senderClient();
    if (client == nullptr) {
        return;
    }
    const QString name = nameForClient(client);
    if (name.isEmpty()) {
        return;
    }

    unregisterToolsFor(name);

    auto &state         = servers_[name];
    state.pendingListId = -1;
    if (state.givenUp) {
        return;
    }
    if (!state.reconnectTimer.isNull() && state.reconnectTimer->isActive()) {
        if (state.reconnectClient == client) {
            return;
        }
        cancelReconnect(state);
    }
    if (state.reconnectAttempts >= kMaxReconnectAttempts) {
        state.givenUp = true;
        cancelReconnect(state);
        const quint64                  revision = ++state.replacementRevision;
        const QPointer<QSocMcpManager> guard(this);
        retireClient(state);
        if (guard.isNull()) {
            return;
        }
        if (servers_[name].replacementRevision != revision) {
            return;
        }
        emit serverGaveUp(name);
        return;
    }

    state.reconnectAttempts++;
    const int delay = backoffDelayMs(state.reconnectAttempts);

    if (state.reconnectTimer.isNull()) {
        auto *timer = new QTimer(this);
        timer->setSingleShot(true);
        connect(timer, &QTimer::timeout, this, [this, name]() {
            auto server = servers_.find(name);
            if (server == servers_.end()) {
                return;
            }
            auto                         &state    = server.value();
            const QPointer<QSocMcpClient> expected = state.reconnectClient;
            state.reconnectClient                  = nullptr;
            if (expected.isNull() || state.client != expected.data()
                || expected->state() != QSocMcpClient::State::Disconnected) {
                return;
            }
            rebuildServer(name, true);
        });
        state.reconnectTimer = timer;
    }
    state.reconnectClient = client;
    state.reconnectTimer->start(delay);

    emit reconnectScheduled(name, state.reconnectAttempts, delay);
}

void QSocMcpManager::buildServer(const McpServerConfig &cfg, quint64 revision)
{
    const McpServerConfig config = cfg;
    auto                  server = servers_.find(config.name);
    if (server == servers_.end() || server->replacementRevision != revision
        || server->client != nullptr || server->givenUp) {
        return;
    }

    const TransportFactory           factory = factory_;
    const QPointer<QSocMcpManager>   guard(this);
    QSocMcpTransport                *transport = factory(config);
    const QPointer<QSocMcpTransport> transportGuard(transport);
    if (guard.isNull()) {
        if (!transportGuard.isNull()) {
            delete transportGuard.data();
        }
        return;
    }

    server = servers_.find(config.name);
    if (server == servers_.end() || server->replacementRevision != revision
        || server->client != nullptr || server->givenUp) {
        if (!transportGuard.isNull()) {
            delete transportGuard.data();
        }
        return;
    }
    if (transport == nullptr) {
        server->givenUp = true;
        return;
    }
    auto *client    = new QSocMcpClient(config, transport, this);
    server->client  = client;
    server->givenUp = false;
    wireClientSignals(client);
}

void QSocMcpManager::rebuildServer(const QString &name, bool start)
{
    auto server = servers_.find(name);
    if (server == servers_.end() || server->givenUp) {
        return;
    }
    const quint64 revision = ++server->replacementRevision;
    cancelReconnect(server.value());

    const QPointer<QSocMcpManager> guard(this);
    unregisterToolsFor(name);
    if (guard.isNull()) {
        return;
    }
    server = servers_.find(name);
    if (server == servers_.end() || server->replacementRevision != revision) {
        return;
    }

    retireClient(server.value());
    if (guard.isNull()) {
        return;
    }
    server = servers_.find(name);
    if (server == servers_.end() || server->replacementRevision != revision
        || server->client != nullptr || server->givenUp) {
        return;
    }

    const McpServerConfig config = server->config;
    buildServer(config, revision);
    if (guard.isNull()) {
        return;
    }
    server = servers_.find(name);
    if (server == servers_.end() || server->replacementRevision != revision) {
        return;
    }
    QPointer<QSocMcpClient> client = server->client;
    if (start && !client.isNull()) {
        client->start();
    }
}

void QSocMcpManager::cancelReconnect(ServerState &state)
{
    if (!state.reconnectTimer.isNull()) {
        state.reconnectTimer->stop();
    }
    state.reconnectClient = nullptr;
}

void QSocMcpManager::retireClient(ServerState &state)
{
    QPointer<QSocMcpClient> client = state.client;
    state.client                   = nullptr;
    state.pendingListId            = -1;
    if (client.isNull()) {
        return;
    }
    client->disconnect(this);
    client->stop();
    if (!client.isNull()) {
        client->deleteLater();
    }
}

void QSocMcpManager::wireClientSignals(QSocMcpClient *client)
{
    connect(client, &QSocMcpClient::ready, this, &QSocMcpManager::onClientReady);
    connect(client, &QSocMcpClient::responseReceived, this, &QSocMcpManager::onClientResponse);
    connect(client, &QSocMcpClient::notificationReceived, this, &QSocMcpManager::onClientNotification);
    connect(client, &QSocMcpClient::closed, this, &QSocMcpManager::onClientClosed);
}

void QSocMcpManager::requestToolsList(QSocMcpClient *client)
{
    const QString name = nameForClient(client);
    if (name.isEmpty()) {
        return;
    }
    int &pendingListId = servers_[name].pendingListId;
    if (client->request(QStringLiteral("tools/list"), nlohmann::json::object(), -1, &pendingListId)
        < 0) {
        pendingListId = -1;
        return;
    }
}

void QSocMcpManager::registerToolsFromResult(QSocMcpClient *client, const nlohmann::json &result)
{
    const QString name = nameForClient(client);
    if (name.isEmpty()) {
        return;
    }
    unregisterToolsFor(name);

    if (toolRegistry_ == nullptr) {
        return;
    }
    if (!result.is_object() || !result.contains("tools") || !result["tools"].is_array()) {
        return;
    }

    auto     &state      = servers_[name];
    qsizetype registered = 0;
    for (const auto &t : result["tools"]) {
        if (!t.is_object()) {
            continue;
        }
        McpToolDescriptor desc = descriptorFromJson(name, t);
        if (desc.toolName.isEmpty()) {
            continue;
        }
        auto *tool = new QSocMcpTool(client, desc, this);
        toolRegistry_->registerTool(tool);
        state.registeredTools.append(tool);
        ++registered;
    }
    emit toolsRegistered(name, registered);
}

void QSocMcpManager::unregisterToolsFor(const QString &name)
{
    if (!servers_.contains(name)) {
        return;
    }
    auto &state = servers_[name];
    for (const auto &tool : state.registeredTools) {
        if (!tool.isNull()) {
            if (!toolRegistry_.isNull()) {
                toolRegistry_->unregisterTool(tool.data());
            }
            tool->retire();
        }
    }
    state.registeredTools.clear();
}

QSocMcpClient *QSocMcpManager::senderClient()
{
    return qobject_cast<QSocMcpClient *>(sender());
}

int QSocMcpManager::backoffDelayMs(int attempt) const
{
    if (attempt < 1) {
        return reconnectInitialDelayMs_;
    }
    /* 1s, 2s, 4s, ..., capped. */
    long long delay = static_cast<long long>(reconnectInitialDelayMs_) << (attempt - 1);
    return static_cast<int>(
        std::min<long long>(delay, static_cast<long long>(reconnectMaxDelayMs_)));
}

void QSocMcpManager::setReconnectDelays(int initialMs, int maxMs)
{
    if (initialMs > 0) {
        reconnectInitialDelayMs_ = initialMs;
    }
    if (maxMs > 0) {
        reconnectMaxDelayMs_ = maxMs;
    }
}

QString QSocMcpManager::nameForClient(const QSocMcpClient *client) const
{
    if (client == nullptr) {
        return {};
    }
    for (auto it = servers_.constBegin(); it != servers_.constEnd(); ++it) {
        if (it.value().client == client) {
            return it.key();
        }
    }
    return {};
}

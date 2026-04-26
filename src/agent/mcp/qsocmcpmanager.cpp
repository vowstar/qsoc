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
        buildServer(cfg);
    }
}

QSocMcpManager::~QSocMcpManager() = default;

void QSocMcpManager::setTransportFactory(TransportFactory factory)
{
    if (!factory) {
        factory_ = &defaultTransportFactory;
        return;
    }
    factory_ = std::move(factory);
    /* Rebuild all servers with the new factory so tests can swap before
     * driving any traffic. */
    const QStringList names = order_;
    for (const QString &name : names) {
        if (!servers_.contains(name)) {
            continue;
        }
        auto &state = servers_[name];
        if (state.client != nullptr) {
            state.client->deleteLater();
            state.client = nullptr;
        }
        unregisterToolsFor(name);
        buildServer(state.config);
    }
}

void QSocMcpManager::startAll()
{
    for (const QString &name : order_) {
        if (servers_.contains(name) && servers_[name].client != nullptr) {
            servers_[name].client->start();
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
    return servers_.value(name).registeredTools;
}

qsizetype QSocMcpManager::totalToolCount() const
{
    qsizetype total = 0;
    for (const auto &state : servers_) {
        total += state.registeredTools.size();
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
    if (!state.reconnectTimer.isNull()) {
        state.reconnectTimer->stop();
    }
    rebuildServer(name);
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
    servers_[name].reconnectAttempts = 0;
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

    auto &state = servers_[name];
    if (state.givenUp) {
        return;
    }
    if (state.reconnectAttempts >= kMaxReconnectAttempts) {
        state.givenUp = true;
        if (state.client != nullptr) {
            state.client->deleteLater();
            state.client = nullptr;
        }
        emit serverGaveUp(name);
        return;
    }

    state.reconnectAttempts++;
    const int delay = backoffDelayMs(state.reconnectAttempts);

    auto *timer = new QTimer(this);
    timer->setSingleShot(true);
    state.reconnectTimer = timer;
    connect(timer, &QTimer::timeout, this, [this, name]() { rebuildServer(name); });
    timer->start(delay);

    emit reconnectScheduled(name, state.reconnectAttempts, delay);
}

void QSocMcpManager::buildServer(const McpServerConfig &cfg)
{
    if (!servers_.contains(cfg.name)) {
        return;
    }
    QSocMcpTransport *transport = factory_(cfg);
    if (transport == nullptr) {
        servers_[cfg.name].givenUp = true;
        return;
    }
    auto *client              = new QSocMcpClient(cfg, transport, this);
    servers_[cfg.name].client = client;
    wireClientSignals(client);
}

void QSocMcpManager::rebuildServer(const QString &name)
{
    if (!servers_.contains(name)) {
        return;
    }
    auto &state = servers_[name];
    if (state.givenUp) {
        return;
    }
    if (state.client != nullptr) {
        state.client->disconnect(this);
        state.client->deleteLater();
        state.client = nullptr;
    }
    buildServer(state.config);
    if (state.client != nullptr) {
        state.client->start();
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
    const int requestId = client->request(QStringLiteral("tools/list"));
    if (requestId < 0) {
        return;
    }
    servers_[name].pendingListId = requestId;
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
    /* QSocToolRegistry has no unregister API today; mark wrappers for
     * deletion and drop our pointer so callers cannot dispatch to them.
     * The registry retains a stale entry until a refresh resolves the
     * same name with a new wrapper. */
    for (auto *tool : state.registeredTools) {
        if (tool != nullptr) {
            tool->deleteLater();
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

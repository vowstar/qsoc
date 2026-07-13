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

#include <QSet>
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

struct ParsedToolCatalog
{
    QList<McpToolDescriptor> descriptors;
    qsizetype                ignoredEntries  = 0;
    qsizetype                sanitizedFields = 0;
};

QString jsonString(const nlohmann::json &value)
{
    const auto &text = value.get_ref<const nlohmann::json::string_t &>();
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

bool hasValidInputSchemaShape(const nlohmann::json &schema)
{
    if (!schema.is_object()) {
        return false;
    }

    const auto type = schema.find("type");
    if (type != schema.end()
        && (!type->is_string() || type->get_ref<const nlohmann::json::string_t &>() != "object")) {
        return false;
    }
    const auto properties = schema.find("properties");
    if (properties != schema.end() && !properties->is_object()) {
        return false;
    }
    const auto required = schema.find("required");
    if (required == schema.end()) {
        return true;
    }
    if (!required->is_array()) {
        return false;
    }
    return std::all_of(required->begin(), required->end(), [](const nlohmann::json &item) {
        return item.is_string();
    });
}

nlohmann::json inputSchemaFromJson(const nlohmann::json &tool, qsizetype *sanitizedFields)
{
    const auto schema = tool.find("inputSchema");
    if (schema == tool.end() || !hasValidInputSchemaShape(*schema)) {
        ++*sanitizedFields;
        return {{"type", "object"}};
    }

    nlohmann::json result = *schema;
    if (!result.contains("type")) {
        result["type"] = "object";
        ++*sanitizedFields;
    }
    return result;
}

bool annotationBool(
    const nlohmann::json &annotations,
    const char           *name,
    bool                  defaultValue,
    qsizetype            *sanitizedFields)
{
    const auto value = annotations.find(name);
    if (value == annotations.end()) {
        return defaultValue;
    }
    if (!value->is_boolean()) {
        ++*sanitizedFields;
        return defaultValue;
    }
    return value->get<bool>();
}

bool descriptorFromJson(
    const QString        &serverName,
    const nlohmann::json &tool,
    McpToolDescriptor    *descriptor,
    qsizetype            *sanitizedFields)
{
    const auto name = tool.find("name");
    if (name == tool.end() || !name->is_string()) {
        return false;
    }

    McpToolDescriptor result;
    result.serverName = serverName;
    result.toolName   = jsonString(*name);
    if (result.toolName.isEmpty() || QSocMcp::normalizeName(result.toolName).isEmpty()) {
        return false;
    }

    const auto description = tool.find("description");
    if (description != tool.end()) {
        if (description->is_string()) {
            result.description = jsonString(*description);
        } else {
            ++*sanitizedFields;
        }
    }
    result.inputSchema = inputSchemaFromJson(tool, sanitizedFields);

    const auto annotations = tool.find("annotations");
    if (annotations != tool.end()) {
        if (annotations->is_object()) {
            result.readOnly
                = annotationBool(*annotations, "readOnlyHint", result.readOnly, sanitizedFields);
            result.destructive = annotationBool(
                *annotations, "destructiveHint", result.destructive, sanitizedFields);
        } else {
            ++*sanitizedFields;
        }
    }

    *descriptor = std::move(result);
    return true;
}

ParsedToolCatalog parseToolCatalog(const QString &serverName, const nlohmann::json &result)
{
    struct Candidate
    {
        McpToolDescriptor descriptor;
        QString           publicName;
    };

    ParsedToolCatalog       catalog;
    QList<Candidate>        candidates;
    QHash<QString, QString> firstRawNames;
    QSet<QString>           ambiguousNames;
    for (const auto &tool : result["tools"]) {
        if (!tool.is_object()) {
            ++catalog.ignoredEntries;
            continue;
        }

        McpToolDescriptor descriptor;
        if (!descriptorFromJson(serverName, tool, &descriptor, &catalog.sanitizedFields)) {
            ++catalog.ignoredEntries;
            continue;
        }

        const QString publicName = QSocMcp::buildToolName(serverName, descriptor.toolName);
        const auto    firstName  = firstRawNames.constFind(publicName);
        if (firstName == firstRawNames.constEnd()) {
            firstRawNames.insert(publicName, descriptor.toolName);
            candidates.append({std::move(descriptor), publicName});
            continue;
        }
        ++catalog.ignoredEntries;
        if (firstName.value() != descriptor.toolName) {
            ambiguousNames.insert(publicName);
        }
    }

    for (Candidate &candidate : candidates) {
        if (ambiguousNames.contains(candidate.publicName)) {
            ++catalog.ignoredEntries;
            continue;
        }
        catalog.descriptors.append(std::move(candidate.descriptor));
    }
    return catalog;
}

bool isToolListResult(const nlohmann::json &result)
{
    return result.is_object() && result.contains("tools") && result["tools"].is_array();
}

QString toolCatalogWarning(
    const QString &serverName, qsizetype ignoredEntries, qsizetype sanitizedFields, bool published)
{
    if (ignoredEntries == 0 && sanitizedFields == 0) {
        return {};
    }
    QString warning = QStringLiteral(
                          "MCP tool catalog for '%1' contains invalid or conflicting data: "
                          "ignored entries %2, sanitized fields %3")
                          .arg(serverName)
                          .arg(ignoredEntries)
                          .arg(sanitizedFields);
    if (!published) {
        warning += QStringLiteral("; catalog unchanged");
    }
    return warning;
}

} // namespace

QSocMcpManager::QSocMcpManager(
    const QList<McpServerConfig> &configs, QSocToolRegistry *toolRegistry, QObject *parent)
    : QObject(parent)
    , toolRegistry_(toolRegistry)
    , factory_(&defaultTransportFactory)
{
    QSet<QString> serverNamespaces;
    for (const auto &cfg : configs) {
        if (!cfg.enabled || !cfg.isValid()) {
            continue;
        }
        if (servers_.contains(cfg.name)) {
            QSocConsole::warn() << "Duplicate MCP server name, ignoring later entry:" << cfg.name;
            continue;
        }
        const QString serverNamespace = QSocMcp::normalizeName(cfg.name);
        if (serverNamespace.isEmpty()) {
            QSocConsole::warn()
                << "MCP server namespace is empty after normalization; ignoring entry";
            continue;
        }
        if (serverNamespaces.contains(serverNamespace)) {
            QSocConsole::warn() << "MCP server namespace collision, ignoring later entry:"
                                << cfg.name;
            continue;
        }
        serverNamespaces.insert(serverNamespace);
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
    auto &state = servers_[name];
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
    if (state.pendingListId < 0 || id != state.pendingListId) {
        return;
    }
    if (!isToolListResult(result)) {
        handleToolListFailure(client, id, QStringLiteral("Invalid tools/list response"));
        return;
    }
    if (state.toolListDirty) {
        finishToolsListLater(client, id);
        return;
    }

    ParsedToolCatalog catalog        = parseToolCatalog(name, result);
    const bool        publishCatalog = result["tools"].empty() || !catalog.descriptors.isEmpty();
    if (!toolRegistry_.isNull()) {
        QSet<QSocTool *> ownedTools;
        for (const auto &tool : std::as_const(state.registeredTools)) {
            if (!tool.isNull()) {
                ownedTools.insert(tool.data());
            }
        }

        QList<McpToolDescriptor> available;
        available.reserve(catalog.descriptors.size());
        for (McpToolDescriptor &descriptor : catalog.descriptors) {
            QSocTool *existing = toolRegistry_->getTool(
                QSocMcp::buildToolName(name, descriptor.toolName));
            if (existing != nullptr && !ownedTools.contains(existing)) {
                ++catalog.ignoredEntries;
                continue;
            }
            available.append(std::move(descriptor));
        }
        catalog.descriptors = std::move(available);
    }

    const QString catalogWarning
        = toolCatalogWarning(name, catalog.ignoredEntries, catalog.sanitizedFields, publishCatalog);
    state.reconnectAttempts = 0;
    state.hasToolCatalog    = true;

    const QPointer<QSocMcpManager> managerGuard(this);
    const QPointer<QSocMcpClient>  clientGuard(client);
    if (publishCatalog) {
        registerTools(client, catalog.descriptors);
    }
    if (managerGuard.isNull() || clientGuard.isNull()) {
        return;
    }
    auto server = servers_.find(name);
    if (server == servers_.end() || server->client != clientGuard.data()
        || server->pendingListId != id || clientGuard->state() != QSocMcpClient::State::Ready) {
        return;
    }
    finishToolsListLater(clientGuard.data(), id, {}, catalogWarning);
}

void QSocMcpManager::onClientRequestFailed(int id, int code, const QString &message)
{
    auto *client = senderClient();
    if (client == nullptr) {
        return;
    }
    handleToolListFailure(client, id, QStringLiteral("[%1] %2").arg(code).arg(message));
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

    auto &state          = servers_[name];
    state.pendingListId  = -1;
    state.hasToolCatalog = false;
    state.toolListDirty  = false;
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
    auto *client                    = new QSocMcpClient(config, transport, this);
    server->client                  = client;
    server->givenUp                 = false;
    server->toolCatalogWarningShown = false;
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
    state.hasToolCatalog           = false;
    state.toolListDirty            = false;
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
    connect(client, &QSocMcpClient::requestFailed, this, &QSocMcpManager::onClientRequestFailed);
    connect(client, &QSocMcpClient::notificationReceived, this, &QSocMcpManager::onClientNotification);
    connect(client, &QSocMcpClient::closed, this, &QSocMcpManager::onClientClosed);
}

void QSocMcpManager::requestToolsList(QSocMcpClient *client)
{
    const QString name = nameForClient(client);
    if (name.isEmpty()) {
        return;
    }
    auto &state = servers_[name];
    if (state.pendingListId >= 0) {
        state.toolListDirty = true;
        return;
    }
    state.toolListDirty = false;
    client->request(QStringLiteral("tools/list"), nlohmann::json::object(), -1, &state.pendingListId);
}

void QSocMcpManager::finishToolsListLater(
    QSocMcpClient *client, int id, QString failureMessage, QString catalogWarning)
{
    const QString name = nameForClient(client);
    if (name.isEmpty()) {
        return;
    }
    const QPointer<QSocMcpClient> expected(client);
    QMetaObject::invokeMethod(
        this,
        [this,
         name,
         expected,
         id,
         failureMessage = std::move(failureMessage),
         catalogWarning = std::move(catalogWarning)]() {
            auto server = servers_.find(name);
            if (expected.isNull() || server == servers_.end() || server->client != expected.data()
                || server->pendingListId != id
                || expected->state() != QSocMcpClient::State::Ready) {
                return;
            }

            auto &state         = server.value();
            state.pendingListId = -1;
            if (!catalogWarning.isEmpty() && !state.toolCatalogWarningShown) {
                state.toolCatalogWarningShown = true;
                QSocConsole::warn() << catalogWarning;
            }
            if (state.toolListDirty) {
                requestToolsList(expected.data());
                return;
            }
            if (!failureMessage.isEmpty()) {
                QSocConsole::warn()
                    << QStringLiteral("MCP tool refresh failed for '%1': %2; keeping current tools")
                           .arg(name, failureMessage);
                return;
            }
        },
        Qt::QueuedConnection);
}

void QSocMcpManager::handleToolListFailure(QSocMcpClient *client, int id, const QString &message)
{
    const QString name = nameForClient(client);
    if (name.isEmpty()) {
        return;
    }
    auto &state = servers_[name];
    if (state.pendingListId < 0 || id != state.pendingListId) {
        return;
    }
    if (client->state() != QSocMcpClient::State::Ready) {
        return;
    }

    if (state.hasToolCatalog) {
        finishToolsListLater(client, id, message);
        return;
    }

    const bool retryLimitReached = state.reconnectAttempts >= kMaxReconnectAttempts;
    const QPointer<QSocMcpManager> managerGuard(this);
    const QPointer<QSocMcpClient>  clientGuard(client);
    if (retryLimitReached) {
        QSocConsole::warn()
            << QStringLiteral(
                   "MCP tool discovery failed for '%1': %2; retry limit reached, use /mcp "
                   "reconnect %1")
                   .arg(name, message);
    } else {
        QSocConsole::warn() << QStringLiteral("MCP tool discovery failed for '%1': %2; reconnecting")
                                   .arg(name, message);
    }
    if (managerGuard.isNull() || clientGuard.isNull()) {
        return;
    }
    auto server = servers_.find(name);
    if (server == servers_.end() || server->client != clientGuard.data()
        || server->pendingListId != id || clientGuard->state() != QSocMcpClient::State::Ready) {
        return;
    }
    server->pendingListId = -1;
    server->toolListDirty = false;
    clientGuard->stop();
}

void QSocMcpManager::registerTools(QSocMcpClient *client, const QList<McpToolDescriptor> &descriptors)
{
    const QString name = nameForClient(client);
    if (name.isEmpty()) {
        return;
    }
    unregisterToolsFor(name);

    if (toolRegistry_ == nullptr) {
        return;
    }

    auto     &state      = servers_[name];
    qsizetype registered = 0;
    for (const McpToolDescriptor &descriptor : descriptors) {
        auto *tool = new QSocMcpTool(client, descriptor, this);
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

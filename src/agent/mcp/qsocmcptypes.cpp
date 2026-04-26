// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/mcp/qsocmcptypes.h"

#include "common/qsocconsole.h"

namespace {

/* yaml-cpp throws InvalidNode if Type-querying methods (IsScalar/IsMap/...)
 * are called on a node returned for a missing map key. Always gate them
 * with IsDefined() first. */

QString yamlScalarToQString(const YAML::Node &node)
{
    if (!node.IsDefined() || !node.IsScalar()) {
        return {};
    }
    try {
        return QString::fromStdString(node.as<std::string>());
    } catch (const YAML::Exception &) {
        return {};
    }
}

QStringList yamlSequenceToQStringList(const YAML::Node &node)
{
    QStringList out;
    if (!node.IsDefined() || !node.IsSequence()) {
        return out;
    }
    for (const auto &item : node) {
        const QString text = yamlScalarToQString(item);
        if (!text.isEmpty()) {
            out << text;
        }
    }
    return out;
}

QMap<QString, QString> yamlMapToQStringMap(const YAML::Node &node)
{
    QMap<QString, QString> out;
    if (!node.IsDefined() || !node.IsMap()) {
        return out;
    }
    for (const auto &item : node) {
        const QString key   = yamlScalarToQString(item.first);
        const QString value = yamlScalarToQString(item.second);
        if (!key.isEmpty()) {
            out.insert(key, value);
        }
    }
    return out;
}

int yamlIntOrDefault(const YAML::Node &node, int defaultValue)
{
    if (!node.IsDefined() || !node.IsScalar()) {
        return defaultValue;
    }
    try {
        return node.as<int>();
    } catch (const YAML::Exception &) {
        return defaultValue;
    }
}

bool yamlBoolOrDefault(const YAML::Node &node, bool defaultValue)
{
    if (!node.IsDefined() || !node.IsScalar()) {
        return defaultValue;
    }
    try {
        return node.as<bool>();
    } catch (const YAML::Exception &) {
        return defaultValue;
    }
}

} // namespace

QString QSocMcp::normalizeName(const QString &name)
{
    QString out;
    out.reserve(name.size());
    for (QChar ch : name) {
        const bool isAlnum = (ch >= QChar('a') && ch <= QChar('z'))
                             || (ch >= QChar('A') && ch <= QChar('Z'))
                             || (ch >= QChar('0') && ch <= QChar('9'));
        if (isAlnum || ch == QChar('_') || ch == QChar('-')) {
            out.append(ch);
        } else {
            out.append(QChar('_'));
        }
    }
    while (out.contains(QStringLiteral("__"))) {
        out.replace(QStringLiteral("__"), QStringLiteral("_"));
    }
    while (out.startsWith(QChar('_'))) {
        out.remove(0, 1);
    }
    while (out.endsWith(QChar('_'))) {
        out.chop(1);
    }
    return out;
}

QString QSocMcp::buildToolName(const QString &serverName, const QString &toolName)
{
    return QString::fromLatin1(kToolNamePrefix) + normalizeName(serverName)
           + QString::fromLatin1(kToolNameSeparator) + normalizeName(toolName);
}

bool McpServerConfig::isValid() const
{
    if (name.isEmpty()) {
        return false;
    }
    if (type == QSocMcp::kTransportStdio) {
        return !command.isEmpty();
    }
    if (type == QSocMcp::kTransportHttp) {
        return !url.isEmpty();
    }
    return false;
}

QList<McpServerConfig> McpServerConfig::parseList(const YAML::Node &node)
{
    QList<McpServerConfig> out;
    if (!node.IsDefined() || node.IsNull() || !node.IsSequence()) {
        return out;
    }

    for (const auto &entry : node) {
        if (!entry.IsMap()) {
            QSocConsole::warn() << "Skipping non-map MCP server entry";
            continue;
        }

        McpServerConfig cfg;
        cfg.name             = yamlScalarToQString(entry["name"]);
        cfg.type             = yamlScalarToQString(entry["type"]);
        cfg.command          = yamlScalarToQString(entry["command"]);
        cfg.args             = yamlSequenceToQStringList(entry["args"]);
        cfg.env              = yamlMapToQStringMap(entry["env"]);
        cfg.url              = yamlScalarToQString(entry["url"]);
        cfg.headers          = yamlMapToQStringMap(entry["headers"]);
        cfg.proxy            = yamlScalarToQString(entry["proxy"]);
        cfg.connectTimeoutMs = yamlIntOrDefault(entry["connect_timeout_ms"], cfg.connectTimeoutMs);
        cfg.requestTimeoutMs = yamlIntOrDefault(entry["request_timeout_ms"], cfg.requestTimeoutMs);
        cfg.enabled          = yamlBoolOrDefault(entry["enabled"], true);

        if (cfg.type.isEmpty()) {
            cfg.type = QSocMcp::kTransportStdio;
        }

        if (!cfg.isValid()) {
            QSocConsole::warn() << "Skipping invalid MCP server entry:"
                                << (cfg.name.isEmpty() ? QStringLiteral("<unnamed>") : cfg.name);
            continue;
        }

        out << cfg;
    }

    return out;
}

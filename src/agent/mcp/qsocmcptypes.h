// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCMCPTYPES_H
#define QSOCMCPTYPES_H

#include <nlohmann/json.hpp>
#include <yaml-cpp/yaml.h>

#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

/**
 * @brief Server type identifiers and naming helpers for the Model
 *        Context Protocol client.
 */
namespace QSocMcp {

inline constexpr auto kTransportStdio    = "stdio";
inline constexpr auto kTransportHttp     = "http";
inline constexpr auto kToolNamePrefix    = "mcp__";
inline constexpr auto kToolNameSeparator = "__";

/**
 * @brief Normalize a name segment for use inside an MCP tool name.
 * @details Replaces every non-alphanumeric character (other than `_`
 *          and `-`) with `_`, collapses runs of `_`, and strips
 *          leading/trailing `_`.
 */
QString normalizeName(const QString &name);

/**
 * @brief Compose the namespaced tool name `mcp__<server>__<tool>`.
 */
QString buildToolName(const QString &serverName, const QString &toolName);

} // namespace QSocMcp

/**
 * @brief Configuration of a single MCP server entry from .qsoc.yml.
 */
struct McpServerConfig
{
    QString                name;    /* Logical name used to namespace tools */
    QString                type;    /* "stdio" | "http" */
    QString                command; /* stdio: executable path */
    QStringList            args;    /* stdio: argv after command */
    QMap<QString, QString> env;     /* stdio: extra environment variables */
    QString                url;     /* http: endpoint URL */
    QMap<QString, QString> headers; /* http: extra request headers */
    int                    connectTimeoutMs = 30000;
    int                    requestTimeoutMs = 60000;
    bool                   enabled          = true;

    /**
     * @brief Quick structural validation.
     * @return True when the entry has a name, a known type, and the
     *         minimum fields required by that type.
     */
    bool isValid() const;

    /**
     * @brief Parse a YAML sequence node into a list of server configs.
     * @details The parser is permissive: malformed entries are skipped
     *          rather than aborting the whole list, so a typo in one
     *          server does not lock the user out of the rest.
     * @param node YAML sequence node (may be undefined / null / non-sequence,
     *             in which case an empty list is returned).
     * @return List of parsed and validated server configs.
     */
    static QList<McpServerConfig> parseList(const YAML::Node &node);
};

/**
 * @brief Description of a single tool exposed by an MCP server.
 * @details Populated from the server's tools/list response.
 */
struct McpToolDescriptor
{
    QString        serverName;  /* Source server name */
    QString        toolName;    /* Original tool name on the server */
    QString        description; /* Human-readable description */
    nlohmann::json inputSchema; /* JSON Schema as returned by the server */
    bool           readOnly    = false;
    bool           destructive = false;
};

#endif // QSOCMCPTYPES_H

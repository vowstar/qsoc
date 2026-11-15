// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCPROXY_H
#define QSOCPROXY_H

#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QString>

class QSocConfig;

/**
 * @brief Process-wide proxy resolution for qsoc.
 * @details Three-tier precedence, highest first:
 *          1. Per-target spec (a flat string the caller plumbs through,
 *             e.g. an MCP server's `proxy:` field or an LLM endpoint's).
 *          2. The qsoc-wide spec parsed from the top-level `proxy:`
 *             section of the loaded QSocConfig (which itself layers
 *             project / user / system YAML files).
 *          3. The system default, picked up via libproxy / env vars
 *             once QSocProxy::ensureSystemBootstrap() has been called.
 *
 *          Per-target syntax accepted by parse():
 *            - empty / "system" / "default" -> DefaultProxy (fall-through)
 *            - "none" / "off" / "direct"    -> NoProxy
 *            - "http://[user:pass@]host:port"  -> HttpProxy
 *            - "socks5://[user:pass@]host:port" -> Socks5Proxy
 */
namespace QSocProxy {

struct Configuration
{
    QNetworkProxy proxy;
    QString       error;

    bool operator==(const Configuration &) const = default;
};

class NetworkAccessManager : public QNetworkAccessManager
{
public:
    explicit NetworkAccessManager(QObject *parent = nullptr);
    void                 setConfiguration(const Configuration &configuration);
    const Configuration &configuration() const;

protected:
    QNetworkReply *createRequest(
        Operation operation, const QNetworkRequest &request, QIODevice *outgoingData) override;

private:
    Configuration configuration_;
};

/**
 * @brief Bootstrap QNetworkProxyFactory once so DefaultProxy resolves
 *        to the system / environment proxy.
 */
void ensureSystemBootstrap();

/**
 * @brief Set the qsoc-wide fallback proxy parsed once at startup.
 * @details Subsystems that don't carry their own QSocConfig pointer
 *          (e.g. an MCP HTTP transport spawned by a factory) read this
 *          when their per-target spec is empty / "system".
 */
void setQsocWideDefault(const Configuration &proxy);

/**
 * @brief Current qsoc-wide fallback. Returns DefaultProxy until set.
 */
Configuration qsocWideDefault();

/**
 * @brief Parse a flat string spec into a QNetworkProxy.
 * @details Returns QNetworkProxy::DefaultProxy when the spec is empty,
 *          "system", or "default". Returns NoProxy for "none", "off",
 *          "direct". Otherwise expects an http:// or socks5:// URL.
 *          Invalid input retains an error and prevents network requests.
 */
Configuration parse(const QString &spec);

/**
 * @brief Parse the legacy nested `proxy.{type,host,port,user,password}`
 *        block from QSocConfig. Returns DefaultProxy if absent or "system".
 */
Configuration fromLegacyConfig(const QSocConfig *config);

/**
 * @brief Resolve a per-target spec over a qsoc-wide fallback.
 * @details An invalid global configuration always rejects requests.
 *          Otherwise per-target wins unless it is empty / "system" / "default",
 *          in which case the fallback applies. The fallback itself may
 *          be DefaultProxy, in which case the system bootstrap kicks in.
 */
Configuration resolve(const QString &perTarget, const Configuration &qsocWide);

/**
 * @brief Apply a resolved proxy to a manager.
 * @details Invalid configurations fail requests before opening a connection.
 */
void apply(NetworkAccessManager *manager, const Configuration &proxy);

} // namespace QSocProxy

#endif // QSOCPROXY_H

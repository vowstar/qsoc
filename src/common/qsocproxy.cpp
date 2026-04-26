// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocproxy.h"

#include "common/qsocconfig.h"
#include "common/qsocconsole.h"

#include <QNetworkAccessManager>
#include <QNetworkProxyFactory>
#include <QUrl>

namespace {

bool isSystemSpec(const QString &lower)
{
    return lower.isEmpty() || lower == QStringLiteral("system")
           || lower == QStringLiteral("default");
}

bool isNoProxySpec(const QString &lower)
{
    return lower == QStringLiteral("none") || lower == QStringLiteral("off")
           || lower == QStringLiteral("direct");
}

QNetworkProxy proxyFromUrl(const QString &raw, QNetworkProxy::ProxyType type, quint16 defaultPort)
{
    const QUrl url(raw);
    if (!url.isValid() || url.host().isEmpty()) {
        QSocConsole::warn() << "Invalid proxy URL, falling back to system:" << raw;
        return QNetworkProxy(QNetworkProxy::DefaultProxy);
    }
    QNetworkProxy
        proxy(type, url.host(), static_cast<quint16>(url.port(static_cast<int>(defaultPort))));
    if (!url.userName().isEmpty()) {
        proxy.setUser(url.userName());
        proxy.setPassword(url.password());
    }
    return proxy;
}

} // namespace

namespace {
QNetworkProxy &qsocWideStorage()
{
    static QNetworkProxy storage(QNetworkProxy::DefaultProxy);
    return storage;
}
} // namespace

void QSocProxy::ensureSystemBootstrap()
{
    static bool bootstrapped = false;
    if (bootstrapped) {
        return;
    }
    QNetworkProxyFactory::setUseSystemConfiguration(true);
    bootstrapped = true;
}

void QSocProxy::setQsocWideDefault(const QNetworkProxy &proxy)
{
    qsocWideStorage() = proxy;
}

QNetworkProxy QSocProxy::qsocWideDefault()
{
    return qsocWideStorage();
}

QNetworkProxy QSocProxy::parse(const QString &spec)
{
    const QString lower = spec.trimmed().toLower();
    if (isSystemSpec(lower)) {
        return QNetworkProxy(QNetworkProxy::DefaultProxy);
    }
    if (isNoProxySpec(lower)) {
        return QNetworkProxy(QNetworkProxy::NoProxy);
    }
    if (lower.startsWith(QStringLiteral("http://"))
        || lower.startsWith(QStringLiteral("https://"))) {
        return proxyFromUrl(spec.trimmed(), QNetworkProxy::HttpProxy, 8080);
    }
    if (lower.startsWith(QStringLiteral("socks5://"))
        || lower.startsWith(QStringLiteral("socks://"))) {
        return proxyFromUrl(spec.trimmed(), QNetworkProxy::Socks5Proxy, 1080);
    }
    QSocConsole::warn() << "Unrecognized proxy spec, falling back to system:" << spec;
    return QNetworkProxy(QNetworkProxy::DefaultProxy);
}

QNetworkProxy QSocProxy::fromLegacyConfig(const QSocConfig *config)
{
    if (config == nullptr) {
        return QNetworkProxy(QNetworkProxy::DefaultProxy);
    }
    const QString type
        = config->getValue(QStringLiteral("proxy.type"), QStringLiteral("system")).toLower();
    if (isSystemSpec(type)) {
        return QNetworkProxy(QNetworkProxy::DefaultProxy);
    }
    if (isNoProxySpec(type)) {
        return QNetworkProxy(QNetworkProxy::NoProxy);
    }

    QNetworkProxy::ProxyType qtype = QNetworkProxy::DefaultProxy;
    quint16                  port  = 0;
    if (type == QStringLiteral("http")) {
        qtype = QNetworkProxy::HttpProxy;
        port  = 8080;
    } else if (type == QStringLiteral("socks5") || type == QStringLiteral("socks")) {
        qtype = QNetworkProxy::Socks5Proxy;
        port  = 1080;
    } else {
        QSocConsole::warn() << "Unknown proxy.type, falling back to system:" << type;
        return QNetworkProxy(QNetworkProxy::DefaultProxy);
    }

    const QString host = config->getValue(QStringLiteral("proxy.host"), QStringLiteral("127.0.0.1"));
    const QString portStr = config->getValue(QStringLiteral("proxy.port"));
    if (!portStr.isEmpty()) {
        bool       ok        = false;
        const auto parsedRaw = portStr.toUInt(&ok);
        if (ok && parsedRaw > 0 && parsedRaw <= 65535) {
            port = static_cast<quint16>(parsedRaw);
        }
    }

    QNetworkProxy proxy(qtype, host, port);
    const QString user = config->getValue(QStringLiteral("proxy.user"));
    if (!user.isEmpty()) {
        proxy.setUser(user);
        proxy.setPassword(config->getValue(QStringLiteral("proxy.password")));
    }
    return proxy;
}

QNetworkProxy QSocProxy::resolve(const QString &perTarget, const QNetworkProxy &qsocWide)
{
    const QString lower = perTarget.trimmed().toLower();
    if (isSystemSpec(lower)) {
        return qsocWide;
    }
    return parse(perTarget);
}

void QSocProxy::apply(QNetworkAccessManager *manager, const QNetworkProxy &proxy)
{
    if (manager == nullptr) {
        return;
    }
    if (proxy.type() == QNetworkProxy::DefaultProxy) {
        ensureSystemBootstrap();
    }
    manager->setProxy(proxy);
}

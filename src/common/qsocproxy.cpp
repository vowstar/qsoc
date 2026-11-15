// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocproxy.h"

#include "common/qsocconfig.h"
#include <QNetworkProxyFactory>
#include <QNetworkReply>
#include <QPointer>
#include <QSet>
#include <QTimer>
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

QSocProxy::Configuration invalidConfiguration(const QString &reason)
{
    return {{}, QStringLiteral("Invalid proxy configuration: ") + reason};
}

QSocProxy::Configuration proxyFromUrl(
    const QString &raw, QNetworkProxy::ProxyType type, quint16 defaultPort)
{
    const QUrl url(raw, QUrl::StrictMode);
    if (!url.isValid() || url.host().isEmpty() || url.hasQuery() || url.hasFragment()
        || (!url.path().isEmpty() && url.path() != QStringLiteral("/")))
        return invalidConfiguration(QStringLiteral("expected a proxy URL with a host and port"));
    const QString authority
        = raw.mid(raw.indexOf(QStringLiteral("://")) + 3).section(QLatin1Char('/'), 0, 0);
    if (url.port(defaultPort) <= 0 || authority.endsWith(QLatin1Char(':')))
        return invalidConfiguration(QStringLiteral("proxy port must be between 1 and 65535"));
    QNetworkProxy
        proxy(type, url.host(), static_cast<quint16>(url.port(static_cast<int>(defaultPort))));
    if (!url.userName().isEmpty()) {
        proxy.setUser(url.userName());
        proxy.setPassword(url.password());
    }
    return {proxy, {}};
}

class RejectedReply : public QNetworkReply
{
public:
    RejectedReply(
        const QNetworkRequest           &request,
        QNetworkAccessManager::Operation operation,
        const QString                   &message,
        QObject                         *parent)
        : QNetworkReply(parent)
    {
        setRequest(request);
        setUrl(request.url());
        setOperation(operation);
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        setError(QNetworkReply::ProxyNotFoundError, message);
        QTimer::singleShot(0, this, [this] { finish(); });
    }

    void abort() override { finish(); }

protected:
    qint64 readData(char *, qint64) override { return -1; }

private:
    void finish()
    {
        if (isFinished())
            return;
        setFinished(true);
        QPointer<RejectedReply> guard(this);
        emit                    errorOccurred(error());
        // Slots can destroy the reply or its parent.
        // cppcheck-suppress knownConditionTrueFalse
        if (guard)
            emit guard->finished();
    }
};

QSocProxy::Configuration &qsocWideStorage()
{
    static QSocProxy::Configuration storage;
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

void QSocProxy::setQsocWideDefault(const Configuration &proxy)
{
    qsocWideStorage() = proxy;
}

QSocProxy::Configuration QSocProxy::qsocWideDefault()
{
    return qsocWideStorage();
}

QSocProxy::Configuration QSocProxy::parse(const QString &spec)
{
    const QString lower = spec.trimmed().toLower();
    if (isSystemSpec(lower)) {
        return {};
    }
    if (isNoProxySpec(lower)) {
        return {QNetworkProxy(QNetworkProxy::NoProxy), {}};
    }
    if (lower.startsWith(QStringLiteral("http://"))) {
        return proxyFromUrl(spec.trimmed(), QNetworkProxy::HttpProxy, 8080);
    }
    if (lower.startsWith(QStringLiteral("socks5://"))
        || lower.startsWith(QStringLiteral("socks://"))) {
        return proxyFromUrl(spec.trimmed(), QNetworkProxy::Socks5Proxy, 1080);
    }
    return invalidConfiguration(QStringLiteral("unsupported proxy scheme or value"));
}

QSocProxy::Configuration QSocProxy::fromLegacyConfig(const QSocConfig *config)
{
    if (config == nullptr) {
        return {};
    }
    if (config->hasLoadErrors())
        return invalidConfiguration(QStringLiteral("configuration file is invalid or unreadable"));

    const QSet<QString> fields
        = {QStringLiteral("type"),
           QStringLiteral("host"),
           QStringLiteral("port"),
           QStringLiteral("user"),
           QStringLiteral("password")};
    const auto node = config->getYamlNode(QStringLiteral("proxy"));
    if (node.IsDefined() && !node.IsNull()) {
        if (!node.IsMap())
            return invalidConfiguration(QStringLiteral("proxy must be a mapping"));
        for (const auto &entry : node) {
            if (!entry.first.IsScalar() || !entry.second.IsScalar())
                return invalidConfiguration(QStringLiteral("proxy fields must be scalar values"));
            if (!fields.contains(QString::fromStdString(entry.first.Scalar())))
                return invalidConfiguration(QStringLiteral("unknown proxy field"));
        }
    }
    if (config->hasKey(QStringLiteral("proxy")))
        return invalidConfiguration(QStringLiteral("proxy must be a mapping"));
    const auto values         = config->getAllValues();
    bool       hasProxyFields = false;
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        if (!it.key().startsWith(QStringLiteral("proxy.")))
            continue;
        hasProxyFields = true;
        if (!fields.contains(it.key().mid(6)))
            return invalidConfiguration(QStringLiteral("unknown proxy field"));
    }
    if (hasProxyFields && !config->hasKey(QStringLiteral("proxy.type")))
        return invalidConfiguration(QStringLiteral("proxy.type is required"));
    const QString type = config->getValue(QStringLiteral("proxy.type"), QStringLiteral("system"))
                             .trimmed()
                             .toLower();
    if (isSystemSpec(type)) {
        return {};
    }
    if (isNoProxySpec(type)) {
        return {QNetworkProxy(QNetworkProxy::NoProxy), {}};
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
        return invalidConfiguration(QStringLiteral("unsupported proxy.type"));
    }

    const QString host
        = config->getValue(QStringLiteral("proxy.host"), QStringLiteral("127.0.0.1")).trimmed();
    QUrl hostUrl;
    hostUrl.setScheme(QStringLiteral("http"));
    hostUrl.setHost(host);
    if (host.isEmpty() || !hostUrl.isValid() || hostUrl.host().isEmpty())
        return invalidConfiguration(QStringLiteral("proxy.host must be a hostname or IP address"));
    const QString portStr = config->getValue(QStringLiteral("proxy.port"));
    if (config->hasKey(QStringLiteral("proxy.port"))) {
        bool       ok        = false;
        const auto parsedRaw = portStr.toUInt(&ok);
        if (!ok || parsedRaw == 0 || parsedRaw > 65535)
            return invalidConfiguration(QStringLiteral("proxy.port must be between 1 and 65535"));
        port = static_cast<quint16>(parsedRaw);
    }

    QNetworkProxy proxy(qtype, host, port);
    const QString user = config->getValue(QStringLiteral("proxy.user"));
    if (!user.isEmpty()) {
        proxy.setUser(user);
        proxy.setPassword(config->getValue(QStringLiteral("proxy.password")));
    }
    return {proxy, {}};
}

QSocProxy::Configuration QSocProxy::resolve(const QString &perTarget, const Configuration &qsocWide)
{
    if (!qsocWide.error.isEmpty())
        return qsocWide;
    const QString lower = perTarget.trimmed().toLower();
    if (isSystemSpec(lower)) {
        return qsocWide;
    }
    return parse(perTarget);
}

void QSocProxy::apply(NetworkAccessManager *manager, const Configuration &proxy)
{
    if (manager == nullptr) {
        return;
    }
    manager->setConfiguration(proxy);
}

QSocProxy::NetworkAccessManager::NetworkAccessManager(QObject *parent)
    : QNetworkAccessManager(parent)
{}

void QSocProxy::NetworkAccessManager::setConfiguration(const Configuration &configuration)
{
    configuration_ = configuration;
    if (!configuration.error.isEmpty())
        return;
    if (configuration.proxy.type() == QNetworkProxy::DefaultProxy) {
        ensureSystemBootstrap();
    }
    setProxy(configuration.proxy);
}

const QSocProxy::Configuration &QSocProxy::NetworkAccessManager::configuration() const
{
    return configuration_;
}

QNetworkReply *QSocProxy::NetworkAccessManager::createRequest(
    Operation operation, const QNetworkRequest &request, QIODevice *outgoingData)
{
    if (!configuration_.error.isEmpty())
        return new RejectedReply(request, operation, configuration_.error, this);
    return QNetworkAccessManager::createRequest(operation, request, outgoingData);
}

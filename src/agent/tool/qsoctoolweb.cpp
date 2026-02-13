// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "agent/tool/qsoctoolweb.h"

#include <QNetworkProxy>
#include <QNetworkProxyFactory>
#include <QNetworkRequest>
#include <QTextDocument>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

static const QString kUserAgent
    = "Mozilla/5.0 AppleWebKit/537.36 (KHTML, like Gecko; compatible; QSoC/1.0; "
      "+https://github.com/vowstar/qsoc)";
static constexpr int kSearchTimeout = 15000;
static constexpr int kFetchTimeout  = 30000;
static constexpr int kMaxBytes      = 1048576;
static constexpr int kMaxTextSize   = 100000;

/* ========== QSocToolWebSearch ========== */

QSocToolWebSearch::QSocToolWebSearch(QObject *parent, QSocConfig *config)
    : QSocTool(parent)
    , config(config)
{
    networkManager = new QNetworkAccessManager(this);
    setupProxy();
}

QSocToolWebSearch::~QSocToolWebSearch() = default;

QString QSocToolWebSearch::getName() const
{
    return "web_search";
}

QString QSocToolWebSearch::getDescription() const
{
    return "Search the web via SearXNG. Returns titles, URLs, and snippets.";
}

json QSocToolWebSearch::getParametersSchema() const
{
    return {
        {"type", "object"},
        {"properties",
         {{"query", {{"type", "string"}, {"description", "Search query"}}},
          {"count",
           {{"type", "integer"}, {"description", "Number of results (default: 5, max: 20)"}}}}},
        {"required", json::array({"query"})}};
}

void QSocToolWebSearch::setupProxy()
{
    if (!networkManager) {
        return;
    }

    if (!config) {
        QNetworkProxyFactory::setUseSystemConfiguration(true);
        networkManager->setProxy(QNetworkProxy::DefaultProxy);
        return;
    }

    QString proxyType = config->getValue("proxy.type", "system").toLower();

    QNetworkProxy proxy;

    if (proxyType == "none") {
        QNetworkProxyFactory::setUseSystemConfiguration(false);
        proxy.setType(QNetworkProxy::NoProxy);
    } else if (proxyType == "socks5") {
        QNetworkProxyFactory::setUseSystemConfiguration(false);
        proxy.setType(QNetworkProxy::Socks5Proxy);
        proxy.setHostName(config->getValue("proxy.host", "127.0.0.1"));
        proxy.setPort(config->getValue("proxy.port", "1080").toUInt());
        QString user = config->getValue("proxy.user");
        if (!user.isEmpty()) {
            proxy.setUser(user);
            proxy.setPassword(config->getValue("proxy.password"));
        }
    } else if (proxyType == "http") {
        QNetworkProxyFactory::setUseSystemConfiguration(false);
        proxy.setType(QNetworkProxy::HttpProxy);
        proxy.setHostName(config->getValue("proxy.host", "127.0.0.1"));
        proxy.setPort(config->getValue("proxy.port", "8080").toUInt());
        QString user = config->getValue("proxy.user");
        if (!user.isEmpty()) {
            proxy.setUser(user);
            proxy.setPassword(config->getValue("proxy.password"));
        }
    } else {
        QNetworkProxyFactory::setUseSystemConfiguration(true);
        networkManager->setProxy(QNetworkProxy::DefaultProxy);
        return;
    }

    networkManager->setProxy(proxy);
}

QString QSocToolWebSearch::execute(const json &arguments)
{
    if (!arguments.contains("query") || !arguments["query"].is_string()) {
        return "Error: query is required";
    }

    QString query = QString::fromStdString(arguments["query"].get<std::string>());
    if (query.trimmed().isEmpty()) {
        return "Error: query must not be empty";
    }

    /* Get API URL from config */
    QString apiUrl;
    if (config) {
        apiUrl = config->getValue("web.search_api_url");
    }
    if (apiUrl.isEmpty()) {
        return "Error: web.search_api_url not configured. "
               "Set it in qsoc.yml or QSOC_WEB_SEARCH_API_URL env.";
    }

    /* Get result count */
    int count = 5;
    if (arguments.contains("count") && arguments["count"].is_number_integer()) {
        count = arguments["count"].get<int>();
        count = qBound(1, count, 20);
    }

    /* Build SearXNG API URL */
    QUrl      url(apiUrl + "/search");
    QUrlQuery urlQuery;
    urlQuery.addQueryItem("q", query);
    urlQuery.addQueryItem("format", "json");
    urlQuery.addQueryItem("categories", "general");
    urlQuery.addQueryItem("pageno", "1");
    url.setQuery(urlQuery);

    /* Build request */
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, kUserAgent);
    request.setRawHeader("Accept", "application/json");

    /* Add API key if configured */
    if (config) {
        QString apiKey = config->getValue("web.search_api_key");
        if (!apiKey.isEmpty()) {
            request.setRawHeader("Authorization", ("Bearer " + apiKey).toUtf8());
        }
    }

    /* Execute request */
    QNetworkReply *reply = networkManager->get(request);
    currentReply         = reply;

    QEventLoop loop;
    currentLoop = &loop;

    bool finished = false;
    QObject::connect(reply, &QNetworkReply::finished, &loop, [&finished, &loop]() {
        finished = true;
        loop.quit();
    });

    QTimer::singleShot(kSearchTimeout, &loop, [&loop]() { loop.quit(); });

    loop.exec();
    currentReply = nullptr;
    currentLoop  = nullptr;

    if (!finished) {
        reply->abort();
        reply->deleteLater();
        return QString("Error: request timed out after %1ms").arg(kSearchTimeout);
    }

    /* Check for network error */
    if (reply->error() != QNetworkReply::NoError) {
        QString errorMsg = QString("Error: %1").arg(reply->errorString());
        reply->deleteLater();
        return errorMsg;
    }

    /* Check HTTP status code */
    int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (httpStatus < 200 || httpStatus >= 300) {
        QByteArray body    = reply->readAll();
        QString    snippet = QString::fromUtf8(body.left(500));
        reply->deleteLater();
        return QString("Error: HTTP %1: %2").arg(httpStatus).arg(snippet);
    }

    /* Parse JSON response */
    QByteArray responseData = reply->readAll();
    reply->deleteLater();

    json response;
    try {
        response = json::parse(responseData.toStdString());
    } catch (const json::exception &e) {
        return QString("Error: failed to parse response: %1").arg(e.what());
    }

    if (!response.contains("results") || !response["results"].is_array()) {
        return "Error: unexpected response format (no results array)";
    }

    /* Format results */
    auto    results = response["results"];
    QString output  = QString("Search results for \"%1\":\n").arg(query);

    int shown = 0;
    for (const auto &result : results) {
        if (shown >= count) {
            break;
        }

        QString title   = result.contains("title") && result["title"].is_string()
                              ? QString::fromStdString(result["title"].get<std::string>())
                              : "(no title)";
        QString url     = result.contains("url") && result["url"].is_string()
                              ? QString::fromStdString(result["url"].get<std::string>())
                              : "(no url)";
        QString snippet = result.contains("content") && result["content"].is_string()
                              ? QString::fromStdString(result["content"].get<std::string>())
                              : "";

        shown++;
        output += QString("\n%1. Title: %2\n   URL: %3\n").arg(shown).arg(title, url);
        if (!snippet.isEmpty()) {
            output += QString("   Snippet: %1\n").arg(snippet);
        }
    }

    if (shown == 0) {
        output += "\nNo results found.";
    }

    return output;
}

void QSocToolWebSearch::abort()
{
    if (currentReply && currentReply->isRunning()) {
        currentReply->abort();
    }
    if (currentLoop && currentLoop->isRunning()) {
        currentLoop->quit();
    }
}

/* ========== QSocToolWebFetch ========== */

QSocToolWebFetch::QSocToolWebFetch(QObject *parent, QSocConfig *config)
    : QSocTool(parent)
    , config(config)
{
    networkManager = new QNetworkAccessManager(this);
    setupProxy();
}

QSocToolWebFetch::~QSocToolWebFetch() = default;

QString QSocToolWebFetch::getName() const
{
    return "web_fetch";
}

QString QSocToolWebFetch::getDescription() const
{
    return "Fetch content from a URL. HTML pages are converted to plain text. "
           "Returns the page content (truncated if too large).";
}

json QSocToolWebFetch::getParametersSchema() const
{
    return {
        {"type", "object"},
        {"properties",
         {{"url", {{"type", "string"}, {"description", "URL to fetch"}}},
          {"timeout",
           {{"type", "integer"}, {"description", "Timeout in milliseconds (default: 30000)"}}}}},
        {"required", json::array({"url"})}};
}

void QSocToolWebFetch::setupProxy()
{
    if (!networkManager) {
        return;
    }

    if (!config) {
        QNetworkProxyFactory::setUseSystemConfiguration(true);
        networkManager->setProxy(QNetworkProxy::DefaultProxy);
        return;
    }

    QString proxyType = config->getValue("proxy.type", "system").toLower();

    QNetworkProxy proxy;

    if (proxyType == "none") {
        QNetworkProxyFactory::setUseSystemConfiguration(false);
        proxy.setType(QNetworkProxy::NoProxy);
    } else if (proxyType == "socks5") {
        QNetworkProxyFactory::setUseSystemConfiguration(false);
        proxy.setType(QNetworkProxy::Socks5Proxy);
        proxy.setHostName(config->getValue("proxy.host", "127.0.0.1"));
        proxy.setPort(config->getValue("proxy.port", "1080").toUInt());
        QString user = config->getValue("proxy.user");
        if (!user.isEmpty()) {
            proxy.setUser(user);
            proxy.setPassword(config->getValue("proxy.password"));
        }
    } else if (proxyType == "http") {
        QNetworkProxyFactory::setUseSystemConfiguration(false);
        proxy.setType(QNetworkProxy::HttpProxy);
        proxy.setHostName(config->getValue("proxy.host", "127.0.0.1"));
        proxy.setPort(config->getValue("proxy.port", "8080").toUInt());
        QString user = config->getValue("proxy.user");
        if (!user.isEmpty()) {
            proxy.setUser(user);
            proxy.setPassword(config->getValue("proxy.password"));
        }
    } else {
        QNetworkProxyFactory::setUseSystemConfiguration(true);
        networkManager->setProxy(QNetworkProxy::DefaultProxy);
        return;
    }

    networkManager->setProxy(proxy);
}

QString QSocToolWebFetch::htmlToText(const QString &html)
{
    QTextDocument doc;
    doc.setHtml(html);
    return doc.toPlainText();
}

QString QSocToolWebFetch::execute(const json &arguments)
{
    if (!arguments.contains("url") || !arguments["url"].is_string()) {
        return "Error: url is required";
    }

    QString urlStr = QString::fromStdString(arguments["url"].get<std::string>());
    QUrl    url(urlStr);
    if (!url.isValid()) {
        return QString("Error: invalid URL: %1").arg(urlStr);
    }

    if (url.scheme() != "http" && url.scheme() != "https") {
        return QString("Error: only http and https URLs are supported, got: %1").arg(url.scheme());
    }

    /* Get timeout */
    int timeout = kFetchTimeout;
    if (arguments.contains("timeout") && arguments["timeout"].is_number_integer()) {
        int paramTimeout = arguments["timeout"].get<int>();
        if (paramTimeout > 0) {
            timeout = paramTimeout;
        }
    }

    /* Build request */
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, kUserAgent);
    request.setMaximumRedirectsAllowed(10);
    request.setAttribute(
        QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

    /* Execute request */
    QNetworkReply *reply = networkManager->get(request);
    currentReply         = reply;

    /* Track download size */
    qint64 downloadedBytes = 0;
    bool   aborted         = false;
    QObject::connect(reply, &QNetworkReply::downloadProgress, reply, [&](qint64 received, qint64) {
        downloadedBytes = received;
        if (received > kMaxBytes && !aborted) {
            aborted = true;
            reply->abort();
        }
    });

    QEventLoop loop;
    currentLoop = &loop;

    bool finished = false;
    QObject::connect(reply, &QNetworkReply::finished, &loop, [&finished, &loop]() {
        finished = true;
        loop.quit();
    });

    QTimer::singleShot(timeout, &loop, [&loop]() { loop.quit(); });

    loop.exec();
    currentReply = nullptr;
    currentLoop  = nullptr;

    if (!finished) {
        reply->abort();
        reply->deleteLater();
        return QString("Error: request timed out after %1ms").arg(timeout);
    }

    if (aborted) {
        reply->deleteLater();
        return QString("Error: response too large (>%1 bytes)").arg(kMaxBytes);
    }

    /* Check for network error */
    if (reply->error() != QNetworkReply::NoError) {
        QString errorMsg = QString("Error: %1").arg(reply->errorString());
        reply->deleteLater();
        return errorMsg;
    }

    /* Check HTTP status code */
    int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (httpStatus < 200 || httpStatus >= 300) {
        QByteArray body    = reply->readAll();
        QString    snippet = QString::fromUtf8(body.left(500));
        reply->deleteLater();
        return QString("Error: HTTP %1: %2").arg(httpStatus).arg(snippet);
    }

    /* Read response body */
    QByteArray responseData = reply->readAll();
    QString    contentType = reply->header(QNetworkRequest::ContentTypeHeader).toString().toLower();
    reply->deleteLater();

    if (responseData.isEmpty()) {
        return "(no content)";
    }

    /* Check content type */
    bool isHtml = contentType.contains("text/html");
    bool isText = contentType.contains("text/") || contentType.contains("application/json")
                  || contentType.contains("application/xml")
                  || contentType.contains("application/javascript") || contentType.contains("+xml")
                  || contentType.contains("+json");

    if (!isHtml && !isText) {
        return QString("Error: binary content (content-type: %1), cannot display").arg(contentType);
    }

    QString text = QString::fromUtf8(responseData);

    if (isHtml) {
        text = htmlToText(text);
    }

    /* Truncate if too large */
    if (text.size() > kMaxTextSize) {
        text = text.left(kMaxTextSize) + "\n... (content truncated)";
    }

    return text.isEmpty() ? "(no content)" : text;
}

void QSocToolWebFetch::abort()
{
    if (currentReply && currentReply->isRunning()) {
        currentReply->abort();
    }
    if (currentLoop && currentLoop->isRunning()) {
        currentLoop->quit();
    }
}

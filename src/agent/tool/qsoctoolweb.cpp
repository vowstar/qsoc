// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "agent/tool/qsoctoolweb.h"

#include <QNetworkProxy>
#include <QNetworkProxyFactory>
#include <QNetworkRequest>
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
    return "Fetch content from a URL. HTML pages are converted to Markdown. "
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

namespace {

enum class CtxType : uint8_t {
    Skip,
    Pre,
    Anchor,
    Heading,
    Bold,
    Italic,
    Code,
    List,
    ListItem,
    Blockquote,
    Table,
    TableRow,
    TableCell,
};

struct Context
{
    CtxType type;
    QString data;
    int     counter  = 0;
    bool    isHeader = false;
};

struct TableBuffer
{
    QVector<QVector<QString>> rows;
    QVector<bool>             headerFlags;
    QVector<QString>          currentRow;
    bool                      currentIsHeader = false;
    QString                   cellBuf;
};

static const QHash<QString, QString> &entityMap()
{
    static const QHash<QString, QString> map = {
        {"amp", "&"},
        {"lt", "<"},
        {"gt", ">"},
        {"quot", "\""},
        {"apos", "'"},
        {"nbsp", " "},
        {"ndash", "\u2013"},
        {"mdash", "\u2014"},
        {"lsquo", "\u2018"},
        {"rsquo", "\u2019"},
        {"ldquo", "\u201C"},
        {"rdquo", "\u201D"},
        {"bull", "\u2022"},
        {"hellip", "\u2026"},
        {"copy", "\u00A9"},
        {"reg", "\u00AE"},
        {"trade", "\u2122"},
        {"times", "\u00D7"},
    };
    return map;
}

static QString decodeEntity(const QString &entity)
{
    auto it = entityMap().constFind(entity);
    if (it != entityMap().constEnd()) {
        return it.value();
    }
    if (entity.startsWith('#')) {
        bool ok  = false;
        uint num = 0;
        if (entity.size() > 1 && (entity[1] == 'x' || entity[1] == 'X')) {
            num = entity.mid(2).toUInt(&ok, 16);
        } else {
            num = entity.mid(1).toUInt(&ok, 10);
        }
        if (ok && num > 0) {
            if (num <= 0xFFFF) {
                return QString(QChar(num));
            }
            /* Surrogate pair for supplementary plane */
            char32_t cp = static_cast<char32_t>(num);
            return QString::fromUcs4(&cp, 1);
        }
    }
    return QString("&%1;").arg(entity);
}

static QString extractAttr(const QString &tagBody, const QString &attr)
{
    /* Find attr="value" or attr='value' in tag body */
    int pos = 0;
    while (pos < tagBody.size()) {
        int idx = tagBody.indexOf(attr, pos, Qt::CaseInsensitive);
        if (idx < 0) {
            return {};
        }
        /* Ensure it's a word boundary: start of string or preceded by space */
        if (idx > 0 && tagBody[idx - 1] != ' ' && tagBody[idx - 1] != '\t'
            && tagBody[idx - 1] != '\n') {
            pos = idx + 1;
            continue;
        }
        int eqPos = idx + attr.size();
        /* Skip whitespace before '=' */
        while (eqPos < tagBody.size() && (tagBody[eqPos] == ' ' || tagBody[eqPos] == '\t')) {
            eqPos++;
        }
        if (eqPos >= tagBody.size() || tagBody[eqPos] != '=') {
            pos = eqPos;
            continue;
        }
        eqPos++;
        /* Skip whitespace after '=' */
        while (eqPos < tagBody.size() && (tagBody[eqPos] == ' ' || tagBody[eqPos] == '\t')) {
            eqPos++;
        }
        if (eqPos >= tagBody.size()) {
            return {};
        }
        QChar quote = tagBody[eqPos];
        if (quote == '"' || quote == '\'') {
            int end = tagBody.indexOf(quote, eqPos + 1);
            if (end < 0) {
                return {};
            }
            return tagBody.mid(eqPos + 1, end - eqPos - 1);
        }
        /* Unquoted value */
        int end = eqPos;
        while (end < tagBody.size() && tagBody[end] != ' ' && tagBody[end] != '>'
               && tagBody[end] != '\t') {
            end++;
        }
        return tagBody.mid(eqPos, end - eqPos);
    }
    return {};
}

static QString formatTable(const TableBuffer &tb)
{
    if (tb.rows.isEmpty()) {
        return {};
    }

    /* Determine column count */
    int cols = 0;
    for (const auto &row : tb.rows) {
        cols = qMax(cols, row.size());
    }
    if (cols == 0) {
        return {};
    }

    /* Compute column widths */
    QVector<int> widths(cols, 3);
    for (const auto &row : tb.rows) {
        for (int c = 0; c < row.size(); c++) {
            widths[c] = qMax(widths[c], row[c].size());
        }
    }

    QString out;

    /* Find header row index (first row where isHeader is true) */
    int headerIdx = -1;
    for (int r = 0; r < tb.rows.size(); r++) {
        if (r < tb.headerFlags.size() && tb.headerFlags[r]) {
            headerIdx = r;
            break;
        }
    }

    auto formatRow = [&](const QVector<QString> &row) {
        out += '|';
        for (int c = 0; c < cols; c++) {
            QString cell = (c < row.size()) ? row[c] : QString();
            /* Escape pipe in cell content */
            cell.replace('|', "\\|");
            out += ' ';
            out += cell;
            int pad = widths[c] - cell.size();
            for (int p = 0; p < pad; p++) {
                out += ' ';
            }
            out += " |";
        }
        out += '\n';
    };

    auto formatSeparator = [&]() {
        out += '|';
        for (int c = 0; c < cols; c++) {
            out += ' ';
            for (int p = 0; p < widths[c]; p++) {
                out += '-';
            }
            out += " |";
        }
        out += '\n';
    };

    if (headerIdx >= 0) {
        /* Output header row */
        formatRow(tb.rows[headerIdx]);
        formatSeparator();
        /* Output remaining rows */
        for (int r = 0; r < tb.rows.size(); r++) {
            if (r != headerIdx) {
                formatRow(tb.rows[r]);
            }
        }
    } else {
        /* No header: use separator after first row */
        formatRow(tb.rows[0]);
        formatSeparator();
        for (int r = 1; r < tb.rows.size(); r++) {
            formatRow(tb.rows[r]);
        }
    }

    return out;
}

/* Count how many nesting levels of a type are on the stack */
static int countCtx(const QVector<Context> &stack, CtxType type)
{
    int n = 0;
    for (const auto &ctx : stack) {
        if (ctx.type == type) {
            n++;
        }
    }
    return n;
}

static bool inCtx(const QVector<Context> &stack, CtxType type)
{
    for (int i = stack.size() - 1; i >= 0; i--) {
        if (stack[i].type == type) {
            return true;
        }
    }
    return false;
}

static QString listIndent(const QVector<Context> &stack)
{
    int depth = countCtx(stack, CtxType::List);
    if (depth <= 1) {
        return {};
    }
    return QString((depth - 1) * 2, ' ');
}

} /* anonymous namespace */

QString QSocToolWebFetch::htmlToMarkdown(const QString &html)
{
    /* Stream-based HTML-to-Markdown converter.
     * Uses a context stack for nesting and a table buffer for GFM tables.
     * QTextDocument::setHtml() segfaults on large/complex pages (e.g. GitHub)
     * and regex approaches overflow the PCRE2 stack. A linear scan is safe. */
    QString result;
    result.reserve(html.size() / 2);

    const QStringList skipTags = {"script", "style", "svg", "noscript", "head"};

    QVector<Context>     stack;
    QVector<TableBuffer> tableBufs;
    int                  skipDepth  = 0;
    int                  tableDepth = 0;
    QString              skipTag;

    auto output = [&](const QString &s) {
        if (tableDepth > 0 && !tableBufs.isEmpty()) {
            tableBufs.last().cellBuf += s;
        } else {
            result += s;
        }
    };

    auto outputCh = [&](QChar c) {
        if (tableDepth > 0 && !tableBufs.isEmpty()) {
            tableBufs.last().cellBuf += c;
        } else {
            result += c;
        }
    };

    auto ensureNewline = [&]() {
        if (tableDepth > 0) {
            return;
        }
        if (!result.isEmpty() && !result.endsWith('\n')) {
            result += '\n';
        }
    };

    auto ensureBlankLine = [&]() {
        if (tableDepth > 0) {
            return;
        }
        if (result.isEmpty()) {
            return;
        }
        if (!result.endsWith('\n')) {
            result += '\n';
        }
        if (!result.endsWith("\n\n")) {
            result += '\n';
        }
    };

    int i   = 0;
    int len = html.size();

    while (i < len) {
        QChar ch = html[i];

        /* HTML comment */
        if (ch == '<' && i + 3 < len && html[i + 1] == '!' && html[i + 2] == '-'
            && html[i + 3] == '-') {
            int end = html.indexOf("-->", i + 4);
            i       = (end >= 0) ? end + 3 : len;
            continue;
        }

        /* CDATA */
        if (ch == '<' && i + 8 < len && html.mid(i, 9) == "<![CDATA[") {
            int end = html.indexOf("]]>", i + 9);
            if (end >= 0) {
                output(html.mid(i + 9, end - i - 9));
                i = end + 3;
            } else {
                i = len;
            }
            continue;
        }

        /* Tag */
        if (ch == '<') {
            int tagStart = i + 1;

            /* Self-closing or doctype check */
            bool isClose = (tagStart < len && html[tagStart] == '/');
            if (isClose) {
                tagStart++;
            }

            /* Skip <! doctype etc */
            if (!isClose && tagStart < len && html[tagStart] == '!') {
                while (i < len && html[i] != '>') {
                    i++;
                }
                if (i < len) {
                    i++;
                }
                continue;
            }

            /* Extract tag name */
            int tagEnd = tagStart;
            while (tagEnd < len && html[tagEnd] != '>' && html[tagEnd] != ' '
                   && html[tagEnd] != '\t' && html[tagEnd] != '\n' && html[tagEnd] != '/'
                   && html[tagEnd] != '"' && html[tagEnd] != '\'') {
                tagEnd++;
            }
            QString tagName = html.mid(tagStart, tagEnd - tagStart).toLower();

            /* Extract full tag body (attributes) up to '>' */
            int tagClosePos = tagEnd;
            while (tagClosePos < len && html[tagClosePos] != '>') {
                /* Skip quoted attribute values */
                if (html[tagClosePos] == '"' || html[tagClosePos] == '\'') {
                    QChar q     = html[tagClosePos];
                    int   end   = html.indexOf(q, tagClosePos + 1);
                    tagClosePos = (end >= 0) ? end + 1 : len;
                    continue;
                }
                tagClosePos++;
            }
            QString tagBody   = html.mid(tagEnd, tagClosePos - tagEnd);
            bool    selfClose = (!tagBody.isEmpty() && tagBody.endsWith('/')) || tagName == "br"
                             || tagName == "hr" || tagName == "img" || tagName == "input"
                             || tagName == "meta" || tagName == "link" || tagName == "wbr";
            i = tagClosePos;
            if (i < len) {
                i++; /* skip '>' */
            }

            /* Skip-depth handling */
            if (skipDepth > 0) {
                if (isClose && tagName == skipTag) {
                    skipDepth--;
                } else if (!isClose && !selfClose && skipTags.contains(tagName)) {
                    skipDepth++;
                }
                continue;
            }

            if (!isClose && !selfClose && skipTags.contains(tagName)) {
                skipTag   = tagName;
                skipDepth = 1;
                continue;
            }

            /* --- Open tags --- */
            if (!isClose && !selfClose) {
                /* Headings */
                if (tagName.size() == 2 && tagName[0] == 'h' && tagName[1] >= '1'
                    && tagName[1] <= '6') {
                    int level = tagName[1].digitValue();
                    ensureBlankLine();
                    output(QString(level, '#') + ' ');
                    stack.push_back({CtxType::Heading, {}, level, false});
                    continue;
                }
                /* Bold */
                if (tagName == "strong" || tagName == "b") {
                    output("**");
                    stack.push_back({CtxType::Bold, {}, 0, false});
                    continue;
                }
                /* Italic */
                if (tagName == "em" || tagName == "i") {
                    output("*");
                    stack.push_back({CtxType::Italic, {}, 0, false});
                    continue;
                }
                /* Inline code */
                if (tagName == "code" && !inCtx(stack, CtxType::Pre)) {
                    output("`");
                    stack.push_back({CtxType::Code, {}, 0, false});
                    continue;
                }
                /* Pre/code block */
                if (tagName == "pre") {
                    ensureBlankLine();
                    QString lang;
                    /* Check for class="language-xxx" on pre or nested code */
                    QString cls = extractAttr(tagBody, "class");
                    if (cls.startsWith("language-")) {
                        lang = cls.mid(9);
                    }
                    output("```" + lang + "\n");
                    stack.push_back({CtxType::Pre, lang, 0, false});
                    continue;
                }
                /* Code inside pre — skip the extra backtick */
                if (tagName == "code" && inCtx(stack, CtxType::Pre)) {
                    /* Check for language hint on code tag */
                    QString cls = extractAttr(tagBody, "class");
                    if (cls.startsWith("language-")) {
                        /* Update the pre's language if not set */
                        for (int s = stack.size() - 1; s >= 0; s--) {
                            if (stack[s].type == CtxType::Pre && stack[s].data.isEmpty()) {
                                QString lang  = cls.mid(9);
                                stack[s].data = lang;
                                /* Patch the output: replace ``` with ```lang */
                                if (tableDepth == 0 && result.endsWith("```\n")) {
                                    result.chop(4);
                                    result += "```" + lang + "\n";
                                }
                                break;
                            }
                        }
                    }
                    /* Don't push context — we handle </code> inside pre specially */
                    continue;
                }
                /* Anchor */
                if (tagName == "a") {
                    QString href = extractAttr(tagBody, "href");
                    output("[");
                    stack.push_back({CtxType::Anchor, href, 0, false});
                    continue;
                }
                /* Lists */
                if (tagName == "ul") {
                    ensureNewline();
                    stack.push_back({CtxType::List, "ul", 0, false});
                    continue;
                }
                if (tagName == "ol") {
                    ensureNewline();
                    stack.push_back({CtxType::List, "ol", 0, false});
                    continue;
                }
                if (tagName == "li") {
                    ensureNewline();
                    /* Find parent list */
                    for (int s = stack.size() - 1; s >= 0; s--) {
                        if (stack[s].type == CtxType::List) {
                            QString indent = listIndent(stack);
                            if (stack[s].data == "ol") {
                                stack[s].counter++;
                                output(indent + QString::number(stack[s].counter) + ". ");
                            } else {
                                output(indent + "- ");
                            }
                            break;
                        }
                    }
                    stack.push_back({CtxType::ListItem, {}, 0, false});
                    continue;
                }
                /* Blockquote */
                if (tagName == "blockquote") {
                    ensureBlankLine();
                    output("> ");
                    stack.push_back({CtxType::Blockquote, {}, 0, false});
                    continue;
                }
                /* Table */
                if (tagName == "table") {
                    ensureBlankLine();
                    tableDepth++;
                    tableBufs.push_back(TableBuffer());
                    stack.push_back({CtxType::Table, {}, 0, false});
                    continue;
                }
                if (tagName == "tr") {
                    if (!tableBufs.isEmpty()) {
                        tableBufs.last().currentRow.clear();
                        tableBufs.last().currentIsHeader = false;
                    }
                    stack.push_back({CtxType::TableRow, {}, 0, false});
                    continue;
                }
                if (tagName == "th" || tagName == "td") {
                    bool isH = (tagName == "th");
                    if (!tableBufs.isEmpty()) {
                        tableBufs.last().cellBuf.clear();
                        if (isH) {
                            tableBufs.last().currentIsHeader = true;
                        }
                    }
                    stack.push_back({CtxType::TableCell, {}, 0, isH});
                    continue;
                }
                /* thead/tbody/tfoot — transparent, no context needed */
                if (tagName == "thead" || tagName == "tbody" || tagName == "tfoot") {
                    continue;
                }
                /* Block elements: p, div, section, etc. */
                if (tagName == "p" || tagName == "div" || tagName == "section"
                    || tagName == "article" || tagName == "header" || tagName == "footer"
                    || tagName == "nav" || tagName == "main" || tagName == "aside"
                    || tagName == "figure" || tagName == "figcaption" || tagName == "details"
                    || tagName == "summary") {
                    ensureBlankLine();
                    continue;
                }
                continue;
            }

            /* --- Self-closing tags --- */
            if (selfClose && !isClose) {
                if (tagName == "br") {
                    if (inCtx(stack, CtxType::Pre)) {
                        outputCh('\n');
                    } else {
                        output("  \n");
                    }
                    continue;
                }
                if (tagName == "hr") {
                    ensureBlankLine();
                    output("---\n");
                    continue;
                }
                if (tagName == "img") {
                    QString alt = extractAttr(tagBody, "alt");
                    QString src = extractAttr(tagBody, "src");
                    if (!src.isEmpty()) {
                        output("![" + alt + "](" + src + ")");
                    }
                    continue;
                }
                continue;
            }

            /* --- Close tags --- */
            if (isClose) {
                /* Code inside pre — skip */
                if (tagName == "code" && inCtx(stack, CtxType::Pre)) {
                    continue;
                }
                /* thead/tbody/tfoot — transparent */
                if (tagName == "thead" || tagName == "tbody" || tagName == "tfoot") {
                    continue;
                }

                /* Find matching context on stack */
                CtxType closeType = CtxType::Skip;
                if (tagName == "strong" || tagName == "b") {
                    closeType = CtxType::Bold;
                } else if (tagName == "em" || tagName == "i") {
                    closeType = CtxType::Italic;
                } else if (tagName == "code") {
                    closeType = CtxType::Code;
                } else if (tagName == "pre") {
                    closeType = CtxType::Pre;
                } else if (tagName == "a") {
                    closeType = CtxType::Anchor;
                } else if (
                    tagName.size() == 2 && tagName[0] == 'h' && tagName[1] >= '1'
                    && tagName[1] <= '6') {
                    closeType = CtxType::Heading;
                } else if (tagName == "ul" || tagName == "ol") {
                    closeType = CtxType::List;
                } else if (tagName == "li") {
                    closeType = CtxType::ListItem;
                } else if (tagName == "blockquote") {
                    closeType = CtxType::Blockquote;
                } else if (tagName == "table") {
                    closeType = CtxType::Table;
                } else if (tagName == "tr") {
                    closeType = CtxType::TableRow;
                } else if (tagName == "th" || tagName == "td") {
                    closeType = CtxType::TableCell;
                }

                if (closeType == CtxType::Skip) {
                    /* Block-level close tags: ensure newline */
                    if (tagName == "p" || tagName == "div" || tagName == "section"
                        || tagName == "article" || tagName == "header" || tagName == "footer"
                        || tagName == "nav" || tagName == "main" || tagName == "aside"
                        || tagName == "figure" || tagName == "figcaption" || tagName == "details"
                        || tagName == "summary") {
                        ensureBlankLine();
                    }
                    continue;
                }

                /* Search stack (up to 8 levels) for matching context */
                int found = -1;
                for (int s = stack.size() - 1; s >= 0 && s >= stack.size() - 8; s--) {
                    if (stack[s].type == closeType) {
                        found = s;
                        break;
                    }
                }
                if (found < 0) {
                    continue; /* No match, ignore */
                }

                Context ctx = stack[found];
                stack.remove(found);

                /* Emit closing markdown */
                switch (ctx.type) {
                case CtxType::Heading:
                    ensureNewline();
                    break;
                case CtxType::Bold:
                    output("**");
                    break;
                case CtxType::Italic:
                    output("*");
                    break;
                case CtxType::Code:
                    output("`");
                    break;
                case CtxType::Pre:
                    /* Ensure newline before closing fence */
                    if (tableDepth == 0 && !result.isEmpty() && !result.endsWith('\n')) {
                        result += '\n';
                    }
                    output("```\n");
                    break;
                case CtxType::Anchor:
                    output("](" + ctx.data + ")");
                    break;
                case CtxType::List:
                    ensureNewline();
                    break;
                case CtxType::ListItem:
                    ensureNewline();
                    break;
                case CtxType::Blockquote:
                    ensureNewline();
                    break;
                case CtxType::TableCell:
                    if (!tableBufs.isEmpty()) {
                        tableBufs.last().currentRow.push_back(tableBufs.last().cellBuf.trimmed());
                        tableBufs.last().cellBuf.clear();
                    }
                    break;
                case CtxType::TableRow:
                    if (!tableBufs.isEmpty()) {
                        tableBufs.last().rows.push_back(tableBufs.last().currentRow);
                        tableBufs.last().headerFlags.push_back(tableBufs.last().currentIsHeader);
                        tableBufs.last().currentRow.clear();
                    }
                    break;
                case CtxType::Table:
                    if (!tableBufs.isEmpty()) {
                        TableBuffer tb = tableBufs.last();
                        tableBufs.pop_back();
                        tableDepth--;
                        output(formatTable(tb));
                    }
                    break;
                default:
                    break;
                }
                continue;
            }
            continue;
        }

        /* Skip-depth */
        if (skipDepth > 0) {
            i++;
            continue;
        }

        /* HTML entity */
        if (ch == '&') {
            int semi = html.indexOf(';', i + 1);
            if (semi > 0 && semi - i < 12) {
                QString entity = html.mid(i + 1, semi - i - 1);
                i              = semi + 1;
                output(decodeEntity(entity));
                continue;
            }
        }

        /* Text content */
        if (inCtx(stack, CtxType::Pre)) {
            /* Preserve whitespace in <pre> */
            outputCh(ch);
            i++;
            continue;
        }

        /* Blockquote prefix for new lines */
        if (ch == '\n' && inCtx(stack, CtxType::Blockquote)) {
            outputCh('\n');
            output("> ");
            i++;
            /* Skip whitespace after newline */
            while (i < len && (html[i] == ' ' || html[i] == '\t' || html[i] == '\n')) {
                i++;
            }
            continue;
        }

        /* Collapse whitespace outside <pre> */
        if (ch == '\n' || ch == '\r' || ch == '\t' || ch == ' ') {
            /* Emit a single space if not already at whitespace */
            if (tableDepth > 0 && !tableBufs.isEmpty()) {
                if (!tableBufs.last().cellBuf.isEmpty() && !tableBufs.last().cellBuf.endsWith(' ')) {
                    tableBufs.last().cellBuf += ' ';
                }
            } else {
                if (!result.isEmpty() && !result.endsWith(' ') && !result.endsWith('\n')) {
                    result += ' ';
                }
            }
            i++;
            continue;
        }

        outputCh(ch);
        i++;
    }

    /* Collapse runs of blank lines (3+ newlines → 2) */
    QString collapsed;
    collapsed.reserve(result.size());
    int newlines = 0;
    for (QChar c : result) {
        if (c == '\n') {
            newlines++;
            if (newlines <= 2) {
                collapsed += c;
            }
        } else {
            newlines = 0;
            collapsed += c;
        }
    }

    return collapsed.trimmed();
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
        text = htmlToMarkdown(text);
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

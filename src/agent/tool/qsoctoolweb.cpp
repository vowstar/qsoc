// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "agent/tool/qsoctoolweb.h"

#include "common/qsocimageattach.h"
#include "common/qsocproxy.h"

#include <lexbor/dom/dom.h>
#include <lexbor/html/html.h>
#include <QNetworkRequest>
#include <QSet>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <cstring>

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
    QSocProxy::apply(networkManager, QSocProxy::fromLegacyConfig(config));
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

QSocToolWebFetch::QSocToolWebFetch(QObject *parent, QSocConfig *config, QLLMService *llm)
    : QSocTool(parent)
    , config(config)
    , llmService(llm)
{
    networkManager = new QNetworkAccessManager(this);
    setupProxy();
}

const char *QSocToolWebFetch::attachmentMarkerOpen()
{
    return QSocImageAttach::attachmentMarkerOpen();
}

const char *QSocToolWebFetch::attachmentMarkerClose()
{
    return QSocImageAttach::attachmentMarkerClose();
}

QSocToolWebFetch::~QSocToolWebFetch() = default;

QString QSocToolWebFetch::getName() const
{
    return "web_fetch";
}

QString QSocToolWebFetch::getDescription() const
{
    /* Mirror read_file's policy: only mention image-URL handling
     * when the active model accepts image content blocks. The agent
     * rebuilds tool definitions per request, so a model switch
     * picks up the right text on the next turn. */
    const bool image = !llmService.isNull() && llmService->currentSupportsImage();
    if (image) {
        return "Fetch content from a URL. HTML pages are converted to "
               "Markdown. Image URLs (PNG, JPG, GIF, WebP) are returned as "
               "visual content that the multimodal LLM can see directly, the "
               "same way as read_file on a local image. Returns the page "
               "content (truncated if too large).";
    }
    return "Fetch content from a URL. HTML pages are converted to "
           "Markdown. Returns the page content (truncated if too large).";
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
    QSocProxy::apply(networkManager, QSocProxy::fromLegacyConfig(config));
}

namespace {

const QSet<QString> &skipTags()
{
    static const QSet<QString> set = {
        QStringLiteral("script"),
        QStringLiteral("style"),
        QStringLiteral("svg"),
        QStringLiteral("noscript"),
        QStringLiteral("head"),
        QStringLiteral("template"),
        QStringLiteral("iframe"),
    };
    return set;
}

const QSet<QString> &blockTags()
{
    static const QSet<QString> set = {
        QStringLiteral("p"),
        QStringLiteral("div"),
        QStringLiteral("section"),
        QStringLiteral("article"),
        QStringLiteral("header"),
        QStringLiteral("footer"),
        QStringLiteral("nav"),
        QStringLiteral("main"),
        QStringLiteral("aside"),
        QStringLiteral("figure"),
        QStringLiteral("figcaption"),
        QStringLiteral("details"),
        QStringLiteral("summary"),
    };
    return set;
}

struct TableBuffer
{
    QVector<QVector<QString>> rows;
    QVector<bool>             headerFlags;
    QVector<QString>          currentRow;
    bool                      currentIsHeader = false;
    QString                   cellBuf;
};

QString formatTable(const TableBuffer &table)
{
    if (table.rows.isEmpty()) {
        return {};
    }
    int cols = 0;
    for (const auto &row : table.rows) {
        cols = qMax(cols, static_cast<int>(row.size()));
    }
    if (cols == 0) {
        return {};
    }
    QVector<int> widths(cols, 3);
    for (const auto &row : table.rows) {
        for (int idx = 0; idx < row.size(); ++idx) {
            widths[idx] = qMax(widths[idx], static_cast<int>(row[idx].size()));
        }
    }

    QString out;
    auto    formatRow = [&](const QVector<QString> &row) {
        out += QLatin1Char('|');
        for (int idx = 0; idx < cols; ++idx) {
            QString cell = (idx < row.size()) ? row[idx] : QString();
            cell.replace(QLatin1Char('|'), QStringLiteral("\\|"));
            out += QLatin1Char(' ');
            out += cell;
            for (int pad = widths[idx] - static_cast<int>(cell.size()); pad > 0; --pad) {
                out += QLatin1Char(' ');
            }
            out += QStringLiteral(" |");
        }
        out += QLatin1Char('\n');
    };
    auto formatSeparator = [&]() {
        out += QLatin1Char('|');
        for (int idx = 0; idx < cols; ++idx) {
            out += QLatin1Char(' ');
            for (int pad = 0; pad < widths[idx]; ++pad) {
                out += QLatin1Char('-');
            }
            out += QStringLiteral(" |");
        }
        out += QLatin1Char('\n');
    };

    int headerIdx = -1;
    for (int idx = 0; idx < table.rows.size(); ++idx) {
        if (idx < table.headerFlags.size() && table.headerFlags[idx]) {
            headerIdx = idx;
            break;
        }
    }
    if (headerIdx >= 0) {
        formatRow(table.rows[headerIdx]);
        formatSeparator();
        for (int idx = 0; idx < table.rows.size(); ++idx) {
            if (idx != headerIdx) {
                formatRow(table.rows[idx]);
            }
        }
    } else {
        formatRow(table.rows[0]);
        formatSeparator();
        for (int idx = 1; idx < table.rows.size(); ++idx) {
            formatRow(table.rows[idx]);
        }
    }
    return out;
}

QString collapseBlankLines(const QString &input)
{
    QString out;
    out.reserve(input.size());
    int newlines = 0;
    for (QChar character : input) {
        if (character == QLatin1Char('\n')) {
            ++newlines;
            if (newlines <= 2) {
                out += character;
            }
        } else {
            newlines = 0;
            out += character;
        }
    }
    return out;
}

class HtmlMarkdownEmitter
{
public:
    QString convert(const QString &html)
    {
        const QByteArray utf8 = html.toUtf8();
        if (utf8.isEmpty()) {
            return {};
        }
        lxb_html_document_t *doc = lxb_html_document_create();
        if (doc == nullptr) {
            return {};
        }
        /* Lexbor's parser implements WHATWG's error-recovery algorithm;
         * a non-OK status still leaves a usable tree, so we don't bail. */
        (void) lxb_html_document_parse(
            doc,
            reinterpret_cast<const lxb_char_t *>(utf8.constData()),
            static_cast<size_t>(utf8.size()));

        lxb_html_body_element_t *body = lxb_html_document_body_element(doc);
        if (body != nullptr) {
            walkChildren(lxb_dom_interface_node(body));
        }
        lxb_html_document_destroy(doc);
        return collapseBlankLines(result).trimmed();
    }

private:
    QString              result;
    QVector<TableBuffer> tableBufs;
    QStringList          listKinds;
    QVector<int>         olCounters;
    int                  tableDepth      = 0;
    int                  preDepth        = 0;
    int                  boldDepth       = 0;
    int                  italicDepth     = 0;
    int                  blockquoteDepth = 0;

    QString *outBuf()
    {
        if (tableDepth > 0 && !tableBufs.isEmpty()) {
            return &tableBufs.last().cellBuf;
        }
        return &result;
    }

    void put(const QString &str) { *outBuf() += str; }
    void put(QChar character) { *outBuf() += character; }

    void ensureNewline()
    {
        if (tableDepth > 0) {
            return;
        }
        if (!result.isEmpty() && !result.endsWith(QLatin1Char('\n'))) {
            result += QLatin1Char('\n');
        }
    }
    void ensureBlankLine()
    {
        if (tableDepth > 0 || result.isEmpty()) {
            return;
        }
        if (!result.endsWith(QLatin1Char('\n'))) {
            result += QLatin1Char('\n');
        }
        if (!result.endsWith(QStringLiteral("\n\n"))) {
            result += QLatin1Char('\n');
        }
    }

    static QString tagName(lxb_dom_node_t *node)
    {
        size_t            len  = 0;
        const lxb_char_t *name = lxb_dom_element_local_name(lxb_dom_interface_element(node), &len);
        if (name == nullptr) {
            return {};
        }
        return QString::fromUtf8(reinterpret_cast<const char *>(name), static_cast<int>(len));
    }
    static QString getAttr(lxb_dom_node_t *node, const char *attr)
    {
        size_t            len   = 0;
        const lxb_char_t *value = lxb_dom_element_get_attribute(
            lxb_dom_interface_element(node),
            reinterpret_cast<const lxb_char_t *>(attr),
            std::strlen(attr),
            &len);
        if (value == nullptr) {
            return {};
        }
        return QString::fromUtf8(reinterpret_cast<const char *>(value), static_cast<int>(len));
    }

    void walkChildren(lxb_dom_node_t *parent)
    {
        for (lxb_dom_node_t *child = lxb_dom_node_first_child(parent); child != nullptr;
             child                 = lxb_dom_node_next(child)) {
            walk(child);
        }
    }

    void walk(lxb_dom_node_t *node)
    {
        if (node->type == LXB_DOM_NODE_TYPE_TEXT) {
            handleText(node);
            return;
        }
        if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) {
            return;
        }
        const QString tag = tagName(node);
        if (tag.isEmpty() || skipTags().contains(tag)) {
            return;
        }

        if (tag.size() == 2 && tag[0] == QLatin1Char('h') && tag[1] >= QLatin1Char('1')
            && tag[1] <= QLatin1Char('6')) {
            ensureBlankLine();
            put(QString(tag[1].digitValue(), QLatin1Char('#')));
            put(QLatin1Char(' '));
            walkChildren(node);
            ensureNewline();
            return;
        }

        if (tag == QStringLiteral("strong") || tag == QStringLiteral("b")) {
            const bool wasOuter = (boldDepth == 0);
            if (wasOuter) {
                put(QStringLiteral("**"));
            }
            ++boldDepth;
            walkChildren(node);
            --boldDepth;
            if (wasOuter) {
                put(QStringLiteral("**"));
            }
            return;
        }
        if (tag == QStringLiteral("em") || tag == QStringLiteral("i")) {
            const bool wasOuter = (italicDepth == 0);
            if (wasOuter) {
                put(QLatin1Char('*'));
            }
            ++italicDepth;
            walkChildren(node);
            --italicDepth;
            if (wasOuter) {
                put(QLatin1Char('*'));
            }
            return;
        }
        if (tag == QStringLiteral("code") && preDepth == 0) {
            put(QLatin1Char('`'));
            walkChildren(node);
            put(QLatin1Char('`'));
            return;
        }
        if (tag == QStringLiteral("pre")) {
            ensureBlankLine();
            QString lang = getAttr(node, "class");
            if (lang.startsWith(QStringLiteral("language-"))) {
                lang = lang.mid(9);
            } else {
                lang.clear();
            }
            if (lang.isEmpty()) {
                /* GFM convention: <pre><code class="language-x">…</code></pre>.
                 * Fish the language out of the inner code if present. */
                for (lxb_dom_node_t *kid = lxb_dom_node_first_child(node); kid != nullptr;
                     kid                 = lxb_dom_node_next(kid)) {
                    if (kid->type != LXB_DOM_NODE_TYPE_ELEMENT) {
                        continue;
                    }
                    if (tagName(kid) != QStringLiteral("code")) {
                        continue;
                    }
                    const QString cls = getAttr(kid, "class");
                    if (cls.startsWith(QStringLiteral("language-"))) {
                        lang = cls.mid(9);
                    }
                    break;
                }
            }
            put(QStringLiteral("```") + lang + QLatin1Char('\n'));
            ++preDepth;
            walkChildren(node);
            --preDepth;
            ensureNewline();
            put(QStringLiteral("```\n"));
            return;
        }
        if (tag == QStringLiteral("a")) {
            const QString href = getAttr(node, "href");
            put(QLatin1Char('['));
            walkChildren(node);
            put(QStringLiteral("](") + href + QLatin1Char(')'));
            return;
        }
        if (tag == QStringLiteral("ul") || tag == QStringLiteral("ol")) {
            ensureNewline();
            listKinds.push_back(tag);
            if (tag == QStringLiteral("ol")) {
                olCounters.push_back(0);
            }
            walkChildren(node);
            listKinds.pop_back();
            if (tag == QStringLiteral("ol")) {
                olCounters.pop_back();
            }
            ensureNewline();
            return;
        }
        if (tag == QStringLiteral("li")) {
            ensureNewline();
            const int     depth = listKinds.size();
            const QString indent((depth > 0 ? depth - 1 : 0) * 2, QLatin1Char(' '));
            if (!listKinds.isEmpty() && listKinds.last() == QStringLiteral("ol")) {
                if (!olCounters.isEmpty()) {
                    ++olCounters.last();
                    put(indent + QString::number(olCounters.last()) + QStringLiteral(". "));
                }
            } else {
                put(indent + QStringLiteral("- "));
            }
            walkChildren(node);
            ensureNewline();
            return;
        }
        if (tag == QStringLiteral("blockquote")) {
            ensureBlankLine();
            put(QStringLiteral("> "));
            ++blockquoteDepth;
            walkChildren(node);
            --blockquoteDepth;
            ensureNewline();
            return;
        }
        if (tag == QStringLiteral("br")) {
            if (preDepth > 0) {
                put(QLatin1Char('\n'));
            } else {
                put(QStringLiteral("  \n"));
            }
            return;
        }
        if (tag == QStringLiteral("hr")) {
            ensureBlankLine();
            put(QStringLiteral("---\n"));
            return;
        }
        if (tag == QStringLiteral("img")) {
            const QString alt = getAttr(node, "alt");
            const QString src = getAttr(node, "src");
            if (!src.isEmpty()) {
                put(QStringLiteral("![") + alt + QStringLiteral("](") + src + QLatin1Char(')'));
            }
            return;
        }
        if (tag == QStringLiteral("table")) {
            ensureBlankLine();
            ++tableDepth;
            tableBufs.push_back(TableBuffer{});
            walkChildren(node);
            const TableBuffer table = tableBufs.last();
            tableBufs.pop_back();
            --tableDepth;
            put(formatTable(table));
            return;
        }
        if (tag == QStringLiteral("tr")) {
            if (!tableBufs.isEmpty()) {
                tableBufs.last().currentRow.clear();
                tableBufs.last().currentIsHeader = false;
            }
            walkChildren(node);
            if (!tableBufs.isEmpty()) {
                tableBufs.last().rows.push_back(tableBufs.last().currentRow);
                tableBufs.last().headerFlags.push_back(tableBufs.last().currentIsHeader);
                tableBufs.last().currentRow.clear();
            }
            return;
        }
        if (tag == QStringLiteral("th") || tag == QStringLiteral("td")) {
            const bool isHeader = (tag == QStringLiteral("th"));
            if (!tableBufs.isEmpty()) {
                tableBufs.last().cellBuf.clear();
                if (isHeader) {
                    tableBufs.last().currentIsHeader = true;
                }
            }
            walkChildren(node);
            if (!tableBufs.isEmpty()) {
                tableBufs.last().currentRow.push_back(tableBufs.last().cellBuf.trimmed());
                tableBufs.last().cellBuf.clear();
            }
            return;
        }

        if (blockTags().contains(tag)) {
            ensureBlankLine();
            walkChildren(node);
            ensureBlankLine();
            return;
        }

        /* Unknown / inline-but-unhandled (span, mark, font, ...): walk
         * through transparently so descendants still render. */
        walkChildren(node);
    }

    void handleText(lxb_dom_node_t *node)
    {
        size_t            len = 0;
        const lxb_char_t *raw = lxb_dom_node_text_content(node, &len);
        if (raw == nullptr || len == 0) {
            return;
        }
        const QString text
            = QString::fromUtf8(reinterpret_cast<const char *>(raw), static_cast<int>(len));
        if (text.isEmpty()) {
            return;
        }

        if (preDepth > 0) {
            put(text);
            return;
        }

        QString out;
        out.reserve(text.size());
        bool whitespace = false;
        for (QChar character : text) {
            if (character == QLatin1Char(' ') || character == QLatin1Char('\t')
                || character == QLatin1Char('\n') || character == QLatin1Char('\r')) {
                whitespace = true;
                continue;
            }
            if (whitespace && !out.isEmpty()) {
                out += QLatin1Char(' ');
            }
            whitespace = false;
            out += character;
        }
        if (whitespace && !out.isEmpty()) {
            out += QLatin1Char(' ');
        }
        if (out.isEmpty()) {
            return;
        }

        QString *buf = outBuf();
        if (out.startsWith(QLatin1Char(' '))
            && (buf->isEmpty() || buf->endsWith(QLatin1Char('\n'))
                || buf->endsWith(QLatin1Char(' ')))) {
            out.remove(0, 1);
        }
        if (out.isEmpty()) {
            return;
        }

        if (blockquoteDepth > 0 && tableDepth == 0) {
            out.replace(QLatin1Char('\n'), QStringLiteral("\n> "));
        }
        put(out);
    }
};

} // namespace

QString QSocToolWebFetch::htmlToMarkdown(const QString &html)
{
    /* Lexbor parses HTML5 per the WHATWG spec (full error-recovery)
     * and exposes a DOM we walk to emit Markdown. The previous hand-
     * rolled stream parser missed many real-world malformations. */
    HtmlMarkdownEmitter emitter;
    return emitter.convert(html);
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

    /* Image MIME path. SVG is XML so it stays on the text branch where
     * htmlToMarkdown can extract textual hints. Bitmap/raster MIMEs are
     * routed through handleImageResponse, which (a) sniffs dimensions
     * via QImageReader without decoding the full pixel buffer, (b)
     * estimates token cost via the model's configured provider hint, and
     * (c) either inlines as a base64 attachment marker or returns an
     * alt-text fallback when the active model is text-only or the image
     * exceeds the configured token budget after resize. */
    bool isImage = contentType.startsWith("image/") && !contentType.contains("svg+xml");
    if (isImage) {
        return handleImageResponse(urlStr, contentType, responseData);
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

QString QSocToolWebFetch::handleImageResponse(
    const QString &sourceUrl, const QString &contentType, const QByteArray &body)
{
    /* All image-encoding logic is shared with read_file's image branch;
     * the helper handles dimension sniff, capability gate, resize,
     * re-encode, and attachment marker construction in one place. */
    return QSocImageAttach::buildAttachmentResult(sourceUrl, contentType, body, llmService);
}

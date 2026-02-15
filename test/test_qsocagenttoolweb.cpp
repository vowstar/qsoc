// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "agent/tool/qsoctoolweb.h"
#include "common/qsocconfig.h"
#include "qsoc_test.h"

#include <QtCore>
#include <QtTest>

class Test : public QObject
{
    Q_OBJECT

private slots:
    /* Tool metadata */
    void testWebSearchName()
    {
        QSocToolWebSearch tool(this);
        QCOMPARE(tool.getName(), "web_search");
    }

    void testWebFetchName()
    {
        QSocToolWebFetch tool(this);
        QCOMPARE(tool.getName(), "web_fetch");
    }

    /* Schema validation */
    void testWebSearchSchemaValid()
    {
        QSocToolWebSearch tool(this);
        json              schema = tool.getParametersSchema();

        QVERIFY(schema.contains("type"));
        QCOMPARE(schema["type"].get<std::string>(), "object");
        QVERIFY(schema.contains("properties"));
        QVERIFY(schema["properties"].contains("query"));
        QVERIFY(schema.contains("required"));
        QVERIFY(schema["required"].is_array());

        bool hasQuery = false;
        for (const auto &req : schema["required"]) {
            if (req.get<std::string>() == "query") {
                hasQuery = true;
            }
        }
        QVERIFY(hasQuery);
    }

    void testWebFetchSchemaValid()
    {
        QSocToolWebFetch tool(this);
        json             schema = tool.getParametersSchema();

        QVERIFY(schema.contains("type"));
        QCOMPARE(schema["type"].get<std::string>(), "object");
        QVERIFY(schema.contains("properties"));
        QVERIFY(schema["properties"].contains("url"));
        QVERIFY(schema.contains("required"));
        QVERIFY(schema["required"].is_array());

        bool hasUrl = false;
        for (const auto &req : schema["required"]) {
            if (req.get<std::string>() == "url") {
                hasUrl = true;
            }
        }
        QVERIFY(hasUrl);
    }

    /* Parameter validation */
    void testWebSearchMissingQuery()
    {
        QSocToolWebSearch tool(this);
        json              args = json::object();

        QString result = tool.execute(args);
        QVERIFY(result.startsWith("Error:"));
        QVERIFY(result.contains("query"));
    }

    void testWebSearchNoApiUrl()
    {
        QSocToolWebSearch tool(this);
        json              args = {{"query", "test search"}};

        QString result = tool.execute(args);
        QVERIFY(result.startsWith("Error:"));
        QVERIFY(result.contains("web.search_api_url"));
    }

    void testWebSearchWithConfigNoApiUrl()
    {
        QSocConfig config(this);
        /* Ensure no API URL even if user config has one */
        config.setValue("web.search_api_url", "");
        QSocToolWebSearch tool(this, &config);
        json              args = {{"query", "test search"}};

        QString result = tool.execute(args);
        QVERIFY(result.startsWith("Error:"));
        QVERIFY(result.contains("web.search_api_url"));
    }

    void testWebFetchMissingUrl()
    {
        QSocToolWebFetch tool(this);
        json             args = json::object();

        QString result = tool.execute(args);
        QVERIFY(result.startsWith("Error:"));
        QVERIFY(result.contains("url"));
    }

    void testWebFetchInvalidUrl()
    {
        QSocToolWebFetch tool(this);
        json             args = {{"url", "not-a-valid-url"}};

        QString result = tool.execute(args);
        QVERIFY(result.contains("Error:"));
    }

    void testWebFetchUnsupportedScheme()
    {
        QSocToolWebFetch tool(this);
        json             args = {{"url", "ftp://example.com/file.txt"}};

        QString result = tool.execute(args);
        QVERIFY(result.contains("Error:"));
        QVERIFY(result.contains("http"));
    }

    /* htmlToMarkdown: basic */
    void testHtmlToMarkdownBasic()
    {
        QString result = QSocToolWebFetch::htmlToMarkdown("<p>Hello</p>");
        QCOMPARE(result.trimmed(), "Hello");
    }

    void testHtmlToMarkdownEmpty()
    {
        QString result = QSocToolWebFetch::htmlToMarkdown("");
        QVERIFY(result.isEmpty());
    }

    void testHtmlToMarkdownPlainText()
    {
        QString result = QSocToolWebFetch::htmlToMarkdown("plain text without tags");
        QCOMPARE(result.trimmed(), "plain text without tags");
    }

    /* htmlToMarkdown: entities */
    void testHtmlToMarkdownEntities()
    {
        QString result = QSocToolWebFetch::htmlToMarkdown("<p>&amp; &lt; &gt; &quot; &apos;</p>");
        QVERIFY(result.contains("&"));
        QVERIFY(result.contains("<"));
        QVERIFY(result.contains(">"));
        QVERIFY(result.contains("\""));
        QVERIFY(result.contains("'"));
    }

    void testHtmlToMarkdownNamedEntities()
    {
        QString result = QSocToolWebFetch::htmlToMarkdown(
            "&ndash; &mdash; &copy; &reg; &trade; &hellip; &bull;");
        QVERIFY(result.contains("\u2013"));
        QVERIFY(result.contains("\u2014"));
        QVERIFY(result.contains("\u00A9"));
        QVERIFY(result.contains("\u00AE"));
        QVERIFY(result.contains("\u2122"));
        QVERIFY(result.contains("\u2026"));
        QVERIFY(result.contains("\u2022"));
    }

    void testHtmlToMarkdownNumericEntity()
    {
        QString result = QSocToolWebFetch::htmlToMarkdown("&#169;");
        QVERIFY(result.contains("\u00A9"));
    }

    void testHtmlToMarkdownHexEntity()
    {
        QString result = QSocToolWebFetch::htmlToMarkdown("&#xA9;");
        QVERIFY(result.contains("\u00A9"));
    }

    /* htmlToMarkdown: headings */
    void testHtmlToMarkdownHeadings()
    {
        QString result = QSocToolWebFetch::htmlToMarkdown(
            "<h1>Title</h1><h2>Sub</h2><h3>Sub2</h3>"
            "<h4>Sub3</h4><h5>Sub4</h5><h6>Sub5</h6>");
        QVERIFY(result.contains("# Title"));
        QVERIFY(result.contains("## Sub"));
        QVERIFY(result.contains("### Sub2"));
        QVERIFY(result.contains("#### Sub3"));
        QVERIFY(result.contains("##### Sub4"));
        QVERIFY(result.contains("###### Sub5"));
    }

    /* htmlToMarkdown: bold and italic */
    void testHtmlToMarkdownBold()
    {
        QString result = QSocToolWebFetch::htmlToMarkdown("<strong>bold</strong>");
        QVERIFY(result.contains("**bold**"));

        result = QSocToolWebFetch::htmlToMarkdown("<b>bold</b>");
        QVERIFY(result.contains("**bold**"));
    }

    void testHtmlToMarkdownItalic()
    {
        QString result = QSocToolWebFetch::htmlToMarkdown("<em>italic</em>");
        QVERIFY(result.contains("*italic*"));

        result = QSocToolWebFetch::htmlToMarkdown("<i>italic</i>");
        QVERIFY(result.contains("*italic*"));
    }

    void testHtmlToMarkdownBoldItalicNested()
    {
        QString result = QSocToolWebFetch::htmlToMarkdown("<strong><em>bold italic</em></strong>");
        QVERIFY(result.contains("**"));
        QVERIFY(result.contains("*"));
        QVERIFY(result.contains("bold italic"));
    }

    /* htmlToMarkdown: inline code */
    void testHtmlToMarkdownInlineCode()
    {
        QString result = QSocToolWebFetch::htmlToMarkdown("<code>foo()</code>");
        QVERIFY(result.contains("`foo()`"));
    }

    /* htmlToMarkdown: code block */
    void testHtmlToMarkdownCodeBlock()
    {
        QString result = QSocToolWebFetch::htmlToMarkdown(
            "<pre><code>int x = 1;\nreturn x;</code></pre>");
        QVERIFY(result.contains("```"));
        QVERIFY(result.contains("int x = 1;"));
        QVERIFY(result.contains("return x;"));
    }

    void testHtmlToMarkdownCodeBlockWithLanguage()
    {
        QString result = QSocToolWebFetch::htmlToMarkdown(
            "<pre><code class=\"language-cpp\">int x = 1;</code></pre>");
        QVERIFY(result.contains("```cpp"));
        QVERIFY(result.contains("int x = 1;"));
    }

    /* htmlToMarkdown: links */
    void testHtmlToMarkdownLink()
    {
        QString result = QSocToolWebFetch::htmlToMarkdown(
            "<a href=\"https://example.com\">Example</a>");
        QVERIFY(result.contains("[Example](https://example.com)"));
    }

    void testHtmlToMarkdownLinkEmpty()
    {
        QString result = QSocToolWebFetch::htmlToMarkdown("<a href=\"\">text</a>");
        QVERIFY(result.contains("[text]()"));
    }

    /* htmlToMarkdown: images */
    void testHtmlToMarkdownImage()
    {
        QString result = QSocToolWebFetch::htmlToMarkdown("<img src=\"pic.png\" alt=\"A picture\">");
        QVERIFY(result.contains("![A picture](pic.png)"));
    }

    void testHtmlToMarkdownImageNoAlt()
    {
        QString result = QSocToolWebFetch::htmlToMarkdown("<img src=\"pic.png\">");
        QVERIFY(result.contains("![](pic.png)"));
    }

    /* htmlToMarkdown: unordered list */
    void testHtmlToMarkdownUnorderedList()
    {
        QString result = QSocToolWebFetch::htmlToMarkdown(
            "<ul><li>First</li><li>Second</li><li>Third</li></ul>");
        QVERIFY(result.contains("- First"));
        QVERIFY(result.contains("- Second"));
        QVERIFY(result.contains("- Third"));
    }

    /* htmlToMarkdown: ordered list */
    void testHtmlToMarkdownOrderedList()
    {
        QString result = QSocToolWebFetch::htmlToMarkdown(
            "<ol><li>One</li><li>Two</li><li>Three</li></ol>");
        QVERIFY(result.contains("1. One"));
        QVERIFY(result.contains("2. Two"));
        QVERIFY(result.contains("3. Three"));
    }

    /* htmlToMarkdown: nested list */
    void testHtmlToMarkdownNestedList()
    {
        QString result = QSocToolWebFetch::htmlToMarkdown(
            "<ul>"
            "<li>A<ul><li>A1</li><li>A2</li></ul></li>"
            "<li>B</li>"
            "</ul>");
        QVERIFY(result.contains("- A"));
        QVERIFY(result.contains("  - A1"));
        QVERIFY(result.contains("  - A2"));
        QVERIFY(result.contains("- B"));
    }

    /* htmlToMarkdown: blockquote */
    void testHtmlToMarkdownBlockquote()
    {
        QString result = QSocToolWebFetch::htmlToMarkdown("<blockquote>Quote text</blockquote>");
        QVERIFY(result.contains("> "));
        QVERIFY(result.contains("Quote text"));
    }

    /* htmlToMarkdown: horizontal rule */
    void testHtmlToMarkdownHorizontalRule()
    {
        QString result = QSocToolWebFetch::htmlToMarkdown("<p>Before</p><hr><p>After</p>");
        QVERIFY(result.contains("---"));
        QVERIFY(result.contains("Before"));
        QVERIFY(result.contains("After"));
    }

    /* htmlToMarkdown: br */
    void testHtmlToMarkdownBr()
    {
        QString result = QSocToolWebFetch::htmlToMarkdown("Line 1<br>Line 2");
        QVERIFY(result.contains("Line 1"));
        QVERIFY(result.contains("Line 2"));
    }

    /* htmlToMarkdown: table with thead */
    void testHtmlToMarkdownTableWithThead()
    {
        QString result = QSocToolWebFetch::htmlToMarkdown(
            "<table>"
            "<thead><tr><th>Name</th><th>Value</th></tr></thead>"
            "<tbody><tr><td>A</td><td>1</td></tr>"
            "<tr><td>B</td><td>2</td></tr></tbody>"
            "</table>");
        QVERIFY(result.contains("| Name"));
        QVERIFY(result.contains("| Value"));
        QVERIFY(result.contains("| ---"));
        QVERIFY(result.contains("| A"));
        QVERIFY(result.contains("| B"));
    }

    /* htmlToMarkdown: table without thead */
    void testHtmlToMarkdownTableNoThead()
    {
        QString result = QSocToolWebFetch::htmlToMarkdown(
            "<table>"
            "<tr><td>X</td><td>Y</td></tr>"
            "<tr><td>1</td><td>2</td></tr>"
            "</table>");
        QVERIFY(result.contains("| X"));
        QVERIFY(result.contains("| Y"));
        QVERIFY(result.contains("| ---"));
        QVERIFY(result.contains("| 1"));
        QVERIFY(result.contains("| 2"));
    }

    /* htmlToMarkdown: table with pipe in cell */
    void testHtmlToMarkdownTablePipeEscape()
    {
        QString result = QSocToolWebFetch::htmlToMarkdown(
            "<table><tr><th>A</th></tr><tr><td>a|b</td></tr></table>");
        QVERIFY(result.contains("a\\|b"));
    }

    /* htmlToMarkdown: skip tags */
    void testHtmlToMarkdownSkipTags()
    {
        QString result = QSocToolWebFetch::htmlToMarkdown(
            "<p>Before</p>"
            "<script>alert('xss');</script>"
            "<style>body{color:red;}</style>"
            "<p>After</p>");
        QVERIFY(!result.contains("alert"));
        QVERIFY(!result.contains("color:red"));
        QVERIFY(result.contains("Before"));
        QVERIFY(result.contains("After"));
    }

    void testHtmlToMarkdownSkipSvg()
    {
        QString result = QSocToolWebFetch::htmlToMarkdown(
            "<p>Text</p><svg><path d=\"M0 0\"/></svg><p>More</p>");
        QVERIFY(!result.contains("path"));
        QVERIFY(result.contains("Text"));
        QVERIFY(result.contains("More"));
    }

    void testHtmlToMarkdownSkipHead()
    {
        QString result = QSocToolWebFetch::htmlToMarkdown(
            "<html><head><title>T</title></head><body><p>Body</p></body></html>");
        QVERIFY(!result.contains("<title>"));
        QVERIFY(result.contains("Body"));
    }

    /* htmlToMarkdown: <pre> preserves whitespace */
    void testHtmlToMarkdownPreWhitespace()
    {
        QString result = QSocToolWebFetch::htmlToMarkdown("<pre>  line 1\n  line 2\n</pre>");
        QVERIFY(result.contains("  line 1\n  line 2"));
    }

    /* htmlToMarkdown: HTML comment */
    void testHtmlToMarkdownComment()
    {
        QString result = QSocToolWebFetch::htmlToMarkdown("<p>A</p><!-- comment --><p>B</p>");
        QVERIFY(!result.contains("comment"));
        QVERIFY(result.contains("A"));
        QVERIFY(result.contains("B"));
    }

    /* htmlToMarkdown: malformed HTML (unclosed tags) */
    void testHtmlToMarkdownMalformed()
    {
        /* Should not crash or hang */
        QString result = QSocToolWebFetch::htmlToMarkdown(
            "<p>Unclosed paragraph"
            "<div><strong>Unclosed bold"
            "<a href=\"x\">Unclosed link");
        QVERIFY(result.contains("Unclosed paragraph"));
        QVERIFY(result.contains("Unclosed bold"));
        QVERIFY(result.contains("Unclosed link"));
    }

    /* htmlToMarkdown: nested HTML */
    void testHtmlToMarkdownNested()
    {
        QString result = QSocToolWebFetch::htmlToMarkdown("<div><p>First</p><p>Second</p></div>");
        QVERIFY(result.contains("First"));
        QVERIFY(result.contains("Second"));
    }

    /* htmlToMarkdown: large input */
    void testHtmlToMarkdownLargeInput()
    {
        /* 500KB+ of repeated paragraphs — should not crash or timeout */
        QString html;
        html.reserve(600000);
        for (int i = 0; i < 10000; i++) {
            html += "<p>Paragraph " + QString::number(i) + " with some text content.</p>\n";
        }
        QString result = QSocToolWebFetch::htmlToMarkdown(html);
        QVERIFY(result.contains("Paragraph 0"));
        QVERIFY(result.contains("Paragraph 9999"));
        QVERIFY(result.size() > 1000);
    }

    /* htmlToMarkdown: collapse blank lines */
    void testHtmlToMarkdownCollapseBlankLines()
    {
        QString result = QSocToolWebFetch::htmlToMarkdown("<p>A</p><p></p><p></p><p></p><p>B</p>");
        /* Should not have more than 2 consecutive newlines */
        QVERIFY(!result.contains("\n\n\n"));
        QVERIFY(result.contains("A"));
        QVERIFY(result.contains("B"));
    }

    /* Tool definition format */
    void testWebSearchDefinition()
    {
        QSocToolWebSearch tool(this);
        json              definition = tool.getDefinition();

        QVERIFY(definition.contains("type"));
        QCOMPARE(definition["type"].get<std::string>(), "function");
        QVERIFY(definition.contains("function"));
        QCOMPARE(definition["function"]["name"].get<std::string>(), "web_search");
    }

    void testWebFetchDefinition()
    {
        QSocToolWebFetch tool(this);
        json             definition = tool.getDefinition();

        QVERIFY(definition.contains("type"));
        QCOMPARE(definition["type"].get<std::string>(), "function");
        QVERIFY(definition.contains("function"));
        QCOMPARE(definition["function"]["name"].get<std::string>(), "web_fetch");
    }

    /* Abort safety */
    void testAbortNoOp()
    {
        QSocToolWebSearch searchTool(this);
        QSocToolWebFetch  fetchTool(this);

        /* Should not crash when no request is in flight */
        searchTool.abort();
        fetchTool.abort();
    }

    void testWebSearchEmptyQuery()
    {
        QSocToolWebSearch tool(this);
        json              args = {{"query", ""}};

        QString result = tool.execute(args);
        QVERIFY(result.startsWith("Error:"));
        QVERIFY(result.contains("empty"));
    }
};

QSOC_TEST_MAIN(Test)
#include "test_qsocagenttoolweb.moc"

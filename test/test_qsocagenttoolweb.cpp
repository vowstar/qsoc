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
        QSocConfig        config(this);
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

    /* htmlToText */
    void testHtmlToTextBasic()
    {
        QString result = QSocToolWebFetch::htmlToText("<p>Hello</p>");
        QCOMPARE(result.trimmed(), "Hello");
    }

    void testHtmlToTextEntities()
    {
        QString result = QSocToolWebFetch::htmlToText("<p>&amp; &lt; &gt;</p>");
        QVERIFY(result.contains("&"));
        QVERIFY(result.contains("<"));
        QVERIFY(result.contains(">"));
    }

    void testHtmlToTextNested()
    {
        QString result = QSocToolWebFetch::htmlToText("<div><p>First</p><p>Second</p></div>");
        QVERIFY(result.contains("First"));
        QVERIFY(result.contains("Second"));
    }

    void testHtmlToTextEmpty()
    {
        QString result = QSocToolWebFetch::htmlToText("");
        QVERIFY(result.isEmpty());
    }

    void testHtmlToTextPlainText()
    {
        QString result = QSocToolWebFetch::htmlToText("plain text without tags");
        QCOMPARE(result.trimmed(), "plain text without tags");
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

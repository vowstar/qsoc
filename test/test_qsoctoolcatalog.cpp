// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocrequestusage.h"
#include "agent/qsoctoolcatalog.h"
#include "qsoc_test.h"
#include "qsoc_tool_discovery_corpus.h"

#include <QtTest>

namespace {
json definitions()
{
    json result = json::array();
    for (const auto &entry : QSocToolDiscoveryCorpus::entries()) {
        if (entry.allowed) {
            result.push_back(
                {{"type", "function"},
                 {"function",
                  {{"name", entry.name.toStdString()},
                   {"description", entry.description.toStdString()},
                   {"parameters", QSocToolDiscoveryCorpus::parameters()}}}});
        }
    }
    return result;
}

class Test : public QObject
{
    Q_OBJECT
private slots:
    void frozenSearchCorpus()
    {
        const QSocToolCatalog catalog(definitions(), "workspace-a");
        QCOMPARE(QSocToolDiscoveryCorpus::queries().size(), 20);
        QCOMPARE(QSocToolDiscoveryCorpus::entries().size(), 220);
        for (const auto &query : QSocToolDiscoveryCorpus::queries()) {
            const auto response = json::parse(
                catalog.query({{"operation", "search"}, {"query", query.text.toStdString()}})
                    .toStdString());
            QStringList names;
            for (const auto &tool : response.at("tools")) {
                names.append(QString::fromStdString(tool.at("name").get<std::string>()));
            }
            QCOMPARE(names, query.expected);
            QVERIFY(response.dump().find("restricted-marker") == std::string::npos);
        }
    }

    void wireBudgetAndModes()
    {
        const auto            all = definitions();
        const QSocToolCatalog catalog(all, "workspace-a");
        QCOMPARE(catalog.resolvedMode("direct"), QStringLiteral("direct"));
        QCOMPARE(catalog.resolvedMode("auto"), QStringLiteral("catalog"));
        QVERIFY(catalog.wireDefinitions("direct") == all);
        const auto directTokens = QSocRequestUsage::estimateText(QString::fromStdString(all.dump()));
        const auto catalogTokens = QSocRequestUsage::estimateText(
            QString::fromStdString(catalog.wireDefinitions("catalog").dump()));
        QVERIFY2(catalogTokens * 4 <= directTokens, "Fixed schemas exceed 25 percent of baseline");
        QCOMPARE(catalog.wireDefinitions("catalog").size(), 2U);
        const QSocToolCatalog small(json::array({all.front()}), "workspace-a");
        QCOMPARE(small.resolvedMode("auto"), QStringLiteral("direct"));
        QVERIFY(catalog.wireDefinitions("catalog") == catalog.wireDefinitions("catalog"));
    }

    void schemaAndVersion()
    {
        const auto            all = definitions();
        const QSocToolCatalog catalog(all, "workspace-a");
        const auto            described = json::parse(
            catalog.query({{"operation", "describe"}, {"name", "test_tool_000"}}).toStdString());
        QVERIFY(described.at("definition") == all.front());
        QCOMPARE(
            QString::fromStdString(described.at("schema_version").get<std::string>()),
            catalog.version());
        QCOMPARE(catalog.version(), QSocToolCatalog(all, "workspace-a").version());
        QVERIFY(catalog.version() != QSocToolCatalog(all, "workspace-b").version());
        auto changed                          = all;
        changed[0]["function"]["description"] = "changed";
        QVERIFY(catalog.version() != QSocToolCatalog(changed, "workspace-a").version());
        const auto denied = catalog.query(
            {{"operation", "describe"}, {"name", "restricted_tool_200"}});
        QVERIFY(denied.startsWith("Error:"));
        QVERIFY(!denied.contains("restricted"));
    }

    void unwrapAndReject()
    {
        const QSocToolCatalog catalog(definitions(), "workspace-a");
        QSocToolDispatchView  view;
        json                  call
            = {{"name", "test_tool_000"},
               {"schema_version", catalog.version().toStdString()},
               {"arguments_json", "{\"option_0\":\"test\"}"}};
        QVERIFY(catalog.unwrap(call, &view, 200).isEmpty());
        QCOMPARE(view.canonicalName, QStringLiteral("test_tool_000"));
        QVERIFY(view.finalArguments == json({{"option_0", "test"}}));
        for (const auto &name : {"tool_invoke", "tool_catalog", "restricted_tool_200", "unknown"}) {
            auto invalid    = call;
            invalid["name"] = name;
            QVERIFY(!catalog.unwrap(invalid, &view, 200).isEmpty());
        }
        auto stale              = call;
        stale["schema_version"] = "old";
        QVERIFY(!catalog.unwrap(stale, &view, 200).isEmpty());
        for (const auto &encoded : {"[]", "null", "1", "broken", "\"string\""}) {
            auto invalid              = call;
            invalid["arguments_json"] = encoded;
            QVERIFY(!catalog.unwrap(invalid, &view, 200).isEmpty());
        }
        call["arguments_json"] = json::object();
        QVERIFY(!catalog.unwrap(call, &view, 200).isEmpty());
    }

    void boundedParsing()
    {
        json parsed;
        QVERIFY(QSocToolCatalog::parseArguments("{\"text\":\"[{}]\\\"\"}", &parsed).isEmpty());
        QVERIFY(!QSocToolCatalog::parseArguments(QString(1024 * 1024 + 1, ' '), &parsed).isEmpty());
        const QString deep = "{\"value\":" + QString(64, '[') + "0" + QString(64, ']') + "}";
        QVERIFY(!QSocToolCatalog::parseArguments(deep, &parsed).isEmpty());
        QVERIFY(!QSocToolCatalog::parseArguments("{}", &parsed, 1024 * 1024).isEmpty());
        QVERIFY(!QSocToolCatalog::validateArguments(json::array()).isEmpty());
    }
};
} // namespace
QSOC_TEST_MAIN(Test)
#include "test_qsoctoolcatalog.moc"

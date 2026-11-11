// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocgeneratemanager.h"
#include "common/qsocprimitivereport.h"
#include "common/qsocprojectmanager.h"
#include "common/qsocsibling.h"
#include "gui/report/qsocprimitivereportlayout.h"
#include "gui/report/qsocreportengine.h"
#include "qsoc_test.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QXmlStreamReader>
#include <QtTest>

namespace {
QByteArray read(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QStringList kinds(const QJsonArray &parts)
{
    QStringList result;
    for (const auto &part : parts)
        result.append(part.toObject()["kind"].toString());
    return result;
}

void verifySheetNavigation(const QString &html, int targets, bool fanIn)
{
    const QRegularExpression articles(
        R"re(<article class="sheet" id="path-(\d+)"><h3>(.*?)</h3>)re");
    QMap<QString, QString> sheetTitles;
    auto                   matches = articles.globalMatch(html);
    while (matches.hasNext()) {
        const auto match = matches.next();
        sheetTitles.insert(match.captured(1), match.captured(2));
    }
    QVERIFY(!sheetTitles.isEmpty());
    QCOMPARE(html.count("class=\"sheet-navigation\""), sheetTitles.size());
    const QRegularExpression links(R"re(href="#path-(\d+)")re");
    matches = links.globalMatch(html);
    while (matches.hasNext())
        QVERIFY(sheetTitles.contains(matches.next().captured(1)));

    const auto               index = html.left(html.indexOf("<section id=\"paths\">"));
    const QRegularExpression rows(
        R"re(<tr><td><a href="#path-(\d+)">([^<]+)</a></td><td>.*?</td><td>(.*?)</td></tr>)re",
        QRegularExpression::DotMatchesEverythingOption);
    matches                 = rows.globalMatch(index);
    int           rowsFound = 0;
    QSet<QString> indexedSheets;
    while (matches.hasNext()) {
        const auto match = matches.next();
        ++rowsFound;
        QVERIFY(sheetTitles.value(match.captured(1)).contains(match.captured(2)));
        indexedSheets.insert(match.captured(1));
        auto sheetLinks = links.globalMatch(match.captured(3));
        while (sheetLinks.hasNext()) {
            const auto sheet = sheetLinks.next().captured(1);
            QVERIFY(sheetTitles.value(sheet).contains(match.captured(2)));
            indexedSheets.insert(sheet);
        }
        if (fanIn)
            QVERIFY(match.captured(3).section('<', 0, 0).contains('-'));
    }
    QCOMPARE(rowsFound, targets);
    QCOMPARE(indexedSheets.size(), sheetTitles.size());
}

void verifyPagePreview(const QByteArray &html, int expectedPages)
{
    const QByteArray startTag("<script type=\"application/json\" id=\"document-page-data\">");
    const auto       start = html.indexOf(startTag);
    QVERIFY(start >= 0);
    const auto end = html.indexOf("</script>", start);
    QVERIFY(end > start);
    QJsonParseError error;
    const auto      pages = QJsonDocument::fromJson(
                                html.mid(start + startTag.size(), end - start - startTag.size()), &error)
                                .array();
    QCOMPARE(error.error, QJsonParseError::NoError);
    QCOMPARE(pages.size(), expectedPages);
    for (qsizetype i = 0; i < pages.size(); ++i) {
        const auto encoded    = pages[i].toString().toLatin1();
        const auto compressed = QByteArray::fromBase64(encoded);
        QCOMPARE(compressed.toBase64(), encoded);
        const auto svg = qUncompress(compressed);
        QVERIFY(svg.startsWith("<svg"));
        QVERIFY(!svg.contains("<script"));
        QXmlStreamReader reader(svg);
        QString          text;
        while (!reader.atEnd()) {
            reader.readNext();
            if (reader.isStartElement() && reader.name() == QLatin1String("text"))
                text += reader.readElementText(QXmlStreamReader::IncludeChildElements) + '\n';
        }
        QVERIFY(!reader.hasError());
        QVERIFY2(
            text.contains(QString("%1 / %2").arg(i + 1).arg(expectedPages)),
            "Preview pages must retain their PDF page numbers");
    }
}
} // namespace

class Test : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase() { qputenv("QSOC_BIN_DIR", QSOC_REPORT_BIN_DIR); }
    void clockOrderAndMux();
    void clockBypassMatchesRtl();
    void clockOverviewAndLayout();
    void orderedCircuit();
    void crossingSemantics();
    void boundedPagination_data();
    void boundedPagination();
    void paginationValidation();
    void overviewKeepsModelOrder();
    void largeControllers_data();
    void largeControllers();
    void htmlSignalContinuations();
    void resetMatchesEffectivePrimitive();
    void powerDomainKinds();
    void headlessExportAndEscaping();
    void pdfTextRoundTrip();
    void sharedParametersRetainFullTargetNames_data();
    void sharedParametersRetainFullTargetNames();
    void parameterDefaultsKeepOverrides();
    void controlLabelSpacing_data();
    void controlLabelSpacing();
    void publication_data();
    void publication();
    void invalidRequest_data();
    void invalidRequest();
    void unsafeStageName();
};

void Test::clockOrderAndMux()
{
    QSocClockPrimitive generator;
    const auto         config = generator.parseClockConfig(YAML::Load(R"(
name: clocks
input: {osc: {freq: 24MHz}, pll: {freq: 800MHz}}
target:
  cpu_clk:
    reset: rst_n
    select: sel
    test_enable: test_en
    test_clock: test_clk
    icg: {enable: cpu_en}
    div: {default: 4, width: 4}
    inv: {}
    link:
      osc: {div: {default: 2, width: 3}}
      pll: {}
)"));
    QVERIFY(config.valid);
    const auto panel = QSocPrimitiveReport::clock(config)["panels"].toArray().first().toObject();
    QCOMPARE(panel["merge"].toObject()["kind"].toString(), "mux");
    QCOMPARE(panel["merge"].toObject()["label"].toString(), "GF");
    QCOMPARE(kinds(panel["tail"].toArray()), QStringList({"gate", "divider", "inverter"}));
    QCOMPARE(
        kinds(panel["branches"].toArray().first().toObject()["steps"].toArray()),
        QStringList({"divider"}));
    const QByteArray encoded = QJsonDocument(panel).toJson();
    QVERIFY(encoded.contains("MUX test clock"));
    QVERIFY(encoded.contains("test_clk"));
}

void Test::clockBypassMatchesRtl()
{
    QSocClockPrimitive generator;
    const auto         config = generator.parseClockConfig(YAML::Load(R"(
name: clocks
input: {osc: {freq: 24MHz}}
target:
  out_clk:
    div: {default: 1, width: 3, sta_guide: {}}
    link:
      osc: {div: {default: 1, width: 3, sta_guide: {}}}
  dynamic_clk:
    link:
      osc: {div: {default: 1, width: 3, value: ratio, sta_guide: {}}}
)"));
    QVERIFY(config.valid);
    const auto panels = QSocPrimitiveReport::clock(config)["panels"].toArray();
    const auto bypass = panels[0].toObject(), dynamic = panels[1].toObject();
    QVERIFY(bypass["branches"].toArray().first().toObject()["steps"].toArray().isEmpty());
    QCOMPARE(kinds(bypass["tail"].toArray()), QStringList{"divider"});
    QVERIFY(bypass["tail"].toArray().first().toObject()["guide"].toBool());
    const auto steps = dynamic["branches"].toArray().first().toObject()["steps"].toArray();
    QCOMPARE(kinds(steps), QStringList{"divider"});
    QVERIFY(steps.first().toObject()["guide"].toBool());
    const auto rtl = generator.generateControllerVerilog(config);
    QVERIFY(!rtl.contains("u_out_clk_osc_div"));
    QVERIFY(rtl.contains("u_out_clk_target_div"));
    QVERIFY(rtl.contains("u_dynamic_clk_osc_div"));
}

void Test::clockOverviewAndLayout()
{
    QSocClockPrimitive generator;
    const auto         config = generator.parseClockConfig(YAML::Load(R"(
name: clocks
input: {osc: {}, pll: {}}
target:
  sys:
    select: sel
    mux: {sta_guide: {}}
    link: {osc: {}, pll: {icg: {enable: en, sta_guide: {}}}}
  cpu:
    div: {default: 2, width: 3, sta_guide: {}}
    link: {sys: {}}
  bus: {link: {sys: {}}}
  peripheral:
    inv: {sta_guide: {}}
    link: {bus: {}}
)"));
    QVERIFY(config.valid);
    const auto    report = QSocPrimitiveReport::clock(config);
    const auto    graph  = report["graph"].toObject();
    const auto    nodes  = graph["nodes"].toArray();
    int           inputs = 0, guides = 0, shared = -1;
    QList<QSizeF> sizes;
    for (int i = 0; i < nodes.size(); ++i) {
        const auto node = nodes[i].toObject();
        inputs += node["kind"] == "input";
        guides += node["guide"].toBool();
        if (node["label"] == "sys") {
            QCOMPARE(node["kind"].toString(), "net");
            shared = i;
        }
        sizes.append({node["kind"] == "input" ? 160.0 : 80.0, 80});
    }
    QCOMPARE(inputs, 2);
    QCOMPARE(guides, 4);
    QVERIFY(shared >= 0);
    int consumers = 0;
    for (const auto &value : graph["edges"].toArray())
        consumers += value.toObject()["from"].toInt() == shared;
    QCOMPARE(consumers, 2);
    QVERIFY(QJsonDocument(report).toJson().contains("clk_in"));

    const auto layout = QSocReportLayout::layout(graph, sizes);
    QVERIFY2(layout.error.isEmpty(), qPrintable(layout.error));
    for (int i = 0; i < nodes.size(); ++i) {
        for (int j = i + 1; j < nodes.size(); ++j)
            QVERIFY(!layout.nodes[i].box.intersects(layout.nodes[j].box));
    }
    for (const auto &edge : layout.edges) {
        QCOMPARE(
            edge.path.first(), QSocReportLayout::port(layout.nodes[edge.from], false, edge.output));
        QCOMPARE(edge.path.last(), QSocReportLayout::port(layout.nodes[edge.to], true, edge.input));
        for (int i = 1; i < edge.path.size(); ++i) {
            const auto a = edge.path[i - 1], b = edge.path[i];
            QVERIFY(a.x() == b.x() || a.y() == b.y());
            const QRectF segment(
                qMin(a.x(), b.x()), qMin(a.y(), b.y()), qAbs(a.x() - b.x()), qAbs(a.y() - b.y()));
            for (int j = 0; j < nodes.size(); ++j) {
                if (j == edge.from || j == edge.to)
                    continue;
                QVERIFY(!layout.nodes[j]
                             .box.adjusted(0.01, 0.01, -0.01, -0.01)
                             .intersects(segment.adjusted(-0.001, -0.001, 0.001, 0.001)));
            }
        }
    }
    QJsonObject cycle = graph;
    cycle["edges"]    = QJsonArray{QJsonObject{{"from", 0}, {"to", 0}}};
    QVERIFY(!QSocReportLayout::layout(cycle, sizes).error.isEmpty());
}

void Test::orderedCircuit()
{
    QSocClockPrimitive generator;
    for (int count = 1; count <= 4; ++count) {
        YAML::Node config;
        config["name"]        = "clocks";
        config["test_enable"] = "test_en";
        for (int i = 0; i < count; ++i)
            config["input"][QString("source_%1").arg(i).toStdString()]["freq"] = "24MHz";
        auto target                = config["target"]["clk_out"];
        target["select"]           = "sel";
        target["reset"]            = "rst_n";
        target["test_clock"]       = "source_0";
        target["mux"]["sta_guide"] = YAML::Node(YAML::NodeType::Map);
        target["icg"]["enable"]    = "output_en";
        target["div"]["default"]   = 2;
        target["div"]["width"]     = 4;
        target["inv"]              = YAML::Node(YAML::NodeType::Map);
        for (int i = 0; i < count; ++i) {
            auto branch             = target["link"][QString("source_%1").arg(i).toStdString()];
            branch["icg"]["enable"] = QString("en_%1").arg(i).toStdString();
            if (i % 2 == 0) {
                branch["div"]["default"] = 2;
                branch["div"]["width"]   = 4;
                branch["inv"]            = YAML::Node(YAML::NodeType::Map);
            }
        }
        const auto parsed = generator.parseClockConfig(config);
        QVERIFY(parsed.valid);
        const auto    panels = QSocPrimitiveReport::clock(parsed)["panels"].toArray();
        const auto    graph  = QSocPrimitiveReport::graph(panels);
        QList<QSizeF> sizes;
        for (const auto &value : graph["nodes"].toArray()) {
            const auto kind = value.toObject()["kind"].toString();
            sizes.append(
                {kind == "input" || kind == "output" ? 140.0
                 : kind == "inverter"                ? 40.0
                                                     : 80.0,
                 88});
        }
        const auto result = QSocReportLayout::layout(graph, sizes);
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        int aliases = 0;
        for (int i = 0; i < result.nodes.size(); ++i) {
            const auto &node = result.nodes[i];
            if (node.data["kind"] == "input") {
                aliases += node.data["label"] == "source_0";
            }
            for (int j = i + 1; j < result.nodes.size(); ++j)
                QVERIFY(!node.box.intersects(result.nodes[j].box));
        }
        QCOMPARE(aliases, count == 1 ? 1 : 2);
        for (const auto &edge : result.edges) {
            QCOMPARE(
                edge.path.first(),
                QSocReportLayout::port(result.nodes[edge.from], false, edge.output));
            QCOMPARE(
                edge.path.last(), QSocReportLayout::port(result.nodes[edge.to], true, edge.input));
        }
        for (const auto &node : result.nodes) {
            if (node.data["kind"] != "mux")
                continue;
            for (int i = 1; i < node.data["inputs"].toInt(); ++i)
                QVERIFY(
                    QSocReportLayout::port(node, true, i - 1).y()
                    < QSocReportLayout::port(node, true, i).y());
        }
    }
}

void Test::crossingSemantics()
{
    QSocReportLayout::Edge horizontal;
    horizontal.from = 1;
    horizontal.path = QPolygonF{{0, 40}, {100, 40}};
    QSocReportLayout::Edge vertical;
    vertical.from = 2;
    vertical.path = QPolygonF{{50, 0}, {50, 80}};
    auto bridges  = QSocReportLayout::crossings({horizontal, vertical});
    QCOMPARE(bridges.size(), 1);
    QCOMPARE(bridges.first().position, QPointF(50, 40));
    QCOMPARE(bridges.first().edge, 0);
    vertical.from = horizontal.from;
    QVERIFY(QSocReportLayout::crossings({horizontal, vertical}).isEmpty());
    vertical.output = 1;
    QCOMPARE(QSocReportLayout::crossings({horizontal, vertical}).size(), 1);
}

void Test::boundedPagination_data()
{
    QTest::addColumn<bool>("overview");
    QTest::addColumn<bool>("cycle");
    QTest::newRow("detail") << false << false;
    QTest::newRow("overview") << true << false;
    QTest::newRow("overview-power-cycle") << true << true;
}

void Test::boundedPagination()
{
    QFETCH(bool, overview);
    QFETCH(bool, cycle);
    QSocReportLayout::Engine engine;
    QString                  error;
    engine.layout(
        QJsonObject{
            {"id", "invalid"},
            {"children", QJsonArray{QJsonObject{{"id", "valid"}, {"width", 10}, {"height", 10}}}},
            {"edges",
             QJsonArray{QJsonObject{
                 {"id", "edge"},
                 {"sources", QJsonArray{"missing"}},
                 {"targets", QJsonArray{"valid"}}}}}},
        error);
    QVERIFY(!error.isEmpty());
    QJsonArray    nodes{QJsonObject{{"kind", "input"}, {"label", "source"}}}, edges;
    QList<QSizeF> sizes{{160, 56}};
    for (int i = 1; i <= 14; ++i) {
        nodes.append(QJsonObject{{"kind", "divider"}, {"label", QString("DIV%1").arg(i)}});
        sizes.append({72, 48});
        edges.append(QJsonObject{{"from", i - 1}, {"to", i}});
    }
    nodes.append(QJsonObject{{"kind", "output"}, {"label", "target"}});
    sizes.append({160, 56});
    edges.append(QJsonObject{{"from", 14}, {"to", 15}});
    nodes.append(QJsonObject{{"kind", "input"}, {"label", "unused"}});
    sizes.append({160, 56});
    if (cycle) {
        auto node      = nodes[1].toObject();
        node["inputs"] = 2;
        nodes[1]       = node;
        edges.append(QJsonObject{{"from", 14}, {"to", 1}, {"input", 1}});
    }
    const QJsonObject
               graph{{"nodes", nodes}, {"edges", edges}, {"overview", overview}, {"power", cycle}};
    const auto sheets = QSocReportLayout::paginate(graph, sizes, engine, 7);
    if (!cycle) {
        const auto initial = QSocReportLayout::layout(graph, sizes, &engine);
        const auto reused  = QSocReportLayout::paginate(graph, sizes, engine, 7, &initial);
        QCOMPARE(reused.size(), sheets.size());
        for (qsizetype i = 0; i < sheets.size(); ++i) {
            const auto &expected = sheets[i];
            const auto &actual   = reused[i];
            QCOMPARE(actual.error, expected.error);
            QCOMPARE(actual.size, expected.size);
            QCOMPARE(actual.nodes.size(), expected.nodes.size());
            QCOMPARE(actual.edges.size(), expected.edges.size());
            for (qsizetype node = 0; node < expected.nodes.size(); ++node) {
                QCOMPARE(actual.nodes[node].data, expected.nodes[node].data);
                QCOMPARE(actual.nodes[node].box, expected.nodes[node].box);
            }
            for (qsizetype edge = 0; edge < expected.edges.size(); ++edge) {
                QCOMPARE(actual.edges[edge].from, expected.edges[edge].from);
                QCOMPARE(actual.edges[edge].to, expected.edges[edge].to);
                QCOMPARE(actual.edges[edge].input, expected.edges[edge].input);
                QCOMPARE(actual.edges[edge].output, expected.edges[edge].output);
                QCOMPARE(actual.edges[edge].style, expected.edges[edge].style);
                QCOMPARE(actual.edges[edge].path, expected.edges[edge].path);
            }
        }
        for (const auto &node : initial.nodes)
            QVERIFY(!node.data.contains("original"));
    }
    QVERIFY(sheets.size() > 1);
    QMap<int, int>        occurrences;
    QSet<QString>         inputs, outputs;
    QSet<QPair<int, int>> reconstructed;
    bool                  unused = false;
    for (const auto &sheet : sheets) {
        QVERIFY2(sheet.error.isEmpty(), qPrintable(sheet.error));
        QVERIFY(sheet.size.width() <= 944);
        QVERIFY(sheet.size.height() <= 448);
        for (const auto &node : sheet.nodes) {
            const int original = node.data["original"].toInt(-1);
            unused |= original == 16;
            if (original > 0 && original < 15)
                ++occurrences[original];
            if (node.data["continuation"].toBool())
                (node.data["kind"] == "input" ? inputs : outputs)
                    .insert(node.data["label"].toString());
        }
        for (const auto &edge : sheet.edges) {
            const auto from = sheet.nodes[edge.from].data, to = sheet.nodes[edge.to].data;
            const int  target = to["original"].toInt(-1);
            if (target < 1)
                continue;
            const int source = from["continuation"].toBool()
                                   ? from["label"].toString().split('.').at(1).toInt()
                                   : from["original"].toInt(-1);
            reconstructed.insert({source, target});
        }
    }
    QVERIFY(unused);
    QCOMPARE(inputs, outputs);
    QCOMPARE(occurrences.size(), 14);
    for (const int count : occurrences)
        QCOMPARE(count, 1);
    QCOMPARE(reconstructed.size(), cycle ? 16 : 15);
    for (int i = 1; i <= 15; ++i)
        QVERIFY(reconstructed.contains({i - 1, i}));
    if (cycle)
        QVERIFY(reconstructed.contains({14, 1}));
}

void Test::paginationValidation()
{
    QJsonArray    nodes, edges;
    QList<QSizeF> sizes;
    for (int i = 0; i < 9; ++i) {
        nodes.append(
            QJsonObject{{"kind", "overview"}, {"label", QString::number(i)}, {"inputs", 2}});
        sizes.append({160, 48});
        if (i > 0)
            edges.append(QJsonObject{{"from", i - 1}, {"to", i}});
    }
    QSocReportLayout::Engine engine;
    const auto               rejected = [&](const QJsonArray &links, const QString &expected) {
        const QJsonObject graph{{"nodes", nodes}, {"edges", links}, {"overview", true}};
        QCOMPARE(QSocReportLayout::validateGraph(graph, sizes), expected);
        QCOMPARE(QSocReportLayout::layout(graph, sizes, &engine).error, expected);
        const auto pages = QSocReportLayout::paginate(graph, sizes, engine, 4);
        QCOMPARE(pages.size(), 1);
        QCOMPARE(pages.first().error, expected);
    };
    auto bad = edges;
    bad.append(QJsonObject{{"from", 9}, {"to", 1}});
    rejected(bad, "Invalid report connection");
    bad = edges;
    bad.append(edges.first());
    rejected(bad, "Invalid or multiply driven report port");
    bad = edges;
    bad.append(QJsonObject{{"from", 8}, {"to", 0}, {"input", 1}});
    rejected(bad, "The report graph contains a cycle");
}

void Test::overviewKeepsModelOrder()
{
    QJsonArray nodes{
        QJsonObject{{"kind", "input"}, {"label", "source_a"}},
        QJsonObject{{"kind", "input"}, {"label", "source_b"}}};
    QJsonArray    edges;
    QList<QSizeF> sizes{{96, 32}, {96, 32}};
    for (int i = 0; i < 8; ++i) {
        nodes.append(
            QJsonObject{{"kind", "overview"}, {"label", QString("target_%1").arg(i)}, {"inputs", 2}});
        sizes.append({160, 44});
    }
    for (int i = 7; i >= 0; --i) {
        edges.append(QJsonObject{{"from", 0}, {"to", i + 2}, {"input", 0}});
        edges.append(QJsonObject{{"from", 1}, {"to", i + 2}, {"input", 1}});
    }
    const auto result = QSocReportLayout::layout(
        QJsonObject{{"nodes", nodes}, {"edges", edges}, {"overview", true}}, sizes);
    QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
    for (int i = 3; i < result.nodes.size(); ++i)
        QVERIFY(result.nodes[i].box.top() > result.nodes[i - 1].box.bottom());
}

void Test::largeControllers_data()
{
    QTest::addColumn<QString>("kind");
    QTest::addColumn<bool>("fanIn");
    QTest::addColumn<int>("divider");
    QTest::addColumn<int>("scale");
    QTest::newRow("clock-targets") << QString("clock") << false << 2 << 128;
    QTest::newRow("reset-targets") << QString("reset") << false << 2 << 128;
    QTest::newRow("power-domains") << QString("power") << false << 2 << 128;
    QTest::newRow("clock-fan-in") << QString("clock") << true << 2 << 128;
    QTest::newRow("wide-divider") << QString("clock") << true << 1000000007 << 128;
    QTest::newRow("clock-targets-384") << QString("clock") << false << 2 << 384;
    QTest::newRow("reset-targets-384") << QString("reset") << false << 2 << 384;
    QTest::newRow("power-domains-384") << QString("power") << false << 2 << 384;
    QTest::newRow("clock-fan-in-384") << QString("clock") << true << 2 << 384;
}

void Test::largeControllers()
{
    QFETCH(QString, kind);
    QFETCH(bool, fanIn);
    QFETCH(int, divider);
    QFETCH(int, scale);
    YAML::Node config;
    config["name"] = "controller";
    QJsonObject report;
    QString     last;
    if (kind == "power") {
        config["host_clock"] = "clk_ao";
        config["host_reset"] = "rst_ao_n";
        for (int i = 0; i < scale; ++i) {
            YAML::Node domain;
            last           = QString("domain_%1").arg(i, 3, 10, QChar('0'));
            domain["name"] = last.toStdString();
            if (i > 0) {
                YAML::Node dependency;
                dependency["name"]
                    = QString("domain_%1").arg((i - 1) / 2, 3, 10, QChar('0')).toStdString();
                dependency["type"] = i % 2 ? "hard" : "soft";
                domain["depend"].push_back(dependency);
            }
            config["domain"].push_back(domain);
        }
        QSocPowerPrimitive generator;
        const auto         parsed = generator.parsePowerConfig(config);
        QVERIFY(parsed.valid);
        report = QSocPrimitiveReport::power(parsed);
    } else {
        const int sources = fanIn ? scale : 4;
        for (int i = 0; i < sources; ++i) {
            const auto name = QString("source_%1").arg(i, 3, 10, QChar('0')).toStdString();
            config[kind == "clock" ? "input" : "source"][name][kind == "clock" ? "freq" : "active"]
                = kind == "clock" ? "24MHz" : (i % 2 ? "high" : "low");
        }
        for (int i = 0; i < (fanIn ? 1 : scale); ++i) {
            last        = QString("target_%1").arg(i, 3, 10, QChar('0'));
            auto target = config["target"][last.toStdString()];
            for (int j = 0; j < sources; ++j) {
                auto link
                    = target["link"][QString("source_%1").arg(j, 3, 10, QChar('0')).toStdString()];
                link = YAML::Node(YAML::NodeType::Map);
                if (fanIn) {
                    link["icg"]["enable"]  = QString("en_%1").arg(j).toStdString();
                    link["div"]["width"]   = 30;
                    link["div"]["default"] = divider;
                    link["inv"]            = YAML::Node(YAML::NodeType::Map);
                }
            }
            if (kind == "clock") {
                target["select"]           = QString("sel_%1").arg(i).toStdString();
                target["comment"]          = "中文时钟与复位";
                target["mux"]["sta_guide"] = YAML::Node(YAML::NodeType::Map);
                if (divider > 2) {
                    target["icg"]["enable"]  = "output_en";
                    target["div"]["width"]   = 30;
                    target["div"]["default"] = divider;
                    target["inv"]            = YAML::Node(YAML::NodeType::Map);
                }
            } else {
                target["async"]["clock"] = QString("clk_%1").arg(i).toStdString();
                target["async"]["stage"] = 3;
                target["active"]         = i % 2 ? "high" : "low";
            }
        }
        if (kind == "clock") {
            QSocClockPrimitive generator;
            const auto         parsed = generator.parseClockConfig(config);
            QVERIFY(parsed.valid);
            report = QSocPrimitiveReport::clock(parsed);
        } else {
            QSocResetPrimitive generator;
            const auto         parsed = generator.parseResetConfig(config);
            QVERIFY(parsed.valid);
            report = QSocPrimitiveReport::reset(parsed);
        }
    }
    QTemporaryDir directory(QDir::tempPath() + "/test_qsoc_report_scale_XXXXXX");
    QVERIFY(directory.isValid());
    QVERIFY(QSocPrimitiveReport::stage(directory.path(), "controller", report));
    QSocPrimitiveReport::render(directory.path());
    const auto html = read(directory.filePath("controller.html"));
    const auto pdf  = read(directory.filePath("controller.pdf"));
    QVERIFY(html.contains(last.toUtf8()));
    QVERIFY(pdf.startsWith("%PDF-"));
    QVERIFY2(html.size() < 20 * 1024 * 1024, "A bounded report must not serialize a dense poster map");
    verifySheetNavigation(QString::fromUtf8(html), fanIn ? 1 : scale, fanIn);
    const int overviewStart = html.indexOf("<h3>Connectivity overview</h3>");
    const int overviewEnd
        = html.indexOf("<p>" + QByteArray::number(fanIn ? 1 : scale) + " targets /", overviewStart);
    QVERIFY(overviewStart >= 0);
    QVERIFY(overviewEnd > overviewStart);
    const auto overview = html.mid(overviewStart, overviewEnd - overviewStart);
    const int  count    = fanIn ? 1 : scale;
    for (int i = 0; i < count; ++i) {
        const auto target
            = QString("%1_%2").arg(kind == "power" ? "domain" : "target").arg(i, 3, 10, QChar('0'));
        QVERIFY2(overview.contains(target.toUtf8()), qPrintable(target));
    }
    QVERIFY(overview.contains("Processing and control pins"));
    if (kind == "power")
        QVERIFY(html.contains("{target}"));
    if (fanIn) {
        const auto end   = html.indexOf("<section id=\"paths\">");
        const auto paths = html.mid(end, html.indexOf("<section id=\"parameters\">", end) - end);
        QVERIFY(paths.contains(QString("inputs %1-%2").arg(scale - 4).arg(scale - 1).toUtf8()));
        QVERIFY(paths.contains(QString(">%1</text>").arg(scale - 1).toUtf8()));
        QVERIFY(paths.contains(QString("source_%1").arg(scale - 1).toUtf8()));
    }
    const QString extractor = QStandardPaths::findExecutable("pdftotext");
    if (extractor.isEmpty())
        QSOC_TEST_MISSING_DEPENDENCY(QStringLiteral("pdftotext"));
    QProcess process;
    process.start(extractor, {"-bbox-layout", directory.filePath("controller.pdf"), "-"});
    QVERIFY(process.waitForFinished(30000));
    QVERIFY2(
        process.exitCode() == 0,
        qPrintable(extractor + ": " + QString::fromUtf8(process.readAllStandardError())));
    const auto xml = process.readAllStandardOutput();
    QVERIFY(xml.contains(last.toUtf8()));
    if (divider > 2)
        QVERIFY(xml.contains(QByteArray::number(divider)));
    QXmlStreamReader reader(xml);
    int              pages = 0;
    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement() && reader.name() == QLatin1String("page")) {
            ++pages;
            const auto attributes = reader.attributes();
            QVERIFY(qAbs(attributes.value("width").toDouble() - 842) < 2);
            QVERIFY(qAbs(attributes.value("height").toDouble() - 595) < 2);
        }
        if (reader.isStartElement() && reader.name() == QLatin1String("word")) {
            const auto attributes = reader.attributes();
            QVERIFY(attributes.value("xMin").toDouble() >= 0);
            QVERIFY(attributes.value("yMin").toDouble() >= 0);
            QVERIFY(attributes.value("xMax").toDouble() <= 842);
            QVERIFY(attributes.value("yMax").toDouble() <= 596);
        }
    }
    QVERIFY(!reader.hasError());
    QVERIFY(pages > 10);
    QVERIFY2(pages <= scale, qPrintable(QString("%1 pages for %2 signals").arg(pages).arg(scale)));
    verifyPagePreview(html, pages);
}

void Test::htmlSignalContinuations()
{
    QSocClockPrimitive generator;
    const auto         config = generator.parseClockConfig(YAML::Load(R"(
name: clocks
input: {osc: {freq: 24MHz}}
target:
  first_clk: {link: {osc: {}}}
  second_clk: {link: {first_clk: {}}}
  third_clk: {link: {second_clk: {}}}
)"));
    QVERIFY(config.valid);
    QTemporaryDir directory(QDir::tempPath() + "/test_qsoc_report_navigation_XXXXXX");
    QVERIFY(directory.isValid());
    QVERIFY(
        QSocPrimitiveReport::stage(directory.path(), "clocks", QSocPrimitiveReport::clock(config)));
    QSocPrimitiveReport::render(directory.path());
    const QString html = QString::fromUtf8(read(directory.filePath("clocks.html")));
    verifySheetNavigation(html, 3, false);
    QVERIFY(html.contains("class=\"signal-continuations\""));
    QVERIFY(html.contains("<td>first_clk</td><td>From</td><td><a href=\"#path-0\">"));
    QVERIFY(html.contains("<td>first_clk</td><td>To</td><td><a href=\"#path-1\">"));
    QVERIFY(html.contains("<td>second_clk</td><td>From</td><td><a href=\"#path-1\">"));
    QVERIFY(html.contains("<td>second_clk</td><td>To</td><td><a href=\"#path-2\">"));
}

void Test::resetMatchesEffectivePrimitive()
{
    QSocResetPrimitive generator;
    const auto         config = generator.parseResetConfig(YAML::Load(R"(
name: resets
source: {por_n: {active: low}, wdt: {active: high}}
target:
  cpu_rst:
    active: high
    async: {clock: cpu_clk, stage: 3}
    sync: {clock: cpu_clk, stage: 4}
    count: {clock: cpu_clk, cycle: 8}
    link:
      wdt: {count: {clock: sys_clk, cycle: 16}}
      por_n: {}
)"));
    QVERIFY(config.valid);
    const auto panel = QSocPrimitiveReport::reset(config)["panels"].toArray().first().toObject();
    QCOMPARE(panel["merge"].toObject()["kind"].toString(), "and");
    QCOMPARE(kinds(panel["tail"].toArray()), QStringList({"reset", "inverter"}));
    QCOMPARE(panel["tail"].toArray().first().toObject()["label"].toString(), "ASYNC");
    QCOMPARE(
        kinds(panel["branches"].toArray().first().toObject()["steps"].toArray()),
        QStringList({"inverter", "reset"}));
    const QString rtl = generator.generateControllerVerilog(config);
    QVERIFY(rtl.contains("qsoc_rst_sync"));
    QVERIFY(!rtl.contains("qsoc_rst_pipe"));
    QTemporaryDir directory(QDir::tempPath() + "/test_qsoc_report_polarity_XXXXXX");
    QVERIFY(directory.isValid());
    QVERIFY(
        QSocPrimitiveReport::stage(directory.path(), "resets", QSocPrimitiveReport::reset(config)));
    QSocPrimitiveReport::render(directory.path());
    const auto html = read(directory.filePath("resets.html"));
    QVERIFY(html.contains(">active high</text>"));
    QVERIFY(html.contains(">active low</text>"));
}

void Test::powerDomainKinds()
{
    QSocPowerPrimitive generator;
    const auto         config = generator.parsePowerConfig(YAML::Load(R"(
name: power
host_clock: clk_ao
host_reset: rst_ao_n
domain:
  - {name: ao}
  - {name: root, depend: []}
  - {name: gpu, depend: [{name: root, type: hard}, {name: ao, type: soft}]}
  - {name: ignored, depend: [{name: root, type: unknown}]}
  - {name: filtered, depend: [null]}
)"));
    QVERIFY(config.valid);
    using Kind = QSocPowerPrimitive::DomainKind;
    QCOMPARE(config.domains[0].kind, Kind::AlwaysOn);
    QCOMPARE(config.domains[1].kind, Kind::Root);
    QCOMPARE(config.domains[2].kind, Kind::Dependent);
    QCOMPARE(config.domains[4].kind, Kind::Root);
    const auto panels = QSocPrimitiveReport::power(config)["panels"].toArray();
    QVERIFY(panels[0].toObject()["subtitle"].toString().startsWith("AO"));
    QVERIFY(panels[1].toObject()["subtitle"].toString().startsWith("ROOT"));
    QCOMPARE(panels[2].toObject()["branches"].toArray().size(), 2);
    const auto ignored = panels[3].toObject();
    QCOMPARE(ignored["merge"].toObject()["kind"].toString(), "power");
    QCOMPARE(ignored["branches"].toArray().first().toObject()["source"].toString(), "en_ignored");
    QVERIFY(QJsonDocument(ignored).toJson().contains("Ignored dependency"));
}

void Test::sharedParametersRetainFullTargetNames_data()
{
    QTest::addColumn<int>("count");
    QTest::newRow("small") << 2;
    QTest::newRow("compact") << 8;
}

void Test::sharedParametersRetainFullTargetNames()
{
    QFETCH(int, count);
    const QString extractor = QStandardPaths::findExecutable("pdftotext");
    if (extractor.isEmpty())
        QSOC_TEST_MISSING_DEPENDENCY(QStringLiteral("pdftotext"));
    QTemporaryDir directory(QDir::tempPath() + "/test_qsoc_report_identity_XXXXXX");
    QVERIFY(directory.isValid());
    QSocClockPrimitive::ClockControllerConfig config;
    config.moduleName = "clocks";
    QSocClockPrimitive::ClockInput source;
    source.name = "osc";
    source.freq = "24MHz";
    config.inputs.append(source);
    QStringList names;
    for (int i = 0; i < count; ++i) {
        QSocClockPrimitive::ClockTarget target;
        target.name = "clock_branch_with_a_long_hierarchical_name_" + QString(64, 'x')
                      + QString::number(i);
        QSocClockPrimitive::ClockLink link;
        link.source = source.name;
        target.links.append(link);
        config.targets.append(target);
        names.append(target.name);
    }
    QVERIFY(
        QSocPrimitiveReport::stage(directory.path(), "clocks", QSocPrimitiveReport::clock(config)));
    QSocPrimitiveReport::render(directory.path());
    QProcess process;
    process.start(extractor, {"-raw", directory.filePath("clocks.pdf"), "-"});
    QVERIFY(process.waitForFinished(30000));
    QCOMPARE(process.exitCode(), 0);
    QString output = QString::fromUtf8(process.readAllStandardOutput());
    output.remove(QRegularExpression(QStringLiteral("\\s+")));
    for (const auto &name : names)
        QVERIFY2(output.contains(name), qPrintable(name));
}

void Test::parameterDefaultsKeepOverrides()
{
    YAML::Node config;
    config["name"]       = "power";
    config["host_clock"] = "clk_ao";
    config["host_reset"] = "rst_ao_n";
    for (int i = 0; i < 8; ++i) {
        YAML::Node domain;
        domain["name"] = QString("domain_%1").arg(i, 3, 10, QChar('0')).toStdString();
        domain["v_mv"] = 800 + 100 * (i % 4);
        if (i > 0) {
            YAML::Node dependency;
            dependency["name"] = "domain_000";
            dependency["type"] = "hard";
            domain["depend"].push_back(dependency);
        }
        config["domain"].push_back(domain);
    }
    QSocPowerPrimitive generator;
    const auto         parsed = generator.parsePowerConfig(config);
    QVERIFY(parsed.valid);
    auto report = QSocPrimitiveReport::power(parsed);
    auto panels = report["panels"].toArray();
    for (int i = 0; i < panels.size(); ++i) {
        auto panel   = panels[i].toObject();
        auto details = panel["details"].toArray();
        if (i + 1 < panels.size())
            details.append(QJsonObject{{"name", "Partial field"}, {"value", "same"}});
        details.append(QJsonObject{{"name", "Repeated field"}, {"value", "same"}});
        if (i + 2 == panels.size())
            details.append(QJsonObject{{"name", "Repeated field"}, {"value", "second"}});
        panel["details"] = details;
        panels[i]        = panel;
    }
    report["panels"] = panels;
    QTemporaryDir directory(QDir::tempPath() + "/test_qsoc_report_defaults_XXXXXX");
    QVERIFY(directory.isValid());
    QVERIFY(QSocPrimitiveReport::stage(directory.path(), "power", report));
    QSocPrimitiveReport::render(directory.path());
    const QString html       = QString::fromUtf8(read(directory.filePath("power.html")));
    const QString parameters = html.mid(html.indexOf("<section id=\"parameters\">"));
    const QString common     = parameters.left(parameters.indexOf("</details>"));
    const auto    rows       = [](const QString &body) {
        QMap<QString, QString>   result;
        const QRegularExpression pattern(R"re(<th scope="row">([^<]+)</th><td>([^<]*)</td>)re");
        auto                     matches = pattern.globalMatch(body);
        while (matches.hasNext()) {
            const auto       match = matches.next();
            QXmlStreamReader value("<value>" + match.captured(2) + "</value>");
            if (value.readNextStartElement())
                result.insert(match.captured(1), value.readElementText());
        }
        return result;
    };
    const auto defaults = rows(common);
    QVERIFY(common.contains("Target defaults"));
    QVERIFY(common.contains("Per-target rows override these defaults."));
    QCOMPARE(defaults.value("Control enable"), "en_{target}");
    QCOMPARE(defaults.value("Fault clear"), "clr_{target}");
    QVERIFY(!defaults.contains("Voltage"));
    QVERIFY(!defaults.contains("Partial field"));
    QVERIFY(!defaults.contains("Repeated field"));
    QCOMPARE(parameters.count("<th scope=\"row\">Voltage</th>"), 8);
    QCOMPARE(parameters.count("<th scope=\"row\">Partial field</th>"), 7);
    QCOMPARE(parameters.count("<th scope=\"row\">Repeated field</th>"), 9);
    const QRegularExpression
               ao(R"re(<summary>3\.\d+ domain_000</summary>(.*?)</details>)re",
                  QRegularExpression::DotMatchesEverythingOption);
    const auto exception = rows(ao.match(parameters).captured(1));
    QCOMPARE(exception.value("Control enable"), "1'b1");
    QCOMPARE(exception.value("Fault clear"), "1'b0");
    QVERIFY(!exception.value("Outputs").contains("sw_domain_000"));
    for (int i = 0; i < panels.size(); ++i) {
        const auto panel    = panels[i].toObject();
        QString    expanded = i == 0 ? exception.value("Outputs") : defaults.value("Outputs");
        expanded.replace("{target}", panel["title"].toString());
        QString expected;
        for (const auto &row : panel["details"].toArray()) {
            if (row.toObject()["name"] == "Outputs")
                expected = row.toObject()["value"].toString();
        }
        QCOMPARE(expanded, expected);
    }
}

void Test::headlessExportAndEscaping()
{
    QTemporaryDir directory(QDir::tempPath() + "/test_qsoc_report_XXXXXX");
    QVERIFY(directory.isValid());
    QSocClockPrimitive::ClockControllerConfig config;
    config.moduleName = "clocks";
    QSocClockPrimitive::ClockInput spare;
    spare.name = "spare_osc";
    spare.freq = "24MHz";
    config.inputs.append(spare);
    QSocClockPrimitive::ClockTarget target;
    target.name    = "cpu_clock_with_a_long_name_" + QString(500, 'x');
    target.comment = QString::fromUtf8("时钟与复位 <script>alert('x')</script> & \"notes\"");
    for (int i = 0; i < 13; ++i) {
        QSocClockPrimitive::ClockLink link;
        link.source = "source_" + QString::number(i);
        target.links.append(link);
    }
    config.targets.append(target);
    QVERIFY(
        QSocPrimitiveReport::stage(directory.path(), "clocks", QSocPrimitiveReport::clock(config)));
    QSocPrimitiveReport::render(directory.path());
    const QString    base = directory.filePath("clocks");
    const QByteArray html = read(base + ".html");
    const QByteArray pdf  = read(base + ".pdf");
    QVERIFY(html.size() > 1000);
    QVERIFY(html.contains("spare_osc (unused)"));
    QVERIFY(pdf.startsWith("%PDF-"));
    QVERIFY(html.contains("data:font/woff2;base64,"));
    QVERIFY(html.contains("The Android Open Source Project"));
    QVERIFY(html.contains("&lt;script&gt;"));
    QCOMPARE(html.count("<script>"), 1);
    QVERIFY(!html.contains("<script>alert("));
    const int  scriptStart = html.indexOf("<script>") + 8;
    const auto script = html.mid(scriptStart, html.indexOf("</script>", scriptStart) - scriptStart);
    const auto scriptHash = QCryptographicHash::hash(script, QCryptographicHash::Sha256).toBase64();
    QVERIFY(html.contains("script-src 'sha256-" + scriptHash + "'"));
    QVERIFY(!html.contains("script-src 'unsafe-inline'"));
    QVERIFY(html.contains(".pdf\" target=\"_blank\" rel=\"noopener\">Download PDF"));
    QVERIFY(html.contains("href=\"#document-preview\" data-document-page=\""));
    QVERIFY(html.contains("Complete document preview"));
    QCOMPARE(html.count("target=\"_blank\" rel=\"noopener\""), 1);
    QVERIFY(html.contains(QString::fromUtf8("时钟与复位").toUtf8()));
    QVERIFY(html.contains("Input 12 source"));
    QVERIFY(html.contains(target.name.toUtf8()));
    QVERIFY(html.contains(QString::fromUtf8("…").toUtf8()));
    const auto paths = html.mid(html.indexOf("<section id=\"paths\">"));
    QCOMPARE(paths.count("<svg"), 4);
    QVERIFY(!html.contains(">G01</text>"));
    QVERIFY(html.contains("<details class=\"parameters\""));
    const auto parameterStart = html.indexOf("<section id=\"parameters\">");
    QVERIFY(parameterStart >= 0);
    QVERIFY(!html.mid(parameterStart).contains("<details open"));
    QVERIFY(html.contains("<text"));
    QVERIFY(!html.contains("<image"));
    QVERIFY(html.contains("Content-Security-Policy"));
}

void Test::pdfTextRoundTrip()
{
    const QString extractor = QStandardPaths::findExecutable("pdftotext");
    const QString detacher  = QStandardPaths::findExecutable("pdfdetach");
    if (extractor.isEmpty() || detacher.isEmpty())
        QSOC_TEST_MISSING_DEPENDENCY(QStringLiteral("pdftotext and pdfdetach"));
    QTemporaryDir directory(QDir::tempPath() + "/test_qsoc_report_text_XXXXXX");
    QVERIFY(directory.isValid());
    QSocClockPrimitive generator;
    auto               config      = generator.parseClockConfig(YAML::Load(R"(
name: clocks
input: {osc: {freq: 24MHz}}
target: {cpu_clk: {link: {osc: {}}}}
)"));
    config.targets.first().comment = QString::fromUtf8(
        "0123456789 configuration 中文时钟与复位 电源");
    QVERIFY(
        QSocPrimitiveReport::stage(directory.path(), "clocks", QSocPrimitiveReport::clock(config)));
    QSocPrimitiveReport::render(directory.path());
    QProcess process;
    process.start(extractor, {"-layout", directory.filePath("clocks.pdf"), "-"});
    QVERIFY(process.waitForFinished(30000));
    QCOMPARE(process.exitStatus(), QProcess::NormalExit);
    QVERIFY2(
        process.exitCode() == 0,
        qPrintable(extractor + ": " + QString::fromUtf8(process.readAllStandardError())));
    const QByteArray text = process.readAllStandardOutput();
    QVERIFY(text.contains("0123456789"));
    QVERIFY(text.contains("configuration"));
    QVERIFY(text.contains(QString::fromUtf8("中文时钟与复位").toUtf8()));
    const QString notice = directory.filePath("font-license.txt");
    process.start(
        detacher,
        {"-savefile", "fonts-LICENSE.txt", "-o", notice, directory.filePath("clocks.pdf")});
    QVERIFY(process.waitForFinished(30000));
    QVERIFY2(
        process.exitCode() == 0,
        qPrintable(detacher + ": " + QString::fromUtf8(process.readAllStandardError())));
    QCOMPARE(read(notice), read(":/license/droid-fonts.txt"));
}

void Test::controlLabelSpacing_data()
{
    QTest::addColumn<bool>("clock");
    QTest::newRow("serial-reset") << false;
    QTest::newRow("test-mux-and-divider") << true;
}

void Test::controlLabelSpacing()
{
    QFETCH(bool, clock);
    const auto extractor = QStandardPaths::findExecutable("pdftotext");
    if (extractor.isEmpty())
        QSOC_TEST_MISSING_DEPENDENCY(QStringLiteral("pdftotext"));
    QJsonObject report;
    if (clock) {
        QSocClockPrimitive generator;
        const auto         config = generator.parseClockConfig(YAML::Load(R"(
name: clocks
test_enable: left_annotation_clock_000
input: {osc: {}, pll: {}}
target:
  clk_out:
    reset: rst_n
    select: functional_select
    test_clock: osc
    div: {default: 2, width: 4, value: right_annotation_clock_111}
    link: {osc: {}, pll: {}}
)"));
        QVERIFY(config.valid);
        report = QSocPrimitiveReport::clock(config);
    } else {
        QSocResetPrimitive generator;
        const auto         config = generator.parseResetConfig(YAML::Load(R"(
name: resets
source: {por_n: {active: low}}
target:
  rst_out_n:
    active: low
    async: {clock: right_annotation_clock_111, stage: 3}
    link:
      por_n: {async: {clock: left_annotation_clock_000, stage: 2}}
)"));
        QVERIFY(config.valid);
        report = QSocPrimitiveReport::reset(config);
    }
    QTemporaryDir directory(QDir::tempPath() + "/test_qsoc_report_labels_XXXXXX");
    QVERIFY(directory.isValid());
    QVERIFY(QSocPrimitiveReport::stage(directory.path(), "controller", report));
    QSocPrimitiveReport::render(directory.path());
    const auto html = read(directory.filePath("controller.html"));
    QVERIFY(html.contains("left_annotation_clock_000"));
    QVERIFY(html.contains("right_annotation_clock_111"));
    QProcess process;
    process.start(extractor, {"-bbox-layout", directory.filePath("controller.pdf"), "-"});
    QVERIFY(process.waitForFinished(30000));
    QVERIFY2(
        process.exitCode() == 0,
        qPrintable(extractor + ": " + QString::fromUtf8(process.readAllStandardError())));
    QXmlStreamReader      reader(process.readAllStandardOutput());
    QMap<QString, QRectF> labels;
    QMap<QString, int>    pages;
    int                   page = 0;
    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement() && reader.name() == QLatin1String("page"))
            ++page;
        if (!reader.isStartElement() || reader.name() != QLatin1String("word"))
            continue;
        const auto attributes = reader.attributes();
        const QRectF
            box(QPointF(attributes.value("xMin").toDouble(), attributes.value("yMin").toDouble()),
                QPointF(attributes.value("xMax").toDouble(), attributes.value("yMax").toDouble()));
        const auto word = reader.readElementText();
        if (!word.contains(QChar(0x2026)) || (!word.startsWith("left") && !word.startsWith("right")))
            continue;
        const auto key = word.startsWith("left") ? "left" : "right";
        labels.insert(key, box);
        pages.insert(key, page);
    }
    QVERIFY(!reader.hasError());
    QCOMPARE(labels.size(), 2);
    QCOMPARE(pages["left"], pages["right"]);
    QVERIFY(labels["left"].right() + 4 <= labels["right"].left());
}

void Test::publication_data()
{
    QTest::addColumn<bool>("missingRenderer");
    QTest::newRow("complete-package") << false;
    QTest::newRow("missing-renderer") << true;
}

void Test::publication()
{
    QFETCH(bool, missingRenderer);
    QTemporaryDir directory(QDir::tempPath() + "/test_qsoc_report_publish_XXXXXX");
    QVERIFY(directory.isValid());
    const QByteArray previous = qgetenv("QSOC_BIN_DIR");
    const auto       restore  = qScopeGuard([&]() { qputenv("QSOC_BIN_DIR", previous); });
    if (missingRenderer)
        qputenv("QSOC_BIN_DIR", directory.path().toUtf8());
    QSocProjectManager project;
    project.setCurrentPath(directory.path());
    QVERIFY(project.mkpath());
    QSocGenerateManager generator(nullptr, &project);
    QVERIFY(generator.setNetlistData(YAML::Load(R"(
clock:
  - name: clocks
    input: {osc: {freq: 24MHz}}
    target: {cpu_clk: {link: {osc: {}}}}
)")));
    QVERIFY(generator.processNetlist());
    QVERIFY(generator.generateVerilog("top"));
    const QDir docs(QDir(project.getOutputPath()).filePath("top/doc"));
    QCOMPARE(
        docs.entryList(QDir::Files),
        missingRenderer ? QStringList() : QStringList({"clocks.html", "clocks.pdf"}));
    QVERIFY(!read(QDir(project.getOutputPath()).filePath("top/rtl/top.v")).isEmpty());
}

void Test::invalidRequest_data()
{
    QTest::addColumn<bool>("qtOption");
    QTest::newRow("invalid-json") << false;
    QTest::newRow("qt-option") << true;
}

void Test::invalidRequest()
{
    QFETCH(bool, qtOption);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QFile request(directory.filePath("bad.json"));
    QVERIFY(request.open(QIODevice::WriteOnly));
    request.write("{}");
    request.close();
    QProcess process;
    process.start(
        QSocSibling::path("qsoc-gui"),
        {"--render-primitive-report",
         qtOption ? "-reverse" : request.fileName(),
         directory.filePath("bad")});
    QVERIFY(process.waitForFinished(30000));
    QCOMPARE(process.exitStatus(), QProcess::NormalExit);
    QCOMPARE(process.exitCode(), 1);
    QVERIFY(!QFileInfo::exists(directory.filePath("bad.pdf")));
}

void Test::unsafeStageName()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QVERIFY(!QSocPrimitiveReport::stage(directory.path(), "../escape", {}));
    QVERIFY(QDir(directory.path()).entryList(QDir::Files).isEmpty());
}

QSOC_TEST_MAIN(Test)
#include "test_qsocprimitivereport.moc"

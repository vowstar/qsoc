// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocgenerateprimitiveclock.h"
#include "common/qsocgenerateprimitivereset.h"
#include "qsoc_test.h"

#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QtCore>
#include <QtTest>

namespace {

constexpr double kTolerance = 0.011;

struct Block
{
    double x = 0;
    double y = 0;
    double w = 0;
    double h = 0;

    double centerY() const { return y + h / 2; }
};

QString readText(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

QMap<QString, Block> parseBlocks(const QString &typst)
{
    static const QRegularExpression pattern(QStringLiteral(
        "element\\.(?:block|multiplexer)\\(\\s*x: (-?[\\d.]+), y: (-?[\\d.]+), "
        "w: ([\\d.]+), h: ([\\d.]+),\\s*id: \"([^\"]+)\""));
    QMap<QString, Block>            blocks;
    for (auto it = pattern.globalMatch(typst); it.hasNext();) {
        const auto match = it.next();
        blocks.insert(
            match.captured(5),
            Block{
                match.captured(1).toDouble(),
                match.captured(2).toDouble(),
                match.captured(3).toDouble(),
                match.captured(4).toDouble()});
    }
    return blocks;
}

class Test : public QObject
{
    Q_OBJECT

private slots:
    void resetAndInputsAreStraightAndBubblesSitOnStubs();
    void clockLinkComponentMeetsItsMuxPort();
};

void Test::resetAndInputsAreStraightAndBubblesSitOnStubs()
{
    using R = QSocResetPrimitive;

    R::ResetControllerConfig config;
    config.sources
        = {{"por_rst_n", "low"}, {"wdt_rst", "high"}, {"sw_rst", "high"}, {"ext_n", "low"}};

    R::ResetTarget target;
    target.name   = "cpu_rst_n";
    target.active = "low";
    R::ResetLink asyncLink;
    asyncLink.source      = "por_rst_n";
    asyncLink.async.clock = "clk_sys";
    R::ResetLink directHigh;
    directHigh.source = "wdt_rst";
    R::ResetLink syncHigh;
    syncHigh.source     = "sw_rst";
    syncHigh.sync.clock = "clk_sys";
    R::ResetLink directLow;
    directLow.source = "ext_n";
    target.links     = {asyncLink, directHigh, syncHigh, directLow};
    config.targets   = {target};

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = QDir(dir.path()).filePath("reset.typ");
    R             primitive;
    QVERIFY(primitive.generateTypstDiagram(config, path));
    const QString typst = readText(path);
    QVERIFY(!typst.isEmpty());

    const auto blocks = parseBlocks(typst);
    QVERIFY(blocks.contains("cpu_rst_n_AND"));
    const Block andGate = blocks.value("cpu_rst_n_AND");
    const auto inside = [&andGate](double y) { return y > andGate.y && y < andGate.y + andGate.h; };

    /* Each AND input endpoint is (x, y); the gate edge or a bubble's west edge. */
    QList<QPointF> endpoints;

    static const QRegularExpression wirePattern(QStringLiteral(
        "wire\\.wire\\(\"[^\"]+\", \\(\\s*\"([^\"]+)-port-out\", \\((-?[\\d.]+), "
        "(-?[\\d.]+)\\)\\s*\\)\\)"));
    int                             wires = 0;
    for (auto it = wirePattern.globalMatch(typst); it.hasNext(); ++wires) {
        const auto match = it.next();
        QVERIFY2(blocks.contains(match.captured(1)), qPrintable(match.captured(1)));
        const double endY = match.captured(3).toDouble();
        QVERIFY2(
            qAbs(blocks.value(match.captured(1)).centerY() - endY) < kTolerance,
            qPrintable(match.captured(0)));
        endpoints.append({match.captured(2).toDouble(), endY});
    }
    QCOMPARE(wires, 2);

    /* A high-active source entering a link component is inverted at that
       component's input, so its stub ends at a bubble on the component edge. */
    QVERIFY(blocks.contains("cpu_rst_n_L2_SYNC"));
    const Block    syncComp = blocks.value("cpu_rst_n_L2_SYNC");
    QList<QPointF> compEndpoints;

    static const QRegularExpression stubPattern(QStringLiteral(
        "wire\\.stub\\(\\((-?[\\d.]+), (-?[\\d.]+)\\), \"west\", name: \"([^\"]+)\""));
    int                             stubs = 0;
    for (auto it = stubPattern.globalMatch(typst); it.hasNext();) {
        const auto    match = it.next();
        const QPointF point(match.captured(1).toDouble(), match.captured(2).toDouble());
        if (match.captured(3) == "sw_rst") {
            compEndpoints.append(point);
        } else {
            endpoints.append(point);
            ++stubs;
        }
    }
    QCOMPARE(stubs, 2);
    QCOMPARE(compEndpoints.size(), 1);
    QVERIFY(qAbs(compEndpoints.first().y() - syncComp.centerY()) < kTolerance);

    for (const QPointF &point : endpoints) {
        QVERIFY(inside(point.y()));
    }

    static const QRegularExpression circlePattern(
        QStringLiteral("draw\\.circle\\(\\((-?[\\d.]+), (-?[\\d.]+)\\), radius: ([\\d.]+)"));
    int bubbles = 0;
    for (auto it = circlePattern.globalMatch(typst); it.hasNext(); ++bubbles) {
        const auto   match  = it.next();
        const double cx     = match.captured(1).toDouble();
        const double cy     = match.captured(2).toDouble();
        const double radius = match.captured(3).toDouble();
        const bool   atAnd  = qAbs(cx + radius - andGate.x) < kTolerance;
        const bool   atComp = qAbs(cx + radius - syncComp.x) < kTolerance;
        QVERIFY(atAnd || atComp);
        bool onEndpoint = false;
        for (const QPointF &point : atAnd ? endpoints : compEndpoints) {
            onEndpoint = onEndpoint
                         || (qAbs(point.x() - (cx - radius)) < kTolerance
                             && qAbs(point.y() - cy) < kTolerance);
        }
        QVERIFY2(onEndpoint, qPrintable(match.captured(0)));
    }
    QCOMPARE(bubbles, 2);

    /* Non-inverted inputs land on the gate edge itself. */
    int onEdge = 0;
    for (const QPointF &point : endpoints) {
        onEdge += qAbs(point.x() - andGate.x) < kTolerance ? 1 : 0;
    }
    QCOMPARE(onEdge, 3);
}

void Test::clockLinkComponentMeetsItsMuxPort()
{
    using C = QSocClockPrimitive;

    C::ClockControllerConfig config;
    config.name       = "clkctl";
    config.moduleName = "clkctl";
    config.inputs     = {{"osc", "24MHz", {}, {}}};

    C::ClockTarget target;
    target.name   = "cpu_clk";
    target.freq   = "24MHz";
    target.select = "cpu_sel";
    C::ClockLink link;
    link.source         = "osc";
    link.icg.configured = true;
    link.icg.enable     = "cpu_en";
    target.links        = {link};
    config.targets      = {target};

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = QDir(dir.path()).filePath("clock.typ");
    C             primitive;
    QVERIFY(primitive.generateTypstDiagram(config, path));
    const QString typst = readText(path);

    static const QRegularExpression entriesPattern(
        QStringLiteral("id: \"cpu_clk_MUX\", fill: [^,]+, entries: (\\d+)"));
    const auto entriesMatch = entriesPattern.match(typst);
    QVERIFY(entriesMatch.hasMatch());
    const int entries = entriesMatch.captured(1).toInt();

    const auto blocks = parseBlocks(typst);
    QVERIFY(blocks.contains("cpu_clk_MUX"));
    QVERIFY(blocks.contains("cpu_clk_L0_ICG"));
    const Block  mux   = blocks.value("cpu_clk_MUX");
    const double portY = mux.y + mux.h * (1.0 - 0.5 / entries);
    QVERIFY(qAbs(blocks.value("cpu_clk_L0_ICG").centerY() - portY) < kTolerance);
}

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocgeneratetypstgeometry.moc"

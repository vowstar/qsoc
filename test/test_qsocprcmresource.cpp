// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocgeneratemanager.h"
#include "common/qsocgenerateprimitiveclock.h"
#include "common/qsocgenerateprimitivereset.h"
#include "common/qsocprojectmanager.h"
#include "qsoc_prcm_fixture.h"
#include "qsoc_test.h"

#include <QFile>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

namespace {

using Shape = QMap<QString, QPair<bool, int>>;

Shape rtlPorts(const QString &rtl)
{
    const QRegularExpression expression(
        R"(\b(input|output)\s+wire\s+(?:\[([0-9]+):0\]\s+)?([A-Za-z_][A-Za-z_0-9]*))");
    auto  match = expression.globalMatch(rtl);
    Shape shape;
    while (match.hasNext()) {
        const auto port  = match.next();
        const int  width = port.captured(2).isEmpty() ? 1 : port.captured(2).toInt() + 1;
        shape.insert(port.captured(3), {port.captured(1) == "input", width});
    }
    return shape;
}

bool save(const QString &path, const QString &text)
{
    QFile      file(path);
    const auto data = text.toUtf8();
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}

void checkCell(const QString &path, const QString &cell, const QString &contract)
{
    for (const auto &tool : {"sby", "yosys", "z3"}) {
        if (QStandardPaths::findExecutable(tool).isEmpty()) {
            QSOC_TEST_MISSING_DEPENDENCY(tool);
        }
    }
    QVERIFY(save(path + "/contract.sv", contract));
    const QString job = QString(R"([tasks]
prove
cover
[options]
prove: mode prove
cover: mode cover
depth 20
timeout 60
multiclock on
prove: aigsmt z3
[engines]
prove: abc pdr
cover: smtbmc z3
[script]
read -formal -D SYNTHESIS dut.v %1 contract.sv
prep -top contract
[files]
dut.v
%1
contract.sv
)")
                            .arg(cell);
    QVERIFY(save(path + "/cell.sby", job));
    QProcess process;
    process.setWorkingDirectory(path);
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(QStandardPaths::findExecutable("sby"), {"-f", "cell.sby"});
    QVERIFY(process.waitForStarted());
    QVERIFY(process.waitForFinished(90000));
    const auto output = process.readAll();
    QCOMPARE(process.exitStatus(), QProcess::NormalExit);
    QVERIFY2(process.exitCode() == 0, output.constData());
    for (const auto &task : {"prove", "cover"}) {
        QFile status(path + "/cell_" + task + "/status");
        QVERIFY(status.open(QIODevice::ReadOnly));
        QCOMPARE(status.readAll().simplified().split(' ').constFirst(), QByteArray("PASS"));
    }
}

class Test : public QObject
{
    Q_OBJECT

private slots:
    void gateBehavior()
    {
        QTemporaryDir directory(QDir::tempPath() + "/test_qsoc_prcm_gate-XXXXXX");
        QVERIFY(directory.isValid());
        QSocProjectManager project;
        project.setCurrentPath(directory.path());
        project.setOutputPath(directory.path());
        QSocGenerateManager manager(nullptr, &project);
        QSocClockPrimitive  generator(&manager);
        QString             rtl;
        QTextStream         stream(&rtl);
        QVERIFY(
            generator.generateClockController(YAML::Load(qsocPrcmDeclaration())["clock"][0], stream));
        QVERIFY(save(directory.filePath("dut.v"), rtl));
        checkCell(
            directory.path(),
            "clock_cell.v",
            R"(
module contract(input clk, input en, input por_n);
wire gated;
clock dut(.aon_clk(clk), .gate_en(en), .por_n(por_n), .periph_clk(gated));
reg history = 0;
always @($global_clock) begin
    history <= 1;
    if (!clk) assert(!gated);
    if (history && clk && $past(clk)) assert(gated == $past(gated));
    if (history && clk && !$past(clk)) assert(gated == $past(en));
    cover(history && clk && !$past(clk) && gated);
    cover(history && clk && !$past(clk) && !gated);
end
endmodule
)");
    }

    void resetBehavior_data()
    {
        QTest::addColumn<int>("stage");
        QTest::newRow("two-stage") << 2;
        QTest::newRow("three-stage") << 3;
    }

    void resetBehavior()
    {
        QFETCH(int, stage);
        QTemporaryDir directory(QDir::tempPath() + "/test_qsoc_prcm_reset-XXXXXX");
        QVERIFY(directory.isValid());
        auto node = YAML::Load(qsocPrcmDeclaration())["reset"][0];
        node["target"]["periph_n"]["async"]["stage"] = stage;
        QSocProjectManager project;
        project.setCurrentPath(directory.path());
        project.setOutputPath(directory.path());
        QSocGenerateManager manager(nullptr, &project);
        QSocResetPrimitive  generator(&manager);
        QString             rtl;
        QTextStream         stream(&rtl);
        QVERIFY(generator.generateResetController(node, stream));
        QVERIFY(save(directory.filePath("dut.v"), rtl));
        checkCell(
            directory.path(),
            "reset_cell.v",
            QString(R"(
module contract(input clk, input por_n, input hold_n);
localparam STAGE = %1;
wire rst_n = por_n & hold_n;
wire out_n;
reset dut(.periph_clk(clk), .por_n(por_n), .hold_n(hold_n), .periph_n(out_n));
reg seen_reset = 0;
reg history = 0;
reg released = 0;
reg [$clog2(STAGE+1)-1:0] count;
always @(posedge clk or negedge rst_n) begin
    if (!rst_n) count <= 0;
    else if (count < STAGE) count <= count + 1'b1;
end
always @($global_clock) begin
    history <= 1;
    if (!rst_n) seen_reset <= 1;
    if (history && !rst_n && !$past(rst_n)) assert(!out_n);
    if (seen_reset && rst_n && count < STAGE) assert(!out_n);
    if (seen_reset && rst_n && count == STAGE) assert(out_n);
    if (seen_reset && rst_n && count == STAGE && out_n) released <= 1;
    cover(seen_reset && rst_n && count == STAGE && out_n);
    cover(released && history && $past(out_n) && !rst_n && !out_n && !clk && !$past(clk));
end
endmodule
)")
                .arg(stage));
    }

    void clockPort_data()
    {
        QTest::addColumn<int>("width");
        QTest::newRow("one-bit-vector") << 1;
        QTest::newRow("four-bit-vector") << 4;
    }

    void clockPort()
    {
        QFETCH(int, width);
        auto declaration                                    = YAML::Load(R"(
name: clock
input:
  aon_clk: {}
target:
  periph_clk:
    icg: {enable: gate_en, reset: por_n}
    div: {default: 1, width: 4, value: ratio, valid: ratio_valid, ready: ratio_ready, count: count, reset: por_n}
    link: {aon_clk: {}}
)");
        declaration["target"]["periph_clk"]["div"]["width"] = width;
        QSocClockPrimitive generator;
        const auto         config = generator.parseClockConfig(declaration);
        QVERIFY(config.valid);
        Shape described;
        for (const auto &port : config.ports) {
            QVERIFY(!described.contains(port.name));
            described.insert(port.name, {port.isInput, port.width});
            QCOMPARE(port.packed, port.name == "ratio" || port.name == "count");
        }
        const Shape expected{
            {"aon_clk", {true, 1}},
            {"periph_clk", {false, 1}},
            {"gate_en", {true, 1}},
            {"por_n", {true, 1}},
            {"ratio", {true, width}},
            {"ratio_valid", {true, 1}},
            {"ratio_ready", {false, 1}},
            {"count", {false, width}}};
        QCOMPARE(described, expected);
        QString     rtl;
        QTextStream stream(&rtl);
        QVERIFY(generator.generateClockController(declaration, stream));
        QCOMPARE(rtlPorts(rtl), expected);
    }

    void resetPort()
    {
        const auto         declaration = YAML::Load(R"(
name: reset
test_enable: test_en
source:
  por_n: {active: low}
  sw_n: {active: low}
  watchdog_n: {active: low}
target:
  periph_n:
    active: low
    async: {clock: clk, stage: 2}
    link: {por_n: {}, sw_n: {}, watchdog_n: {}}
reason:
  clock: clk
  root_reset: por_n
  output: reason
  valid: reason_valid
  clear: reason_clear
)");
        QSocResetPrimitive generator;
        const auto         config = generator.parseResetConfig(declaration);
        QVERIFY(config.valid);
        const auto  ports = QSocResetPrimitive::describePorts(config);
        Shape       described;
        QStringList order;
        for (const auto &port : ports) {
            QVERIFY(!described.contains(port.name));
            described.insert(port.name, {port.isInput, port.width});
            order.append(port.name);
        }
        const Shape expected{
            {"clk", {true, 1}},
            {"por_n", {true, 1}},
            {"sw_n", {true, 1}},
            {"watchdog_n", {true, 1}},
            {"test_en", {true, 1}},
            {"reason_clear", {true, 1}},
            {"periph_n", {false, 1}},
            {"reason", {false, 2}},
            {"reason_valid", {false, 1}}};
        QCOMPARE(described, expected);
        QCOMPARE(
            order,
            (QStringList{
                "clk",
                "por_n",
                "sw_n",
                "watchdog_n",
                "test_en",
                "reason_clear",
                "periph_n",
                "reason",
                "reason_valid"}));
        QString     rtl;
        QTextStream stream(&rtl);
        QVERIFY(generator.generateResetController(declaration, stream));
        QCOMPARE(rtlPorts(rtl), expected);
    }

    void resetCascade()
    {
        const auto         declaration = YAML::Load(R"(
name: reset
source: {por_n: {active: low}}
target:
  parent_n: {active: low, link: {por_n: {}}}
  child_n: {active: low, async: {clock: clk, stage: 2}, link: {parent_n: {}}}
)");
        QSocResetPrimitive generator;
        const auto         config = generator.parseResetConfig(declaration);
        QVERIFY(config.valid);
        const auto ports = QSocResetPrimitive::describePorts(config);
        QCOMPARE(ports.size(), 4);
        for (const auto &port : ports) {
            if (port.name == "parent_n" || port.name == "child_n") {
                QVERIFY(!port.isInput);
            }
        }
        QString     rtl;
        QTextStream stream(&rtl);
        QVERIFY(generator.generateResetController(declaration, stream));
        const Shape expected{
            {"clk", {true, 1}},
            {"por_n", {true, 1}},
            {"parent_n", {false, 1}},
            {"child_n", {false, 1}}};
        QCOMPARE(rtlPorts(rtl), expected);
    }
};

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocprcmresource.moc"

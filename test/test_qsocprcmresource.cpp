// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocgenerateprimitiveclock.h"
#include "common/qsocgenerateprimitivereset.h"
#include "qsoc_test.h"

#include <QRegularExpression>
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

class Test : public QObject
{
    Q_OBJECT

private slots:
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

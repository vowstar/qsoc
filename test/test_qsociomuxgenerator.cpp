// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsociomuxgenerator.h"
#include "common/qsocmodulemanager.h"
#include "qsoc_test.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextStream>
#include <QtTest>

namespace {

QSocModuleDefinition makeDefinition(const QString &body, const QString &moduleName = "iomux0")
{
    QSocModuleManager manager;
    return manager.moduleYamlToDefinition("peripheral", moduleName, YAML::Load(body.toStdString()));
}

QString integrationBlock()
{
    return QString(R"(    integration:
      instance: u_iomux0
      clock: clk_iomux
      reset: rst_iomux_n
      control: iomux_control
      pad:
        input_value: pad_input_value
        input_enable: pad_input_enable
        output_value: pad_output_value
        output_enable: pad_output_enable
)");
}

QString sourceForConfig(quint32 pinCount, quint32 hsSlots, quint32 dataWidth, quint32 addressWidth)
{
    return QString(R"(generator:
    kind: iomux
    bus: axi4_lite
    data_width: %1
    address_width: %2
    pin_count: %3
    hs_slots: %4
%5    route: []
)")
        .arg(dataWidth)
        .arg(addressWidth)
        .arg(pinCount)
        .arg(hsSlots)
        .arg(integrationBlock());
}

QSocModuleDefinition makeValidDefinition()
{
    return makeDefinition(QString(R"(generator:
    kind: iomux
    bus: axi4_lite
    data_width: 32
    address_width: 12
    pin_count: 2
    hs_slots: 2
%1    route:
      - pin: 0
        slot: 0
        function: gpio0
        signal: data0
        input_value: {link: gpio0_input, bit: 0}
        input_enable: 1
        output_value: {link: gpio0_output, bit: 0}
        output_enable: {link: gpio0_enable, bit: 0}
      - pin: 0
        slot: 1
        function: uart0
        signal: tx
        output_value: {link: uart0_tx}
        output_enable: 1
      - pin: 1
        slot: 1
        function: uart0
        signal: rx
        input_value: {link: uart0_rx}
        input_enable: 1
)")
                              .arg(integrationBlock()));
}

QSocModuleDefinition makeSimulationDefinition()
{
    return makeDefinition(QString(R"(generator:
    kind: iomux
    bus: axi4_lite
    data_width: 32
    address_width: 8
    pin_count: 2
    hs_slots: 3
%1    route:
      - pin: 0
        slot: 0
        function: gpio0
        signal: data0
        input_value: {link: gpio0_in, bit: 0}
        input_enable: 1
        output_value: {link: gpio0_out, bit: 0}
        output_enable: {link: gpio0_oe, bit: 0}
      - pin: 0
        slot: 1
        function: uart0
        signal: tx
        output_value: {link: uart0_tx}
        output_enable: 1
      - pin: 1
        slot: 0
        function: spi0
        signal: mode
        output_value: {link: spi_mode, invert: true}
        output_enable: 1
      - pin: 1
        slot: 1
        function: uart0
        signal: rx
        input_value: {link: uart0_rx}
        input_enable: 1
)")
                              .arg(integrationBlock()));
}

void writeTextFile(const QString &path, const QString &text)
{
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
    QTextStream(&file) << text;
}

QString routingTestbench()
{
    return QString(R"VERILOG(module tb;
reg  [1:0] pad_in;
wire [1:0] pad_ie;
wire [1:0] pad_ov;
wire [1:0] pad_oe;
wire [5:0] tx_ie;
wire [5:0] tx_ov;
wire [5:0] tx_oe;
wire [5:0] rx_val;
reg  [1:0] sel0;
reg  [1:0] sel1;
reg        gpio0_out_r;
reg        gpio0_oe_r;
reg        uart0_tx_r;
reg        spi_mode_r;
wire       gpio0_in_w;
wire       uart0_rx_w;
integer    failures;

iomux0_conn u_conn (
    .tx_input_enable_o(tx_ie),
    .tx_output_value_o(tx_ov),
    .tx_output_enable_o(tx_oe),
    .rx_input_value_i(rx_val),
    .hs_p0_s0_input_value_o(gpio0_in_w),
    .hs_p0_s0_output_value_i(gpio0_out_r),
    .hs_p0_s0_output_enable_i(gpio0_oe_r),
    .hs_p0_s1_output_value_i(uart0_tx_r),
    .hs_p1_s0_output_value_i(spi_mode_r),
    .hs_p1_s1_input_value_o(uart0_rx_w)
);

iomux0_core u_core (
    .pad_input_value_i(pad_in),
    .pad_input_enable_o(pad_ie),
    .pad_output_value_o(pad_ov),
    .pad_output_enable_o(pad_oe),
    .tx_input_enable_i(tx_ie),
    .tx_output_value_i(tx_ov),
    .tx_output_enable_i(tx_oe),
    .rx_input_value_o(rx_val),
    .pin_0_select_i(sel0),
    .pin_1_select_i(sel1)
);

task check_value;
    input condition;
    input [255:0] label;
    begin
        if (condition !== 1'b1) begin
            failures = failures + 1;
            $display("CHECK_FAIL %0s", label);
        end
    end
endtask

initial begin
    failures = 0;
    pad_in = 2'b00;
    sel0 = 2'd0;
    sel1 = 2'd0;
    gpio0_out_r = 1'b1;
    gpio0_oe_r = 1'b1;
    uart0_tx_r = 1'b0;
    spi_mode_r = 1'b0;
    #1;
    check_value(pad_ie[0] === 1'b1, "p0 slot0 input_enable constant 1");
    check_value(pad_ov[0] === 1'b1, "p0 slot0 output_value follows link");
    check_value(pad_oe[0] === 1'b1, "p0 slot0 output_enable follows link");
    gpio0_out_r = 1'b0;
    #1;
    check_value(pad_ov[0] === 1'b0, "p0 slot0 output_value tracks source");
    uart0_tx_r = 1'b1;
    #1;
    check_value(pad_ov[0] === 1'b0, "unselected slot1 source is isolated");
    sel0 = 2'd1;
    #1;
    check_value(pad_ie[0] === 1'b0, "p0 slot1 omitted input_enable is 0");
    check_value(pad_ov[0] === 1'b1, "p0 slot1 output_value follows link");
    check_value(pad_oe[0] === 1'b1, "p0 slot1 output_enable constant 1");
    sel0 = 2'd2;
    #1;
    check_value({pad_ie[0], pad_ov[0], pad_oe[0]} === 3'b000, "undeclared slot2 drives zero");
    sel0 = 2'd3;
    #1;
    check_value({pad_ie[0], pad_ov[0], pad_oe[0]} === 3'b000, "invalid code 3 drives zero");
    spi_mode_r = 1'b0;
    #1;
    check_value(pad_ov[1] === 1'b1, "p1 slot0 invert drives 1 from 0");
    check_value(pad_oe[1] === 1'b1, "p1 slot0 output_enable constant 1");
    spi_mode_r = 1'b1;
    #1;
    check_value(pad_ov[1] === 1'b0, "p1 slot0 invert drives 0 from 1");
    pad_in = 2'b11;
    sel0 = 2'd3;
    sel1 = 2'd0;
    #1;
    check_value(gpio0_in_w === 1'b1, "p0 rx sink sees pad input");
    check_value(uart0_rx_w === 1'b1, "p1 rx sink sees pad with selector elsewhere");
    pad_in = 2'b00;
    #1;
    check_value(gpio0_in_w === 1'b0, "p0 rx sink tracks pad input");
    check_value(uart0_rx_w === 1'b0, "p1 rx sink tracks pad input");
    sel0 = 2'd0;
    #1;
    check_value(pad_ov[1] === 1'b0, "p1 pad ignores p0 selector change");
    check_value(pad_oe[1] === 1'b1, "p1 pad ignores p0 selector change");
    if (failures == 0)
        $display("TEST_PASS");
    else
        $display("TEST_FAIL count=%0d", failures);
    $finish;
end
endmodule
)VERILOG");
}

const QSocMmioFieldPlan *findField(
    const QSocMmioPlan &mmio, const QString &registerName, const QString &fieldName)
{
    for (const QSocMmioRegisterPlan &reg : mmio.registers) {
        if (reg.name != registerName) {
            continue;
        }
        for (const QSocMmioFieldPlan &field : reg.fields) {
            if (field.name == fieldName) {
                return &field;
            }
        }
    }
    return nullptr;
}

class Test : public QObject
{
    Q_OBJECT

private slots:
    void draftIsRecognizedAndIncomplete();
    void planPreservesSemantics();
    void omittedSlotCountMatchesExplicitDefault();
    void sourceOrderDoesNotChangeGeneratedVerilog();
    void selectorLayoutMatchesFrozenKnownAnswer_data();
    void selectorLayoutMatchesFrozenKnownAnswer();
    void pinCountBoundaryFollowsLaneFormula_data();
    void pinCountBoundaryFollowsLaneFormula();
    void slotCountSetsSelectorFieldWidth_data();
    void slotCountSetsSelectorFieldWidth();
    void apertureExceedingAddressWidthIsRejected();
    void reportListsRoutesAndLayout();
    void invalidSource_data();
    void invalidSource();
    void routingSimulationWhenIverilogIsAvailable();
};

void Test::draftIsRecognizedAndIncomplete()
{
    QSocModuleDefinition definition;
    definition.libraryName                  = "peripheral";
    definition.moduleName                   = "iomux0";
    definition.extraAttributes["generator"] = QSocIomuxGenerator::createDraftGenerator();

    QVERIFY(QSocIomuxGenerator::isIomux(definition));
    const QStringList errors = QSocIomuxGenerator::validate(definition);
    QCOMPARE(errors.size(), 2);
    QVERIFY(errors.at(0).startsWith("IOMUX_REQUIRED generator.integration"));
    QVERIFY(errors.at(1).startsWith("IOMUX_REQUIRED generator.pin_count"));
}

void Test::planPreservesSemantics()
{
    QSocIomuxPlan plan;
    QStringList   errors;
    QVERIFY2(
        QSocIomuxGenerator::buildPlan(makeValidDefinition(), &plan, &errors),
        qPrintable(errors.join('\n')));

    QCOMPARE(plan.moduleName, QString("iomux0"));
    QCOMPARE(plan.pinCount, 2U);
    QCOMPARE(plan.hsSlots, 2U);
    QCOMPARE(plan.integration.instance, QString("u_iomux0"));
    QCOMPARE(plan.integration.control, QString("iomux_control"));
    QCOMPARE(plan.integration.padOutputEnable, QString("pad_output_enable"));

    QCOMPARE(plan.routes.size(), 3);
    QCOMPARE(plan.routes.at(0).pin, 0U);
    QCOMPARE(plan.routes.at(0).slot, 0U);
    QCOMPARE(plan.routes.at(0).function, QString("gpio0"));
    QCOMPARE(plan.routes.at(0).inputValue.link, QString("gpio0_input"));
    QCOMPARE(plan.routes.at(0).inputValue.bit.value(), 0U);
    QCOMPARE(plan.routes.at(0).inputEnable.constant.value(), quint8(1));
    QCOMPARE(plan.routes.at(1).slot, 1U);
    QCOMPARE(plan.routes.at(1).outputValue.link, QString("uart0_tx"));
    QVERIFY(!plan.routes.at(1).outputValue.bit.has_value());
    QCOMPARE(plan.routes.at(2).pin, 1U);

    QCOMPARE(plan.mmio.moduleName, QString("iomux0_regs"));
    QCOMPARE(plan.mmio.dataWidth, 32U);
    QCOMPARE(plan.mmio.addressWidth, 12U);
    QCOMPARE(plan.mmio.registers.size(), 2);
    QCOMPARE(plan.mmio.registers.at(0).name, QString("capability"));
    QCOMPARE(plan.mmio.registers.at(0).byteOffset, quint64(0));
    QCOMPARE(plan.mmio.registers.at(1).name, QString("hs_select_0"));
    QCOMPARE(plan.mmio.registers.at(1).byteOffset, quint64(4));
    QCOMPARE(plan.mmio.registers.at(1).fields.size(), 2);
    QCOMPARE(plan.mmio.registers.at(1).fields.at(0).name, QString("pin_0_select"));
    QCOMPARE(plan.mmio.registers.at(1).fields.at(0).width, 1U);
    QCOMPARE(plan.mmio.registers.at(1).fields.at(0).lsb, 0U);
    QCOMPARE(plan.mmio.registers.at(1).fields.at(1).lsb, 4U);
    QCOMPARE(plan.mmio.registers.at(1).fields.at(1).outputPort, QString("pin_1_select_o"));

    const QSocMmioFieldPlan *pinCountField = findField(plan.mmio, "capability", "pin_count");
    QVERIFY(pinCountField != nullptr);
    QCOMPARE(pinCountField->constantValue.value(), quint64(2));
}

void Test::omittedSlotCountMatchesExplicitDefault()
{
    const QString explicitSource = sourceForConfig(9, 4, 32, 8);
    QString       omittedSource  = explicitSource;
    omittedSource.replace("    hs_slots: 4\n", QString());
    QVERIFY(omittedSource != explicitSource);

    QSocIomuxPlan explicitPlan;
    QSocIomuxPlan omittedPlan;
    QVERIFY(QSocIomuxGenerator::buildPlan(makeDefinition(explicitSource), &explicitPlan));
    QVERIFY(QSocIomuxGenerator::buildPlan(makeDefinition(omittedSource), &omittedPlan));
    QCOMPARE(omittedPlan.hsSlots, 4U);
    QVERIFY(explicitPlan == omittedPlan);

    QCOMPARE(
        QSocIomuxGenerator::generateCoreVerilog(omittedPlan),
        QSocIomuxGenerator::generateCoreVerilog(explicitPlan));
    QCOMPARE(
        QSocIomuxGenerator::generateConnVerilog(omittedPlan),
        QSocIomuxGenerator::generateConnVerilog(explicitPlan));
    QCOMPARE(
        QSocIomuxGenerator::generateReport(omittedPlan),
        QSocIomuxGenerator::generateReport(explicitPlan));
}

void Test::sourceOrderDoesNotChangeGeneratedVerilog()
{
    QSocIomuxPlan plan;
    QVERIFY(QSocIomuxGenerator::buildPlan(makeValidDefinition(), &plan));

    const QString reordered = QString(R"(generator:
    kind: iomux
    bus: axi4_lite
    data_width: 32
    address_width: 12
    pin_count: 2
    hs_slots: 2
%1    route:
      - pin: 1
        slot: 1
        function: uart0
        signal: rx
        input_enable: 1
        input_value: {link: uart0_rx}
      - pin: 0
        slot: 1
        function: uart0
        signal: tx
        output_enable: 1
        output_value: {link: uart0_tx}
      - pin: 0
        slot: 0
        function: gpio0
        signal: data0
        output_enable: {link: gpio0_enable, bit: 0}
        output_value: {link: gpio0_output, bit: 0}
        input_enable: 1
        input_value: {link: gpio0_input, bit: 0}
)")
                                  .arg(integrationBlock());
    QSocIomuxPlan reorderedPlan;
    QVERIFY(QSocIomuxGenerator::buildPlan(makeDefinition(reordered), &reorderedPlan));

    QVERIFY(plan == reorderedPlan);
    QCOMPARE(
        QSocIomuxGenerator::generateCoreVerilog(reorderedPlan),
        QSocIomuxGenerator::generateCoreVerilog(plan));
    QCOMPARE(
        QSocIomuxGenerator::generateConnVerilog(reorderedPlan),
        QSocIomuxGenerator::generateConnVerilog(plan));
    QCOMPARE(
        QSocIomuxGenerator::generateReport(reorderedPlan), QSocIomuxGenerator::generateReport(plan));
}

void Test::selectorLayoutMatchesFrozenKnownAnswer_data()
{
    QTest::addColumn<quint32>("pinCount");
    QTest::addColumn<quint32>("hsSlots");
    QTest::addColumn<quint32>("dataWidth");
    QTest::addColumn<int>("registerCount");
    QTest::addColumn<quint64>("lastOffset");
    QTest::addColumn<quint64>("capability");

    QTest::newRow("185-4-32") << 185U << 4U << 32U << 25 << quint64(0x60) << quint64(0x000400B9);
    QTest::newRow("185-4-64") << 185U << 4U << 64U << 13 << quint64(0x60) << quint64(0x000400B9);
    QTest::newRow("256-8-32") << 256U << 8U << 32U << 33 << quint64(0x80) << quint64(0x00080100);
    QTest::newRow("256-8-64") << 256U << 8U << 64U << 17 << quint64(0x80) << quint64(0x00080100);
}

void Test::selectorLayoutMatchesFrozenKnownAnswer()
{
    QFETCH(quint32, pinCount);
    QFETCH(quint32, hsSlots);
    QFETCH(quint32, dataWidth);
    QFETCH(int, registerCount);
    QFETCH(quint64, lastOffset);
    QFETCH(quint64, capability);

    QSocIomuxPlan plan;
    QStringList   errors;
    QVERIFY2(
        QSocIomuxGenerator::buildPlan(
            makeDefinition(sourceForConfig(pinCount, hsSlots, dataWidth, 8)), &plan, &errors),
        qPrintable(errors.join('\n')));

    QCOMPARE(plan.mmio.registers.size(), registerCount);
    QCOMPARE(plan.mmio.registers.constFirst().byteOffset, quint64(0));
    QCOMPARE(plan.mmio.registers.at(1).byteOffset, quint64(dataWidth / 8));
    QCOMPARE(plan.mmio.registers.constLast().byteOffset, lastOffset);

    const QSocMmioFieldPlan *pinCountField = findField(plan.mmio, "capability", "pin_count");
    const QSocMmioFieldPlan *hsSlotsField  = findField(plan.mmio, "capability", "hs_slots");
    QVERIFY(pinCountField != nullptr && hsSlotsField != nullptr);
    QCOMPARE(
        pinCountField->constantValue.value() | (hsSlotsField->constantValue.value() << 16),
        capability);

    const QString            lastSelector = QString("pin_%1_select").arg(pinCount - 1);
    const QSocMmioFieldPlan *lastField
        = findField(plan.mmio, plan.mmio.registers.constLast().name, lastSelector);
    QVERIFY(lastField != nullptr);
    const quint32 lanes = dataWidth / 4;
    QCOMPARE(lastField->lsb, ((pinCount - 1) % lanes) * 4);
    QCOMPARE(lastField->resetValue.value(), quint64(0));
    QCOMPARE(lastField->outputPort, QString("pin_%1_select_o").arg(pinCount - 1));
}

void Test::pinCountBoundaryFollowsLaneFormula_data()
{
    QTest::addColumn<quint32>("pinCount");
    for (const quint32 pinCount : {1U, 7U, 8U, 9U, 15U, 16U, 17U, 184U, 185U, 186U, 255U, 256U}) {
        QTest::newRow(qPrintable(QString::number(pinCount))) << pinCount;
    }
}

void Test::pinCountBoundaryFollowsLaneFormula()
{
    QFETCH(quint32, pinCount);
    for (const quint32 dataWidth : {32U, 64U}) {
        QSocIomuxPlan plan;
        QStringList   errors;
        QVERIFY2(
            QSocIomuxGenerator::buildPlan(
                makeDefinition(sourceForConfig(pinCount, 4, dataWidth, 12)), &plan, &errors),
            qPrintable(errors.join('\n')));
        const quint32 lanes = dataWidth / 4;
        const quint32 words = (pinCount + lanes - 1) / lanes;
        QCOMPARE(plan.mmio.registers.size(), int(words) + 1);
        QCOMPARE(plan.mmio.registers.constLast().byteOffset, quint64(words) * (dataWidth / 8));
        const QSocMmioFieldPlan *lastField = findField(
            plan.mmio,
            plan.mmio.registers.constLast().name,
            QString("pin_%1_select").arg(pinCount - 1));
        QVERIFY(lastField != nullptr);
        QCOMPARE(lastField->lsb, ((pinCount - 1) % lanes) * 4);
    }
}

void Test::slotCountSetsSelectorFieldWidth_data()
{
    QTest::addColumn<quint32>("hsSlots");
    QTest::addColumn<quint32>("fieldWidth");

    QTest::newRow("2") << 2U << 1U;
    QTest::newRow("3") << 3U << 2U;
    QTest::newRow("4") << 4U << 2U;
    QTest::newRow("5") << 5U << 3U;
    QTest::newRow("8") << 8U << 3U;
}

void Test::slotCountSetsSelectorFieldWidth()
{
    QFETCH(quint32, hsSlots);
    QFETCH(quint32, fieldWidth);

    QSocIomuxPlan plan;
    QVERIFY(
        QSocIomuxGenerator::buildPlan(makeDefinition(sourceForConfig(9, hsSlots, 32, 8)), &plan));
    const QSocMmioFieldPlan *field = findField(plan.mmio, "hs_select_1", "pin_8_select");
    QVERIFY(field != nullptr);
    QCOMPARE(field->width, fieldWidth);
    QCOMPARE(field->lsb, 0U);

    const QSocMmioFieldPlan *hsSlotsField = findField(plan.mmio, "capability", "hs_slots");
    QVERIFY(hsSlotsField != nullptr);
    QCOMPARE(hsSlotsField->constantValue.value(), quint64(hsSlots));
}

void Test::apertureExceedingAddressWidthIsRejected()
{
    QSocIomuxPlan plan;
    QStringList   errors;
    QVERIFY(!QSocIomuxGenerator::buildPlan(
        makeDefinition(sourceForConfig(256, 8, 32, 7)), &plan, &errors));
    QCOMPARE(plan, QSocIomuxPlan());
    QCOMPARE(errors.size(), 1);
    QVERIFY(errors.constFirst().startsWith("IOMUX_RANGE generator.address_width"));
    QVERIFY(errors.constFirst().contains("minimum address_width is 8"));
    QVERIFY(
        QSocIomuxGenerator::generateCoreVerilog(plan).isEmpty()
        && QSocIomuxGenerator::generateConnVerilog(plan).isEmpty()
        && QSocIomuxGenerator::generateReport(plan).isEmpty());

    QVERIFY(QSocIomuxGenerator::buildPlan(makeDefinition(sourceForConfig(256, 8, 32, 8)), &plan));

    QVERIFY(
        !QSocIomuxGenerator::buildPlan(makeDefinition(sourceForConfig(1, 2, 32, 2)), &plan, &errors));
    QVERIFY(errors.constFirst().contains("minimum address_width is 3"));
    QVERIFY(QSocIomuxGenerator::buildPlan(makeDefinition(sourceForConfig(1, 2, 32, 3)), &plan));
}

void Test::reportListsRoutesAndLayout()
{
    QSocIomuxPlan plan;
    QVERIFY(QSocIomuxGenerator::buildPlan(makeValidDefinition(), &plan));
    const QString report = QSocIomuxGenerator::generateReport(plan);

    QVERIFY(report.contains("IOMUX route report for iomux0"));
    QVERIFY(report.contains("pin_count: 2"));
    QVERIFY(report.contains("hs_slots: 2"));
    QVERIFY(report.contains("selector: 1-bit field in a fixed 4-bit lane per pin"));
    QVERIFY(report.contains("capability: 0x00020002 at offset 0x0"));
    QVERIFY(report.contains("reset: every selector resets to 0 and selects slot 0"));
    QVERIFY(report.contains("pin 0 selector word 0 lsb 0 offset 0x4"));
    QVERIFY(report.contains("  slot 1 function uart0 signal tx"));
    QVERIFY(report.contains("    input_value: no sink"));
    QVERIFY(report.contains("    output_value: link uart0_tx"));
    QVERIFY(report.contains("    input_enable: constant 1"));
    QVERIFY(report.contains("undeclared pin/slot pairs drive a zero tx bundle"));
}

void Test::invalidSource_data()
{
    QTest::addColumn<QString>("source");
    QTest::addColumn<QString>("expectedError");

    const QString basePrefix = QString(R"(generator:
    kind: iomux
    bus: axi4_lite
    pin_count: 2
    hs_slots: 2
%1)")
                                   .arg(integrationBlock());
    const auto    withRoute  = [basePrefix](const QString &route) {
        return basePrefix + "    route:\n" + route;
    };
    const QString minimalRoute = QString(R"(      - pin: 0
        slot: 0
        function: gpio0
        signal: data0
        output_enable: 1
)");

    QTest::newRow("missing-pin-count")
        << QString("generator:\n    kind: iomux\n    bus: axi4_lite\n%1    route: []\n")
               .arg(integrationBlock())
        << "IOMUX_REQUIRED generator.pin_count";
    QTest::newRow("pin-count-zero")
        << sourceForConfig(0, 2, 32, 8) << "IOMUX_RANGE generator.pin_count";
    QTest::newRow("pin-count-over-max")
        << sourceForConfig(257, 2, 32, 8) << "IOMUX_RANGE generator.pin_count";
    QTest::newRow("pin-count-quoted-string")
        << QString(basePrefix).replace("pin_count: 2", "pin_count: \"2\"") + "    route: []\n"
        << "IOMUX_TYPE generator.pin_count";
    QTest::newRow("pin-count-bool")
        << QString(basePrefix).replace("pin_count: 2", "pin_count: true") + "    route: []\n"
        << "IOMUX_TYPE generator.pin_count";
    QTest::newRow("pin-count-float")
        << QString(basePrefix).replace("pin_count: 2", "pin_count: 2.0") + "    route: []\n"
        << "IOMUX_TYPE generator.pin_count";
    QTest::newRow("hs-slots-one") << sourceForConfig(2, 1, 32, 8)
                                  << "IOMUX_RANGE generator.hs_slots";
    QTest::newRow("hs-slots-nine")
        << sourceForConfig(2, 9, 32, 8) << "IOMUX_RANGE generator.hs_slots";
    QTest::newRow("wrong-kind") << QString(basePrefix).replace("kind: iomux", "kind: mmio")
                                       + "    route: []\n"
                                << "IOMUX_KIND generator.kind";
    QTest::newRow("wrong-bus") << QString(basePrefix).replace("bus: axi4_lite", "bus: apb4")
                                      + "    route: []\n"
                               << "IOMUX_BUS generator.bus";
    QTest::newRow("unknown-generator-key") << basePrefix + "    layout: compact\n    route: []\n"
                                           << "IOMUX_UNSUPPORTED generator.layout";
    QTest::newRow("missing-integration") << QString(
        "generator:\n    kind: iomux\n    bus: axi4_lite\n    pin_count: 2\n    route: []\n")
                                         << "IOMUX_REQUIRED generator.integration";
    QTest::newRow("missing-pad") << QString(R"(generator:
    kind: iomux
    bus: axi4_lite
    pin_count: 2
    integration:
      instance: u_iomux0
      clock: clk_iomux
      reset: rst_iomux_n
      control: iomux_control
    route: []
)") << "IOMUX_REQUIRED generator.integration.pad";
    QTest::newRow("missing-route")
        << QString("generator:\n    kind: iomux\n    bus: axi4_lite\n    pin_count: 2\n%1")
               .arg(integrationBlock())
        << "IOMUX_REQUIRED generator.route";
    QTest::newRow("route-not-sequence") << basePrefix + "    route: {}\n"
                                        << "IOMUX_TYPE generator.route";
    QTest::newRow("pin-out-of-range")
        << withRoute(QString(minimalRoute).replace("pin: 0", "pin: 2"))
        << "IOMUX_RANGE generator.route[0].pin";
    QTest::newRow("slot-out-of-range")
        << withRoute(QString(minimalRoute).replace("slot: 0", "slot: 2"))
        << "IOMUX_RANGE generator.route[0].slot";
    QTest::newRow("duplicate-pin-slot")
        << withRoute(minimalRoute + minimalRoute) << "IOMUX_DUPLICATE generator.route[1]";
    QTest::newRow("duplicate-rx-sink") << withRoute(QString(R"(      - pin: 0
        slot: 0
        function: gpio0
        signal: data0
        input_value: {link: shared_in, bit: 3}
      - pin: 1
        slot: 0
        function: gpio0
        signal: data1
        input_value: {link: shared_in, bit: 3}
)")) << "IOMUX_DUPLICATE generator.route[1].input_value";
    QTest::newRow("no-role-key") << withRoute(QString(R"(      - pin: 0
        slot: 0
        function: gpio0
        signal: data0
)")) << "IOMUX_ROLE generator.route[0]";
    QTest::newRow("empty-function")
        << withRoute(QString(minimalRoute).replace("function: gpio0", "function: \"  \""))
        << "IOMUX_EMPTY generator.route[0].function";
    QTest::newRow("input-value-constant") << withRoute(QString(R"(      - pin: 0
        slot: 0
        function: gpio0
        signal: data0
        input_value: 1
)")) << "IOMUX_TYPE generator.route[0].input_value";
    QTest::newRow("hdl-constant") << withRoute(
        QString(minimalRoute).replace("output_enable: 1", "output_enable: 1'b1"))
                                  << "IOMUX_TYPE generator.route[0].output_enable";
    QTest::newRow("invert-not-bool") << withRoute(
        QString(minimalRoute).replace("output_enable: 1", "output_enable: {link: a, invert: 1}"))
                                     << "IOMUX_TYPE generator.route[0].output_enable.invert";
    QTest::newRow("endpoint-unknown-key")
        << withRoute(QString(minimalRoute)
                         .replace("output_enable: 1", "output_enable: {link: a, bits: \"[0]\"}"))
        << "IOMUX_UNSUPPORTED generator.route[0].output_enable.bits";
    QTest::newRow("endpoint-bad-identifier")
        << withRoute(
               QString(minimalRoute).replace("output_enable: 1", "output_enable: {link: 1bad}"))
        << "IOMUX_IDENTIFIER generator.route[0].output_enable.link";
    QTest::newRow("manual-port") << basePrefix + "    route: []\nport:\n  clk:\n    direction: in\n"
                                 << "IOMUX_MANUAL_SECTION module.port";
    QTest::newRow("manual-parameter")
        << basePrefix + "    route: []\nparameter:\n  WIDTH:\n    value: 8\n"
        << "IOMUX_MANUAL_SECTION module.parameter";
}

void Test::invalidSource()
{
    QFETCH(QString, source);
    QFETCH(QString, expectedError);

    QSocIomuxPlan plan;
    QStringList   errors;
    QVERIFY(!QSocIomuxGenerator::buildPlan(makeDefinition(source), &plan, &errors));
    QCOMPARE(plan, QSocIomuxPlan());
    bool found = false;
    for (const QString &error : errors) {
        found = found || error.startsWith(expectedError);
    }
    QVERIFY2(found, qPrintable(expectedError + "\nactual:\n" + errors.join('\n')));
}

void Test::routingSimulationWhenIverilogIsAvailable()
{
    const QString compiler = QStandardPaths::findExecutable("iverilog");
    const QString runtime  = QStandardPaths::findExecutable("vvp");
    if (compiler.isEmpty() || runtime.isEmpty()) {
        QSOC_TEST_MISSING_DEPENDENCY(QStringLiteral("iverilog and vvp"));
    }

    QSocIomuxPlan plan;
    QStringList   errors;
    QVERIFY2(
        QSocIomuxGenerator::buildPlan(makeSimulationDefinition(), &plan, &errors),
        qPrintable(errors.join('\n')));

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString corePath   = QDir(directory.path()).filePath("iomux0_core.v");
    const QString connPath   = QDir(directory.path()).filePath("iomux0_conn.v");
    const QString benchPath  = QDir(directory.path()).filePath("tb.v");
    const QString outputPath = QDir(directory.path()).filePath("iomux0.out");
    writeTextFile(corePath, QSocIomuxGenerator::generateCoreVerilog(plan));
    writeTextFile(connPath, QSocIomuxGenerator::generateConnVerilog(plan));
    writeTextFile(benchPath, routingTestbench());

    QProcess process;
    process.setWorkingDirectory(directory.path());
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(compiler, {"-g2001", "-s", "tb", "-o", outputPath, corePath, connPath, benchPath});
    QVERIFY(process.waitForStarted());
    QVERIFY(process.waitForFinished());
    QCOMPARE(process.exitStatus(), QProcess::NormalExit);
    const QByteArray compilerOutput = process.readAll();
    QVERIFY2(process.exitCode() == 0, compilerOutput.constData());

    QProcess simulation;
    simulation.setWorkingDirectory(directory.path());
    simulation.setProcessChannelMode(QProcess::MergedChannels);
    simulation.start(runtime, {outputPath});
    QVERIFY(simulation.waitForStarted());
    QVERIFY(simulation.waitForFinished());
    QCOMPARE(simulation.exitStatus(), QProcess::NormalExit);
    const QByteArray simulationOutput = simulation.readAll();
    QCOMPARE(simulation.exitCode(), 0);
    QVERIFY2(!simulationOutput.contains("TEST_FAIL"), simulationOutput.constData());
    QVERIFY2(!simulationOutput.contains("CHECK_FAIL"), simulationOutput.constData());
    QVERIFY2(simulationOutput.contains("TEST_PASS"), simulationOutput.constData());
}

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsociomuxgenerator.moc"

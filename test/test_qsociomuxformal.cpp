// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsociomuxformal.h"
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

struct CommandResult
{
    bool                 started    = false;
    bool                 finished   = false;
    QProcess::ExitStatus exitStatus = QProcess::CrashExit;
    int                  exitCode   = -1;
    QByteArray           output;
};

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

QSocModuleDefinition makeSmallDefinition()
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

QSocModuleDefinition makeTailDefinition()
{
    return makeDefinition(QString(R"(generator:
    kind: iomux
    bus: axi4_lite
    data_width: 32
    address_width: 8
    pin_count: 9
    hs_slots: 3
%1    route:
      - pin: 8
        slot: 1
        function: tail
        signal: oe
        output_enable: 1
)")
                              .arg(integrationBlock()));
}

QSocModuleDefinition makeSlotCountDefinition(quint32 hsSlots)
{
    return makeDefinition(QString(R"(generator:
    kind: iomux
    bus: axi4_lite
    data_width: 32
    address_width: 8
    pin_count: 3
    hs_slots: %2
%1    route:
      - pin: 0
        slot: 0
        function: gpio0
        signal: data0
        input_value: {link: gpio0_in, bit: 0}
        input_enable: 1
        output_value: {link: gpio0_out, bit: 0}
        output_enable: {link: gpio0_oe, bit: 0}
      - pin: 2
        slot: 1
        function: tail
        signal: oe
        input_value: {link: tail_in, invert: true}
        output_enable: 1
)")
                              .arg(integrationBlock())
                              .arg(hsSlots));
}

void writeTextFile(const QString &path, const QString &text)
{
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
    QTextStream(&file) << text;
}

CommandResult runCommand(
    const QString &workingDirectory, const QString &program, const QStringList &arguments)
{
    QProcess process;
    process.setWorkingDirectory(workingDirectory);
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(program, arguments);

    CommandResult result;
    result.started = process.waitForStarted();
    if (!result.started) {
        result.output = process.errorString().toUtf8();
        return result;
    }
    result.finished = process.waitForFinished(600000);
    if (!result.finished) {
        process.kill();
        process.waitForFinished();
    }
    result.exitStatus = process.exitStatus();
    result.exitCode   = process.exitCode();
    result.output     = process.readAll();
    return result;
}

void writeCollateral(const QTemporaryDir &directory, const QSocIomuxPlan &plan, const QString &top)
{
    writeTextFile(QDir(directory.path()).filePath("iomux0.v"), top);
    writeTextFile(
        QDir(directory.path()).filePath("iomux0_conn.v"),
        QSocIomuxGenerator::generateConnVerilog(plan));
    const QSocIomuxFormalCollateral collateral = QSocIomuxFormal::generate(plan);
    writeTextFile(QDir(directory.path()).filePath("iomux0_hs_formal.sv"), collateral.systemVerilog);
    writeTextFile(QDir(directory.path()).filePath("iomux0_hs_formal.sby"), collateral.sby);
}

class Test : public QObject
{
    Q_OBJECT

private slots:
    void collateralAssertsBundlesBroadcastAndInvalidCodes();
    void assertionCountCoversEverySlotOfEveryPin_data();
    void assertionCountCoversEverySlotOfEveryPin();
    void sourceOrderDoesNotChangeCollateral();
    void emptyPlanProducesNoCollateral();
    void generatedCollateralPassesSbyWhenAvailable();
    void tailPinSelectorDisconnectFailsBmcWhenAvailable();
    void padConstraintsPassSbyWhenAvailable();
    void padConstraintFalseClaimFailsSbyWhenAvailable();
};

void Test::assertionCountCoversEverySlotOfEveryPin_data()
{
    QTest::addColumn<quint32>("hsSlots");
    QTest::newRow("2") << 2u;
    QTest::newRow("3") << 3u;
    QTest::newRow("4-default") << 4u;
    QTest::newRow("5") << 5u;
    QTest::newRow("8") << 8u;
}

void Test::assertionCountCoversEverySlotOfEveryPin()
{
    QFETCH(quint32, hsSlots);
    QSocIomuxPlan plan;
    QStringList   errors;
    QVERIFY2(
        QSocIomuxGenerator::buildPlan(makeSlotCountDefinition(hsSlots), &plan, &errors),
        qPrintable(errors.join('\n')));
    const QSocIomuxFormalCollateral collateral = QSocIomuxFormal::generate(plan);

    QVERIFY(!collateral.systemVerilog.isEmpty());
    QVERIFY(!collateral.sby.isEmpty());

    /* Three bundle assertions per (pin, slot), one per RX sink, and one all-zero
     * block of three only when the selector field has codes above hs_slots. */
    const quint32 width      = hsSlots <= 2 ? 1u : (hsSlots <= 4 ? 2u : 3u);
    const bool    hasInvalid = hsSlots < (1u << width);
    const int     expected   = int(3 * plan.pinCount * hsSlots) + 2
                               + (hasInvalid ? int(3 * plan.pinCount) : 0);
    QCOMPARE(collateral.systemVerilog.count("assert ("), expected);
    QCOMPARE(collateral.systemVerilog.count("cover ("), int(2 * hsSlots));
}

void Test::collateralAssertsBundlesBroadcastAndInvalidCodes()
{
    QSocIomuxPlan plan;
    QStringList   errors;
    QVERIFY2(
        QSocIomuxGenerator::buildPlan(makeSmallDefinition(), &plan, &errors),
        qPrintable(errors.join('\n')));
    const QSocIomuxFormalCollateral collateral = QSocIomuxFormal::generate(plan);

    QVERIFY(collateral.systemVerilog.contains("module iomux0_hs_formal ("));
    QVERIFY(collateral.systemVerilog.contains("iomux0_conn u_conn ("));
    QVERIFY(collateral.systemVerilog.contains("iomux0_core u_core ("));
    QVERIFY(collateral.systemVerilog.contains(
        "assert (pad_output_enable_w[0] == (hs_p0_s0_output_enable_i));"));
    QVERIFY(collateral.systemVerilog.contains(
        "assert (pad_output_value_w[1] == (hs_p1_s0_output_value_i ^ 1'b1));"));
    QVERIFY(collateral.systemVerilog.contains("if (pin_0_select_i >= 2'd3) begin"));
    QVERIFY(collateral.systemVerilog.contains(
        "assert (hs_p1_s1_input_value_o == (pad_input_value_i[1]));"));
    QVERIFY(collateral.systemVerilog.contains("cover (pin_1_select_i == 2'd2);"));
    QVERIFY(collateral.sby.contains("prep -top iomux0_hs_formal"));
    QVERIFY(collateral.sby.contains("iomux0_conn.v"));
}

void Test::sourceOrderDoesNotChangeCollateral()
{
    QSocIomuxPlan plan;
    QVERIFY(QSocIomuxGenerator::buildPlan(makeSmallDefinition(), &plan, nullptr));
    const QSocIomuxFormalCollateral first = QSocIomuxFormal::generate(plan);

    /* Permute the source text rather than the parsed plan: re-sorting a plan with
     * the comparator the generator already applied cannot detect anything. */
    const QSocModuleDefinition reordered = makeDefinition(QString(R"(generator:
    kind: iomux
    bus: axi4_lite
    data_width: 32
    address_width: 8
    pin_count: 2
    hs_slots: 3
%1    route:
      - pin: 1
        slot: 1
        function: uart0
        signal: rx
        input_enable: 1
        input_value: {link: uart0_rx}
      - pin: 1
        slot: 0
        function: spi0
        signal: mode
        output_enable: 1
        output_value: {link: spi_mode, invert: true}
      - pin: 0
        slot: 0
        function: gpio0
        signal: data0
        output_enable: {link: gpio0_oe, bit: 0}
        output_value: {link: gpio0_out, bit: 0}
        input_enable: 1
        input_value: {link: gpio0_in, bit: 0}
)")
                                                              .arg(integrationBlock()));

    QSocIomuxPlan reorderedPlan;
    QStringList   errors;
    QVERIFY2(
        QSocIomuxGenerator::buildPlan(reordered, &reorderedPlan, &errors),
        qPrintable(errors.join('\n')));
    QCOMPARE(reorderedPlan, plan);
    const QSocIomuxFormalCollateral second = QSocIomuxFormal::generate(reorderedPlan);
    QCOMPARE(second.systemVerilog, first.systemVerilog);
    QCOMPARE(second.sby, first.sby);
}

void Test::emptyPlanProducesNoCollateral()
{
    const QSocIomuxFormalCollateral collateral = QSocIomuxFormal::generate(QSocIomuxPlan());
    QVERIFY(collateral.systemVerilog.isEmpty());
    QVERIFY(collateral.sby.isEmpty());
}

void Test::generatedCollateralPassesSbyWhenAvailable()
{
    const QString sby   = QStandardPaths::findExecutable(QStringLiteral("sby"));
    const QString yosys = QStandardPaths::findExecutable(QStringLiteral("yosys"));
    const QString z3    = QStandardPaths::findExecutable(QStringLiteral("z3"));
    if (sby.isEmpty() || yosys.isEmpty() || z3.isEmpty()) {
        QSOC_TEST_MISSING_DEPENDENCY(QStringLiteral("sby, yosys, and z3"));
    }

    QSocIomuxPlan plan;
    QVERIFY(QSocIomuxGenerator::buildPlan(makeSmallDefinition(), &plan, nullptr));

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    writeCollateral(directory, plan, QSocIomuxGenerator::generateTopVerilog(plan));

    for (const QString &task :
         {QStringLiteral("prove"), QStringLiteral("bmc"), QStringLiteral("cover")}) {
        const CommandResult result = runCommand(
            directory.path(),
            sby,
            {QStringLiteral("-f"), QStringLiteral("iomux0_hs_formal.sby"), task});
        QVERIFY2(result.started, result.output.constData());
        QVERIFY2(result.finished, result.output.constData());
        QCOMPARE(result.exitStatus, QProcess::NormalExit);
        QVERIFY2(result.exitCode == 0, result.output.constData());
    }
}

void Test::tailPinSelectorDisconnectFailsBmcWhenAvailable()
{
    const QString sby   = QStandardPaths::findExecutable(QStringLiteral("sby"));
    const QString yosys = QStandardPaths::findExecutable(QStringLiteral("yosys"));
    const QString z3    = QStandardPaths::findExecutable(QStringLiteral("z3"));
    if (sby.isEmpty() || yosys.isEmpty() || z3.isEmpty()) {
        QSOC_TEST_MISSING_DEPENDENCY(QStringLiteral("sby, yosys, and z3"));
    }

    QSocIomuxPlan plan;
    QVERIFY(QSocIomuxGenerator::buildPlan(makeTailDefinition(), &plan, nullptr));

    QString mutatedTop = QSocIomuxGenerator::generateTopVerilog(plan);
    QVERIFY(mutatedTop.contains("case (pin_8_select_i)"));
    mutatedTop.replace("case (pin_8_select_i)", "case ({2{1'b0}})");

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    writeCollateral(directory, plan, mutatedTop);

    const CommandResult result = runCommand(
        directory.path(),
        sby,
        {QStringLiteral("-f"), QStringLiteral("iomux0_hs_formal.sby"), QStringLiteral("bmc")});
    QVERIFY2(result.started, result.output.constData());
    QVERIFY2(result.finished, result.output.constData());
    QVERIFY2(result.exitCode != 0, result.output.constData());
    QVERIFY2(result.output.contains("FAIL"), result.output.constData());
}

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsociomuxformal.moc"

namespace {

QSocModuleDefinition makePadCellDefinition(const QString &claim)
{
    return makeDefinition(QString(R"yaml(generator:
    kind: iomux
    bus: axi4_lite
    data_width: 32
    address_width: 12
    pin_count: 2
    hs_slots: 2
    pad_cell:
      cell: gpio_pad_ps
      port:
        pad: PAD
        input_value: C
        input_enable: IE
        output_value: I
        output_enable: OE
      pull:
        port: [PE, PS]
        table:
          none: ["0", "x"]
          up: ["1", "1"]
          down: ["1", "0"]
      constraint:
        - name: pull_select_needs_enable
          expr: "%1"
        - name: ie_oe_exclusive
          expr: "!(IE && OE)"
        - name: pull_holds_across_oe_rise
          property: "!$rose(OE) || $stable(PE)"
    integration:
      instance: u_iomux0
      clock: clk_iomux
      reset: rst_iomux_n
      control: iomux_control
      pad:
        io: chip_gpio
    route:
      - pin: 0
        slot: 0
        function: uart0
        signal: tx
        output_value: {link: uart0_tx}
        output_enable: 1
        pull: up
      - pin: 1
        slot: 1
        function: gpio0
        signal: in1
        input_value: {link: gpio0_in1}
        input_enable: 1
        pull: keeper
)yaml")
                              .arg(claim));
}

QSocIomuxPlan padPlan(const QString &claim)
{
    QSocIomuxPlan plan;
    QStringList   errors;
    if (!QSocIomuxGenerator::buildPlan(makePadCellDefinition(claim), &plan, &errors)) {
        qWarning() << errors;
        return {};
    }
    plan.integration.padCell.cellPorts
        = {{"PAD", "inout"},
           {"C", "out"},
           {"IE", "in"},
           {"I", "in"},
           {"OE", "in"},
           {"PE", "in"},
           {"PS", "in"},
           {"DS", "in"}};
    return plan;
}

QStringList padSbyTasks(const QTemporaryDir &directory, const QSocIomuxPlan &plan)
{
    const QDir dir(directory.path());
    writeTextFile(dir.filePath("iomux0_pad.v"), QSocIomuxGenerator::generatePadVerilog(plan));
    const QSocIomuxFormalCollateral collateral = QSocIomuxFormal::generatePad(plan);
    writeTextFile(dir.filePath("iomux0_pad_formal.sv"), collateral.systemVerilog);
    writeTextFile(dir.filePath("iomux0_pad_formal.sby"), collateral.sby);
    return {QStringLiteral("prove"), QStringLiteral("bmc")};
}

} // namespace

void Test::padConstraintsPassSbyWhenAvailable()
{
    const QString sby   = QStandardPaths::findExecutable(QStringLiteral("sby"));
    const QString yosys = QStandardPaths::findExecutable(QStringLiteral("yosys"));
    const QString z3    = QStandardPaths::findExecutable(QStringLiteral("z3"));
    if (sby.isEmpty() || yosys.isEmpty() || z3.isEmpty()) {
        QSOC_TEST_MISSING_DEPENDENCY(QStringLiteral("sby, yosys, and z3"));
    }
    const QSocIomuxPlan plan = padPlan(QStringLiteral("!PS || PE"));
    QVERIFY(plan.integration.padCell.declared());

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    for (const QString &task : padSbyTasks(directory, plan)) {
        const CommandResult result = runCommand(
            directory.path(),
            sby,
            {QStringLiteral("-f"), QStringLiteral("iomux0_pad_formal.sby"), task});
        QVERIFY2(result.started, result.output.constData());
        QVERIFY2(result.finished, result.output.constData());
        QCOMPARE(result.exitStatus, QProcess::NormalExit);
        QVERIFY2(result.exitCode == 0, result.output.constData());
    }
}

void Test::padConstraintFalseClaimFailsSbyWhenAvailable()
{
    const QString sby   = QStandardPaths::findExecutable(QStringLiteral("sby"));
    const QString yosys = QStandardPaths::findExecutable(QStringLiteral("yosys"));
    const QString z3    = QStandardPaths::findExecutable(QStringLiteral("z3"));
    if (sby.isEmpty() || yosys.isEmpty() || z3.isEmpty()) {
        QSOC_TEST_MISSING_DEPENDENCY(QStringLiteral("sby, yosys, and z3"));
    }
    /* The down row has PE high with PS low, so this claim is false and the
     * proof must say so by name. */
    const QSocIomuxPlan plan = padPlan(QStringLiteral("!PE || PS"));
    QVERIFY(plan.integration.padCell.declared());

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    padSbyTasks(directory, plan);
    const CommandResult result = runCommand(
        directory.path(),
        sby,
        {QStringLiteral("-f"), QStringLiteral("iomux0_pad_formal.sby"), QStringLiteral("bmc")});
    QVERIFY2(result.started, result.output.constData());
    QVERIFY2(result.finished, result.output.constData());
    QVERIFY2(result.exitCode != 0, result.output.constData());
    QVERIFY2(
        result.output.contains("Assert failed")
            && result.output.contains("pull_select_needs_enable"),
        result.output.constData());
}

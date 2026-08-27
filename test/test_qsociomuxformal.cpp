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
    void sourceOrderDoesNotChangeCollateral();
    void emptyPlanProducesNoCollateral();
    void generatedCollateralPassesSbyWhenAvailable();
    void tailPinSelectorDisconnectFailsBmcWhenAvailable();
};

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

    QSocIomuxPlan reversedPlan = plan;
    std::reverse(reversedPlan.routes.begin(), reversedPlan.routes.end());
    std::sort(
        reversedPlan.routes.begin(),
        reversedPlan.routes.end(),
        [](const QSocIomuxRoutePlan &left, const QSocIomuxRoutePlan &right) {
            return left.pin != right.pin ? left.pin < right.pin : left.slot < right.slot;
        });
    const QSocIomuxFormalCollateral second = QSocIomuxFormal::generate(reversedPlan);
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

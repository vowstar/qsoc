// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "cli/qsoccliworker.h"
#include "common/qsocconsole.h"
#include "common/qsocprojectmanager.h"
#include "qsoc_test.h"

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTextStream>
#include <QtTest>

namespace {

const QString validModule = R"(iomux0:
  generator:
    kind: iomux
    bus: axi4_lite
    data_width: 32
    address_width: 14
    pin_count: 2
    integration:
      instance: u_iomux0
      clock: clk_iomux
      reset: rst_iomux_n
      control: iomux_control
      pad:
        input_value: pad_input_value
        input_enable: pad_input_enable
        output_value: pad_output_value
        output_enable: pad_output_enable
    route:
      - pin: 0
        slot: 0
        function: gpio0
        signal: data0
        input_value: {link: gpio0_in, bit: 0}
        input_enable: 1
        output_value: {link: gpio0_out, bit: 0}
        output_enable: {link: gpio0_oe, bit: 0}
)";

const QString invalidModule = R"(iomux0:
  generator:
    kind: iomux
    bus: axi4_lite
    pin_count: 2
    integration:
      instance: u_iomux0
      clock: clk_iomux
      reset: rst_iomux_n
      control: iomux_control
      pad:
        input_value: pad_input_value
        input_enable: pad_input_enable
        output_value: pad_output_value
        output_enable: pad_output_enable
    route:
      - pin: 5
        slot: 0
        function: gpio0
        signal: data0
        output_enable: 1
)";

const QString collidingModule = R"(iomux0_regs:
  port:
    status_i:
      direction: input
      type: logic
)";

struct CommandResult
{
    int     exitCode = -1;
    QString output;
};

class Test : public QObject
{
    Q_OBJECT

private:
    static QStringList      messages;
    static QtMessageHandler previousMessageHandler;

    static void messageOutput(QtMsgType type, const QMessageLogContext &context, const QString &text)
    {
        Q_UNUSED(type);
        Q_UNUSED(context);
        messages.append(text);
    }

    static void writeTextFile(const QString &path, const QString &text)
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        QTextStream(&file) << text;
    }

    static QString readTextFile(const QString &path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return QString();
        }
        return QString::fromUtf8(file.readAll());
    }

    static void createProject(const QTemporaryDir &directory)
    {
        QVERIFY(directory.isValid());
        QSocProjectManager projectManager;
        projectManager.setCurrentPath(directory.path());
        QVERIFY(projectManager.create("iomux_project"));
    }

    static QStringList projectOptions(const QTemporaryDir &directory)
    {
        return {"-d", directory.path(), "-p", "iomux_project"};
    }

    static QStringList artifactPaths(const QString &outputDirectory)
    {
        return {
            QDir(outputDirectory).filePath("iomux0_regs.v"),
            QDir(outputDirectory).filePath("iomux0_conn.v"),
            QDir(outputDirectory).filePath("iomux0.v"),
            QDir(outputDirectory).filePath("iomux0.f"),
            QDir(outputDirectory).filePath("iomux0.iomux.rpt"),
        };
    }

    static CommandResult runCommand(const QStringList &arguments)
    {
        messages.clear();
        QSocCliWorker worker;
        QSignalSpy    exitSpy(&worker, &QSocCliWorker::exit);
        worker.setup(arguments, false);
        worker.run();

        CommandResult result;
        if (exitSpy.count() == 1) {
            result.exitCode = exitSpy.takeFirst().first().toInt();
        }
        result.output = messages.join('\n');
        return result;
    }

private slots:
    void initTestCase();
    void cleanupTestCase();
    void createWritesAnIncompleteIomuxDraft();
    void validateReportsSummaryWithDefaultMarker();
    void validateReportsExplicitSlotsWithoutMarker();
    void generateWritesArtifactsAndRequiresForce();
    void generateWithFormalAndUvmTargetsRegs();
    void formalBankSplitsTheRoutingProof();
    void invalidSourceProducesNoArtifacts();
    void generateRejectsDerivedModuleNameCollision();
};

QStringList      Test::messages;
QtMessageHandler Test::previousMessageHandler = nullptr;

void Test::initTestCase()
{
    previousMessageHandler = qInstallMessageHandler(messageOutput);
    QSocConsole::setTeeToMessageHandler(true);
}

void Test::cleanupTestCase()
{
    QSocConsole::setTeeToMessageHandler(false);
    qInstallMessageHandler(previousMessageHandler);
}

void Test::createWritesAnIncompleteIomuxDraft()
{
    QTemporaryDir directory;
    createProject(directory);

    QStringList arguments = {"qsoc", "module", "create", "--generator", "iomux", "-l", "peripheral"};
    arguments.append(projectOptions(directory));
    arguments.append("iomux0");

    const CommandResult created = runCommand(arguments);
    QCOMPARE(created.exitCode, 0);
    QVERIFY2(created.output.contains("Created IOMUX module draft"), qPrintable(created.output));

    const QString modulePath = QDir(directory.path()).filePath("module/peripheral.soc_mod");
    const QString original   = readTextFile(modulePath);
    QVERIFY(!original.isEmpty());
    const YAML::Node library = YAML::Load(original.toStdString());
    QCOMPARE(
        QString::fromStdString(library["iomux0"]["generator"]["kind"].as<std::string>()), "iomux");
    QCOMPARE(
        QString::fromStdString(library["iomux0"]["generator"]["bus"].as<std::string>()),
        "axi4_lite");
    QVERIFY(library["iomux0"]["generator"]["route"].IsSequence());
    QCOMPARE(library["iomux0"]["generator"]["route"].size(), std::size_t(0));
    QVERIFY(!library["iomux0"]["generator"]["hs_slots"]);

    QStringList validateArguments = {"qsoc", "module", "validate", "-l", "peripheral"};
    validateArguments.append(projectOptions(directory));
    validateArguments.append("iomux0");
    const CommandResult validated = runCommand(validateArguments);
    QCOMPARE(validated.exitCode, 1);
    QVERIFY2(
        validated.output.contains("IOMUX_REQUIRED generator.pin_count"),
        qPrintable(validated.output));
    QVERIFY2(
        validated.output.contains("IOMUX_REQUIRED generator.integration"),
        qPrintable(validated.output));
    QVERIFY2(!validated.output.contains("hs_slots"), qPrintable(validated.output));

    const CommandResult duplicate = runCommand(arguments);
    QCOMPARE(duplicate.exitCode, 1);
    QVERIFY2(duplicate.output.contains("already exists"), qPrintable(duplicate.output));
    QCOMPARE(readTextFile(modulePath), original);
}

void Test::validateReportsSummaryWithDefaultMarker()
{
    QTemporaryDir directory;
    createProject(directory);
    writeTextFile(QDir(directory.path()).filePath("module/peripheral.soc_mod"), validModule);

    QStringList arguments = {"qsoc", "module", "validate", "-l", "peripheral"};
    arguments.append(projectOptions(directory));
    arguments.append("iomux0");

    const CommandResult result = runCommand(arguments);
    QCOMPARE(result.exitCode, 0);
    QVERIFY2(result.output.contains("IOMUX source is valid"), qPrintable(result.output));
    QVERIFY2(
        result.output.contains(
            "Pins: 2, HS slots: 4 (default), routes: 1, selector registers: 1, "
            "registers total: 5"),
        qPrintable(result.output));
    QVERIFY2(result.output.contains("Reset selects slot 0, RX broadcasts"), qPrintable(result.output));
    QVERIFY2(result.output.contains("Integration pending merge"), qPrintable(result.output));
}

void Test::validateReportsExplicitSlotsWithoutMarker()
{
    QTemporaryDir directory;
    createProject(directory);
    QString explicitModule = validModule;
    explicitModule.replace("    pin_count: 2\n", "    pin_count: 2\n    hs_slots: 4\n");
    writeTextFile(QDir(directory.path()).filePath("module/peripheral.soc_mod"), explicitModule);

    QStringList arguments = {"qsoc", "module", "validate", "-l", "peripheral"};
    arguments.append(projectOptions(directory));
    arguments.append("iomux0");

    const CommandResult result = runCommand(arguments);
    QCOMPARE(result.exitCode, 0);
    QVERIFY2(
        result.output.contains("Pins: 2, HS slots: 4, routes: 1, selector registers: 1"),
        qPrintable(result.output));
    QVERIFY2(!result.output.contains("(default)"), qPrintable(result.output));
}

void Test::generateWritesArtifactsAndRequiresForce()
{
    QTemporaryDir directory;
    createProject(directory);
    writeTextFile(QDir(directory.path()).filePath("module/peripheral.soc_mod"), validModule);

    QStringList arguments = {"qsoc", "generate", "module", "-l", "peripheral"};
    arguments.append(projectOptions(directory));
    arguments.append("iomux0");

    const CommandResult generated = runCommand(arguments);
    QCOMPARE(generated.exitCode, 0);
    QVERIFY2(generated.output.contains("Generated IOMUX Verilog"), qPrintable(generated.output));

    const QString outputDirectory = QDir(directory.path()).filePath("output/peripheral/iomux0");
    for (const QString &path : artifactPaths(outputDirectory)) {
        QVERIFY2(QFile::exists(path), qPrintable(path));
    }
    const QString regs = readTextFile(QDir(outputDirectory).filePath("iomux0_regs.v"));
    QVERIFY(regs.contains("module iomux0_regs ("));
    const QString top = readTextFile(QDir(outputDirectory).filePath("iomux0.v"));
    QVERIFY(top.contains("module iomux0_core ("));
    QVERIFY(top.contains("module iomux0 ("));
    QVERIFY(top.contains("iomux0_regs u_regs ("));
    const QString fileList = readTextFile(QDir(outputDirectory).filePath("iomux0.f"));
    QCOMPARE(fileList, QString("iomux0_regs.v\niomux0_conn.v\niomux0.v\n"));
    const QString report = readTextFile(QDir(outputDirectory).filePath("iomux0.iomux.rpt"));
    QVERIFY(report.contains("IOMUX route report for iomux0"));

    const CommandResult refused = runCommand(arguments);
    QCOMPARE(refused.exitCode, 1);
    QVERIFY2(refused.output.contains("already exists"), qPrintable(refused.output));

    QStringList forcedArguments = arguments;
    forcedArguments.insert(3, "--force");
    const CommandResult forced = runCommand(forcedArguments);
    QCOMPARE(forced.exitCode, 0);
}

void Test::generateWithFormalAndUvmTargetsRegs()
{
    QTemporaryDir directory;
    createProject(directory);
    writeTextFile(QDir(directory.path()).filePath("module/peripheral.soc_mod"), validModule);

    QStringList arguments
        = {"qsoc", "generate", "module", "--with-formal", "--with-uvm", "-l", "peripheral"};
    arguments.append(projectOptions(directory));
    arguments.append("iomux0");

    const CommandResult generated = runCommand(arguments);
    QCOMPARE(generated.exitCode, 0);

    const QString outputDirectory = QDir(directory.path()).filePath("output/peripheral/iomux0");
    QStringList   paths           = artifactPaths(outputDirectory);
    paths.append(QDir(outputDirectory).filePath("iomux0_regs_formal.sv"));
    paths.append(QDir(outputDirectory).filePath("iomux0_regs_formal.sby"));
    paths.append(QDir(outputDirectory).filePath("iomux0_hs_formal.sv"));
    paths.append(QDir(outputDirectory).filePath("iomux0_hs_formal.sby"));
    paths.append(QDir(outputDirectory).filePath("iomux0_regs_uvm_if.sv"));
    paths.append(QDir(outputDirectory).filePath("iomux0_regs_uvm_pkg.sv"));
    paths.append(QDir(outputDirectory).filePath("iomux0_regs_uvm_tb.sv"));
    paths.append(QDir(outputDirectory).filePath("iomux0_regs_uvm.f"));
    for (const QString &path : paths) {
        QVERIFY2(QFile::exists(path), qPrintable(path));
    }
    const QString sby = readTextFile(QDir(outputDirectory).filePath("iomux0_regs_formal.sby"));
    QVERIFY2(sby.contains("iomux0_regs.v"), qPrintable(sby));
    const QString hsSby = readTextFile(QDir(outputDirectory).filePath("iomux0_hs_formal.sby"));
    QVERIFY2(hsSby.contains("prep -top iomux0_hs_formal"), qPrintable(hsSby));
    QVERIFY2(
        generated.output.contains("covers iomux0_regs only, not routing"),
        qPrintable(generated.output));
}

void Test::formalBankSplitsTheRoutingProof()
{
    QTemporaryDir directory;
    createProject(directory);
    writeTextFile(QDir(directory.path()).filePath("module/peripheral.soc_mod"), validModule);

    QStringList arguments
        = {"qsoc", "generate", "module", "--with-formal", "--formal-bank", "1", "-l", "peripheral"};
    arguments.append(projectOptions(directory));
    arguments.append("iomux0");

    const CommandResult generated = runCommand(arguments);
    QCOMPARE(generated.exitCode, 0);
    QVERIFY2(
        generated.output.contains("HS routing proof: 2 banks, bank size 1"),
        qPrintable(generated.output));
    const QString outputDirectory = QDir(directory.path()).filePath("output/peripheral/iomux0");
    const QString hsSby = readTextFile(QDir(outputDirectory).filePath("iomux0_hs_formal.sby"));
    QVERIFY2(hsSby.contains("bmc_b1 bmc b1\n"), qPrintable(hsSby));
    QVERIFY2(
        hsSby.contains("b1: chparam -set PIN_LO 1 -set PIN_HI 1 iomux0_hs_formal\n"),
        qPrintable(hsSby));

    QStringList zero = arguments;
    zero.insert(3, "--force");
    zero[zero.indexOf("1")]     = "0";
    const CommandResult refused = runCommand(zero);
    QCOMPARE(refused.exitCode, 1);
    QVERIFY2(
        refused.output.contains("--formal-bank takes a pin count of 1 or more"),
        qPrintable(refused.output));
}

void Test::invalidSourceProducesNoArtifacts()
{
    QTemporaryDir directory;
    createProject(directory);
    writeTextFile(QDir(directory.path()).filePath("module/peripheral.soc_mod"), invalidModule);

    QStringList arguments = {"qsoc", "generate", "module", "-l", "peripheral"};
    arguments.append(projectOptions(directory));
    arguments.append("iomux0");

    const CommandResult generated = runCommand(arguments);
    QCOMPARE(generated.exitCode, 1);
    QVERIFY2(
        generated.output.contains("IOMUX_RANGE generator.route[0].pin"),
        qPrintable(generated.output));

    const QString outputDirectory = QDir(directory.path()).filePath("output/peripheral/iomux0");
    for (const QString &path : artifactPaths(outputDirectory)) {
        QVERIFY2(!QFile::exists(path), qPrintable(path));
    }
}

void Test::generateRejectsDerivedModuleNameCollision()
{
    QTemporaryDir directory;
    createProject(directory);
    writeTextFile(
        QDir(directory.path()).filePath("module/peripheral.soc_mod"), validModule + collidingModule);

    QStringList arguments = {"qsoc", "generate", "module", "-l", "peripheral"};
    arguments.append(projectOptions(directory));
    arguments.append("iomux0");

    const CommandResult generated = runCommand(arguments);
    QCOMPARE(generated.exitCode, 1);
    QVERIFY2(
        generated.output.contains("generated module name collides with peripheral/iomux0_regs"),
        qPrintable(generated.output));

    const QString outputDirectory = QDir(directory.path()).filePath("output/peripheral/iomux0");
    for (const QString &path : artifactPaths(outputDirectory)) {
        QVERIFY2(!QFile::exists(path), qPrintable(path));
    }
}

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsoccliparseiomux.moc"

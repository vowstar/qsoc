// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocmmiogenerator.h"
#include "common/qsocmodulemanager.h"
#include "qsoc_test.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
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

QSocModuleDefinition makeDefinition(const QString &moduleName, const QString &body)
{
    QSocModuleManager manager;
    return manager.moduleYamlToDefinition(
        QStringLiteral("peripheral"), moduleName, YAML::Load(body.toStdString()));
}

QSocModuleDefinition makeUvm32Definition()
{
    return makeDefinition(QStringLiteral("timer_ctrl"), R"(
generator:
  kind: mmio
  bus: axi4_lite
  data_width: 32
  address_width: 8
  register:
    control:
      offset: 0x00
      field:
        enable:
          lsb: 0
          access: rw
          reset: 1
          output: enable_o
        mode:
          lsb: 8
          width: 2
          access: rw
          reset: 2
          output: mode_o
    status:
      offset: 0x04
      field:
        busy:
          lsb: 0
          access: ro
          input: busy_i
        tag:
          lsb: 8
          width: 8
          access: ro
          value: 0xa5
    scratch:
      offset: 0x08
      field:
        data:
          lsb: 0
          width: 32
          access: rw
          reset: 0x12345678
          output: scratch_o
)");
}

QSocModuleDefinition makeUvm32ReverseDefinition()
{
    return makeDefinition(QStringLiteral("timer_ctrl"), R"(
generator:
  register:
    scratch:
      field:
        data:
          output: scratch_o
          reset: 0x12345678
          access: rw
          width: 32
          lsb: 0
      offset: 0x08
    status:
      field:
        tag:
          value: 0xa5
          access: ro
          width: 8
          lsb: 8
        busy:
          input: busy_i
          access: ro
          lsb: 0
      offset: 0x04
    control:
      field:
        mode:
          output: mode_o
          reset: 2
          access: rw
          width: 2
          lsb: 8
        enable:
          output: enable_o
          reset: 1
          access: rw
          lsb: 0
      offset: 0x00
  address_width: 8
  data_width: 32
  bus: axi4_lite
  kind: mmio
)");
}

QSocModuleDefinition makeUvm64Definition()
{
    return makeDefinition(QStringLiteral("wide_ctrl"), R"(
generator:
  kind: mmio
  bus: axi4_lite
  data_width: 64
  address_width: 16
  register:
    payload:
      offset: 0x00
      field:
        data:
          lsb: 0
          width: 64
          access: rw
          reset: 0x0123456789abcdef
          output: payload_o
    status:
      offset: 0x08
      field:
        ready:
          lsb: 0
          access: ro
          input: ready_i
        tag:
          lsb: 40
          width: 8
          access: ro
          value: 0x5a
)");
}

QSocModuleDefinition makeFirstRoSingleBitDefinition()
{
    return makeDefinition(QStringLiteral("first_ro_ctrl"), R"(
generator:
  kind: mmio
  bus: axi4_lite
  data_width: 32
  address_width: 8
  register:
    status:
      offset: 0x00
      field:
        ready:
          lsb: 0
          access: ro
          value: 1
    control:
      offset: 0x04
      field:
        enable:
          lsb: 0
          access: rw
          reset: 0
          output: enable_o
)");
}

QSocModuleDefinition makeAllRoDefinition()
{
    return makeDefinition(QStringLiteral("read_only_ctrl"), R"(
generator:
  kind: mmio
  bus: axi4_lite
  data_width: 32
  address_width: 8
  register:
    identity:
      offset: 0x00
      field:
        code:
          lsb: 0
          width: 8
          access: ro
          value: 0xa5
    status:
      offset: 0x04
      field:
        ready:
          lsb: 0
          access: ro
          input: ready_i
)");
}

enum UvmFixture {
    Mixed32Fixture,
    Wide64Fixture,
    FirstRoSingleBitFixture,
    AllRoFixture,
};

QSocModuleDefinition makeFixtureDefinition(int fixture)
{
    switch (fixture) {
    case Mixed32Fixture:
        return makeUvm32Definition();
    case Wide64Fixture:
        return makeUvm64Definition();
    case FirstRoSingleBitFixture:
        return makeFirstRoSingleBitDefinition();
    case AllRoFixture:
        return makeAllRoDefinition();
    }
    return {};
}

QSocModuleDefinition makeInvalidDefinition()
{
    return makeDefinition(QStringLiteral("invalid_ctrl"), R"(
generator:
  kind: mmio
  bus: axi4_lite
  register:
    control:
      offset: 0
      field:
        enable:
          lsb: 0
          access: rw
)");
}

void writeTextFile(const QString &path, const QString &text)
{
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
    QTextStream(&file) << text;
}

CommandResult runCommand(
    const QString     &workingDirectory,
    const QString     &program,
    const QStringList &arguments,
    int                timeoutMilliseconds)
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
    result.finished = process.waitForFinished(timeoutMilliseconds);
    if (!result.finished) {
        process.kill();
        process.waitForFinished();
    }
    result.exitStatus = process.exitStatus();
    result.exitCode   = process.exitCode();
    result.output     = process.readAll();
    return result;
}

bool findUvmSources(QString *sourceDirectory, QString *packagePath)
{
    const QString home = qEnvironmentVariable("UVM_HOME");
    if (home.isEmpty()) {
        return false;
    }

    const QDir homeDirectory(home);
    for (const QString &candidate :
         {homeDirectory.filePath(QStringLiteral("src")), homeDirectory.absolutePath()}) {
        const QString package = QDir(candidate).filePath(QStringLiteral("uvm_pkg.sv"));
        const QString macros  = QDir(candidate).filePath(QStringLiteral("uvm_macros.svh"));
        if (QFileInfo::exists(package) && QFileInfo::exists(macros)) {
            *sourceDirectory = candidate;
            *packagePath     = package;
            return true;
        }
    }
    return false;
}

class Test : public QObject
{
    Q_OBJECT

private slots:
    void failureClearsCollateral();
    void sourceOrderDoesNotChangeCollateral();
    void collateralHasExpectedStructure_data();
    void collateralHasExpectedStructure();
    void generatedTestbenchPassesVerilator_data();
    void generatedTestbenchPassesVerilator();
};

void Test::failureClearsCollateral()
{
    QSocMmioUvmCollateral collateral{
        QStringLiteral("sentinel"),
        QStringLiteral("sentinel"),
        QStringLiteral("sentinel"),
        QStringLiteral("sentinel")};
    QStringList errors = {QStringLiteral("sentinel")};

    QVERIFY(
        !QSocMmioGenerator::generateUvmCollateral(makeInvalidDefinition(), &collateral, &errors));
    QVERIFY(collateral.interfaceSource.isEmpty());
    QVERIFY(collateral.packageSource.isEmpty());
    QVERIFY(collateral.testbenchSource.isEmpty());
    QVERIFY(collateral.fileList.isEmpty());
    QVERIFY(!errors.isEmpty());
    QVERIFY(!errors.contains(QStringLiteral("sentinel")));
}

void Test::sourceOrderDoesNotChangeCollateral()
{
    QSocMmioUvmCollateral canonical;
    QSocMmioUvmCollateral reversed;
    QStringList           canonicalErrors;
    QStringList           reversedErrors;

    QVERIFY(
        QSocMmioGenerator::generateUvmCollateral(makeUvm32Definition(), &canonical, &canonicalErrors));
    QVERIFY(
        QSocMmioGenerator::generateUvmCollateral(
            makeUvm32ReverseDefinition(), &reversed, &reversedErrors));
    QVERIFY2(canonicalErrors.isEmpty(), qPrintable(canonicalErrors.join('\n')));
    QVERIFY2(reversedErrors.isEmpty(), qPrintable(reversedErrors.join('\n')));
    QCOMPARE(reversed.interfaceSource, canonical.interfaceSource);
    QCOMPARE(reversed.packageSource, canonical.packageSource);
    QCOMPARE(reversed.testbenchSource, canonical.testbenchSource);
    QCOMPARE(reversed.fileList, canonical.fileList);
}

void Test::collateralHasExpectedStructure_data()
{
    QTest::addColumn<bool>("wide");
    QTest::addColumn<QString>("moduleName");
    QTest::addColumn<quint32>("addressWidth");
    QTest::addColumn<quint32>("dataWidth");

    QTest::newRow("32-bit") << false << QStringLiteral("timer_ctrl") << quint32(8) << quint32(32);
    QTest::newRow("64-bit") << true << QStringLiteral("wide_ctrl") << quint32(16) << quint32(64);
}

void Test::collateralHasExpectedStructure()
{
    QFETCH(bool, wide);
    QFETCH(QString, moduleName);
    QFETCH(quint32, addressWidth);
    QFETCH(quint32, dataWidth);

    const QSocModuleDefinition definition = wide ? makeUvm64Definition() : makeUvm32Definition();
    QSocMmioUvmCollateral      collateral;
    QStringList                errors;
    QVERIFY(QSocMmioGenerator::generateUvmCollateral(definition, &collateral, &errors));
    QVERIFY2(errors.isEmpty(), qPrintable(errors.join('\n')));

    QVERIFY(
        collateral.interfaceSource.contains(QStringLiteral("interface %1_uvm_if").arg(moduleName)));
    QVERIFY(collateral.interfaceSource.contains(
        QRegularExpression(QStringLiteral("\\[%1:0\\]\\s+s_axi_awaddr").arg(addressWidth - 1))));
    QVERIFY(collateral.interfaceSource.contains(
        QRegularExpression(QStringLiteral("\\[%1:0\\]\\s+s_axi_wdata").arg(dataWidth - 1))));
    QVERIFY(collateral.interfaceSource.contains(
        QRegularExpression(QStringLiteral("\\[%1:0\\]\\s+s_axi_wstrb").arg(dataWidth / 8 - 1))));
    QVERIFY(collateral.packageSource.contains(QStringLiteral("package %1_uvm_pkg").arg(moduleName)));
    QVERIFY(collateral.packageSource.contains(QStringLiteral("extends uvm_test")));
    QVERIFY(collateral.packageSource.contains(QStringLiteral("QSOC_UVM_PASS")));
    const qsizetype writeCausalityIndex = collateral.packageSource.indexOf(QStringLiteral(
        "if (vif.s_axi_bvalid === 1'b1\n"
        "                        && (!aw_pending || !w_pending))"));
    const qsizetype writeAddressIndex   = collateral.packageSource.indexOf(
        QStringLiteral("aw_address = vif.s_axi_awaddr"));
    const qsizetype writeDataIndex = collateral.packageSource.indexOf(
        QStringLiteral("write_data = vif.s_axi_wdata"));
    const qsizetype readCausalityIndex = collateral.packageSource.indexOf(
        QStringLiteral("if (vif.s_axi_rvalid === 1'b1 && !ar_pending)"));
    const qsizetype readAddressIndex = collateral.packageSource.indexOf(
        QStringLiteral("ar_address = vif.s_axi_araddr"));
    QVERIFY(writeCausalityIndex >= 0);
    QVERIFY(writeAddressIndex > writeCausalityIndex);
    QVERIFY(writeDataIndex > writeCausalityIndex);
    QVERIFY(readCausalityIndex >= 0);
    QVERIFY(readAddressIndex > readCausalityIndex);
    QVERIFY(collateral.testbenchSource.contains(QStringLiteral("module %1_uvm_tb").arg(moduleName)));
    QVERIFY(collateral.testbenchSource.contains(QStringLiteral("run_test")));
    QVERIFY(collateral.testbenchSource.contains(QStringLiteral("QSOC_UVM_FAILED")));
    QVERIFY(!collateral.interfaceSource.contains(QStringLiteral("verilator"), Qt::CaseInsensitive));
    QVERIFY(!collateral.packageSource.contains(QStringLiteral("verilator"), Qt::CaseInsensitive));
    QVERIFY(!collateral.testbenchSource.contains(QStringLiteral("verilator"), Qt::CaseInsensitive));

    const QString expectedFileList = moduleName + QStringLiteral(".v\n") + moduleName
                                     + QStringLiteral("_uvm_if.sv\n") + moduleName
                                     + QStringLiteral("_uvm_pkg.sv\n") + moduleName
                                     + QStringLiteral("_uvm_tb.sv\n");
    QCOMPARE(collateral.fileList, expectedFileList);
}

void Test::generatedTestbenchPassesVerilator_data()
{
    QTest::addColumn<int>("fixture");
    QTest::addColumn<QString>("moduleName");

    QTest::newRow("32-bit-mixed-registers") << int(Mixed32Fixture) << QStringLiteral("timer_ctrl");
    QTest::newRow("64-bit-wide-registers") << int(Wide64Fixture) << QStringLiteral("wide_ctrl");
    QTest::newRow("first-ro-single-bit-rw")
        << int(FirstRoSingleBitFixture) << QStringLiteral("first_ro_ctrl");
    QTest::newRow("all-ro-registers") << int(AllRoFixture) << QStringLiteral("read_only_ctrl");
}

void Test::generatedTestbenchPassesVerilator()
{
    QFETCH(int, fixture);
    QFETCH(QString, moduleName);

    const QString verilator = QStandardPaths::findExecutable(QStringLiteral("verilator"));
    const QString make      = QStandardPaths::findExecutable(QStringLiteral("make"));
    QString       uvmSourceDirectory;
    QString       uvmPackagePath;
    if (verilator.isEmpty() || make.isEmpty()
        || !findUvmSources(&uvmSourceDirectory, &uvmPackagePath)) {
        QSOC_TEST_MISSING_DEPENDENCY(QStringLiteral("verilator, make, and UVM_HOME/src/uvm_pkg.sv"));
    }

    const QSocModuleDefinition definition = makeFixtureDefinition(fixture);
    QString                    verilog;
    QSocMmioUvmCollateral      collateral;
    QStringList                errors;
    QVERIFY(QSocMmioGenerator::generateVerilog(definition, &verilog, &errors));
    QVERIFY2(errors.isEmpty(), qPrintable(errors.join('\n')));
    QVERIFY(QSocMmioGenerator::generateUvmCollateral(definition, &collateral, &errors));
    QVERIFY2(errors.isEmpty(), qPrintable(errors.join('\n')));

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QDir    outputDirectory(directory.path());
    const QString verilogPath = outputDirectory.filePath(moduleName + QStringLiteral(".v"));
    writeTextFile(verilogPath, verilog);
    writeTextFile(
        outputDirectory.filePath(moduleName + QStringLiteral("_uvm_if.sv")),
        collateral.interfaceSource);
    writeTextFile(
        outputDirectory.filePath(moduleName + QStringLiteral("_uvm_pkg.sv")),
        collateral.packageSource);
    writeTextFile(
        outputDirectory.filePath(moduleName + QStringLiteral("_uvm_tb.sv")),
        collateral.testbenchSource);
    const QString fileListName = moduleName + QStringLiteral("_uvm.f");
    writeTextFile(outputDirectory.filePath(fileListName), collateral.fileList);

    const QString     topName         = moduleName + QStringLiteral("_uvm_tb");
    const QString     objectDirectory = outputDirectory.filePath(QStringLiteral("obj_dir"));
    const QStringList arguments{
        QStringLiteral("--binary"),
        QStringLiteral("--timing"),
        QStringLiteral("--top-module"),
        topName,
        QStringLiteral("--Mdir"),
        objectDirectory,
        QStringLiteral("-j"),
        QStringLiteral("16"),
        QStringLiteral("--threads"),
        QStringLiteral("1"),
        QStringLiteral("-Wno-fatal"),
        QStringLiteral("-Wno-TIMESCALEMOD"),
        QStringLiteral("-Wno-WIDTHTRUNC"),
        QStringLiteral("-Wno-WIDTHEXPAND"),
        QStringLiteral("+define+UVM_NO_DPI"),
        QStringLiteral("-I") + uvmSourceDirectory,
        uvmPackagePath,
        QStringLiteral("-f"),
        fileListName};
    const CommandResult compileResult = runCommand(directory.path(), verilator, arguments, 300000);
    QVERIFY2(compileResult.started, compileResult.output.constData());
    QVERIFY2(compileResult.finished, compileResult.output.constData());
    QCOMPARE(compileResult.exitStatus, QProcess::NormalExit);
    QVERIFY2(compileResult.exitCode == 0, compileResult.output.constData());

    QString binaryName = QStringLiteral("V") + topName;
#ifdef Q_OS_WIN
    binaryName += QStringLiteral(".exe");
#endif
    const QString binary = QDir(objectDirectory).filePath(binaryName);
    QVERIFY2(QFileInfo::exists(binary), qPrintable(binary));
    const CommandResult runResult = runCommand(directory.path(), binary, {}, 60000);
    QVERIFY2(runResult.started, runResult.output.constData());
    QVERIFY2(runResult.finished, runResult.output.constData());
    QCOMPARE(runResult.exitStatus, QProcess::NormalExit);
    QVERIFY2(runResult.exitCode == 0, runResult.output.constData());
    QVERIFY2(runResult.output.contains("QSOC_UVM_PASS"), runResult.output.constData());
    const QString runOutput = QString::fromUtf8(runResult.output);
    QVERIFY2(
        runOutput.contains(QRegularExpression(QStringLiteral("UVM_ERROR\\s*:\\s*0\\b"))),
        runResult.output.constData());
    QVERIFY2(
        runOutput.contains(QRegularExpression(QStringLiteral("UVM_FATAL\\s*:\\s*0\\b"))),
        runResult.output.constData());

    const CommandResult failureResult = runCommand(
        directory.path(), binary, {QStringLiteral("+UVM_TESTNAME=qsoc_missing_test")}, 60000);
    QVERIFY2(failureResult.started, failureResult.output.constData());
    QVERIFY2(failureResult.finished, failureResult.output.constData());
    QVERIFY2(
        failureResult.exitStatus != QProcess::NormalExit || failureResult.exitCode != 0,
        failureResult.output.constData());
    QVERIFY2(failureResult.output.contains("QSOC_UVM_FAILED"), failureResult.output.constData());

    if (fixture == FirstRoSingleBitFixture) {
        QString       mutatedVerilog = verilog;
        const QString writeGuard     = QStringLiteral(
            "            if (address_is_mapped(write_address)) begin");
        QVERIFY(mutatedVerilog.contains(writeGuard));
        mutatedVerilog.replace(writeGuard, QStringLiteral("            if (1'b1) begin"));

        const QString   defaultWrite = QStringLiteral("                default: begin end");
        const qsizetype defaultIndex = mutatedVerilog.lastIndexOf(defaultWrite);
        QVERIFY(defaultIndex >= 0);
        mutatedVerilog.replace(
            defaultIndex,
            defaultWrite.size(),
            QStringLiteral("                default: mmio_field_0_q <= 1'b1;"));
        mutatedVerilog.replace(QStringLiteral("s_axi_bvalid"), QStringLiteral("s_axi_bvalid_q"));
        mutatedVerilog.replace(QStringLiteral("s_axi_rvalid"), QStringLiteral("s_axi_rvalid_q"));
        const QString bValidPort = QStringLiteral("output reg         s_axi_bvalid_q");
        const QString rValidPort = QStringLiteral("output reg         s_axi_rvalid_q");
        QVERIFY(mutatedVerilog.contains(bValidPort));
        QVERIFY(mutatedVerilog.contains(rValidPort));
        mutatedVerilog.replace(bValidPort, QStringLiteral("output wire        s_axi_bvalid"));
        mutatedVerilog.replace(rValidPort, QStringLiteral("output wire        s_axi_rvalid"));
        const QString   moduleHeaderEnd = QStringLiteral(");\n\n");
        const qsizetype headerEndIndex  = mutatedVerilog.indexOf(moduleHeaderEnd);
        QVERIFY(headerEndIndex >= 0);
        mutatedVerilog.insert(
            headerEndIndex + moduleHeaderEnd.size(),
            QStringLiteral(
                "reg s_axi_bvalid_q;\n"
                "reg s_axi_rvalid_q;\n"
                "reg inject_premature_write;\n"
                "reg inject_premature_read;\n"
                "\n"
                "initial begin\n"
                "    inject_premature_write = $test$plusargs(\"QSOC_PREMATURE_WRITE\");\n"
                "    inject_premature_read  = $test$plusargs(\"QSOC_PREMATURE_READ\");\n"
                "end\n"
                "\n"
                "assign s_axi_bvalid = inject_premature_write\n"
                "                      ? rst_ni && s_axi_awvalid && s_axi_wvalid\n"
                "                      : s_axi_bvalid_q;\n"
                "assign s_axi_rvalid = inject_premature_read\n"
                "                      ? rst_ni && s_axi_arvalid\n"
                "                      : s_axi_rvalid_q;\n"
                "\n"));
        writeTextFile(verilogPath, mutatedVerilog);

        const QString mutatedObjectDirectory = outputDirectory.filePath(
            QStringLiteral("obj_dir_mutated"));
        QStringList     mutatedArguments = arguments;
        const qsizetype mdirIndex        = mutatedArguments.indexOf(QStringLiteral("--Mdir"));
        QVERIFY(mdirIndex >= 0 && mdirIndex + 1 < mutatedArguments.size());
        mutatedArguments[mdirIndex + 1] = mutatedObjectDirectory;
        const CommandResult mutatedCompile
            = runCommand(directory.path(), verilator, mutatedArguments, 300000);
        QVERIFY2(mutatedCompile.started, mutatedCompile.output.constData());
        QVERIFY2(mutatedCompile.finished, mutatedCompile.output.constData());
        QCOMPARE(mutatedCompile.exitStatus, QProcess::NormalExit);
        QVERIFY2(mutatedCompile.exitCode == 0, mutatedCompile.output.constData());

        const QString mutatedBinary = QDir(mutatedObjectDirectory).filePath(binaryName);
        QVERIFY2(QFileInfo::exists(mutatedBinary), qPrintable(mutatedBinary));
        const CommandResult mutatedRun = runCommand(directory.path(), mutatedBinary, {}, 60000);
        QVERIFY2(mutatedRun.started, mutatedRun.output.constData());
        QVERIFY2(mutatedRun.finished, mutatedRun.output.constData());
        QVERIFY2(
            mutatedRun.exitStatus != QProcess::NormalExit || mutatedRun.exitCode != 0,
            mutatedRun.output.constData());
        QVERIFY2(mutatedRun.output.contains("READ_DATA"), mutatedRun.output.constData());
        QVERIFY2(mutatedRun.output.contains("QSOC_UVM_FAILED"), mutatedRun.output.constData());

        const QList<QPair<QString, QByteArray>> protocolFaults{
            {QStringLiteral("+QSOC_PREMATURE_WRITE"), QByteArray("B_ORDER")},
            {QStringLiteral("+QSOC_PREMATURE_READ"), QByteArray("R_ORDER")},
        };
        for (const auto &[plusArgument, expectedFailure] : protocolFaults) {
            const CommandResult protocolRun
                = runCommand(directory.path(), mutatedBinary, {plusArgument}, 60000);
            QVERIFY2(protocolRun.started, protocolRun.output.constData());
            QVERIFY2(protocolRun.finished, protocolRun.output.constData());
            QVERIFY2(
                protocolRun.exitStatus != QProcess::NormalExit || protocolRun.exitCode != 0,
                protocolRun.output.constData());
            QVERIFY2(protocolRun.output.contains(expectedFailure), protocolRun.output.constData());
            QVERIFY2(protocolRun.output.contains("QSOC_UVM_FAILED"), protocolRun.output.constData());
        }
    }
}

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocmmiouvm.moc"

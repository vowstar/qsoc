// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocmmiogenerator.h"
#include "common/qsocmodulemanager.h"
#include "qsoc_test.h"

#include <QDir>
#include <QFile>
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

QSocModuleDefinition makeFormal32Definition()
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
          output: ref_field_0_q
        mode:
          lsb: 4
          width: 3
          access: rw
          reset: 3
          output: formal_past_valid
        tag:
          lsb: 16
          width: 8
          access: ro
          value: 0xa5
    status:
      offset: 0x04
      field:
        busy:
          lsb: 0
          access: ro
          input: ref_aw_pending_q
        code:
          lsb: 8
          width: 8
          access: ro
          input: ref_s_axi_rdata
        address_sample:
          lsb: 20
          access: ro
          input: address
)");
}

QSocModuleDefinition makeFormal32ReverseDefinition()
{
    return makeDefinition(QStringLiteral("timer_ctrl"), R"(
generator:
  register:
    status:
      field:
        address_sample:
          input: address
          access: ro
          lsb: 20
        code:
          input: ref_s_axi_rdata
          access: ro
          width: 8
          lsb: 8
        busy:
          input: ref_aw_pending_q
          access: ro
          lsb: 0
      offset: 0x04
    control:
      field:
        tag:
          value: 0xa5
          access: ro
          width: 8
          lsb: 16
        mode:
          output: formal_past_valid
          reset: 3
          access: rw
          width: 3
          lsb: 4
        enable:
          output: ref_field_0_q
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

QSocModuleDefinition makeFormal64Definition()
{
    return makeDefinition(QStringLiteral("wide_ctrl"), R"(
generator:
  kind: mmio
  bus: axi4_lite
  data_width: 64
  address_width: 64
  register:
    boundary:
      offset: 0xfffffffffffffff8
      field:
        low:
          lsb: 0
          width: 9
          access: rw
          reset: 0x101
          output: low_o
        middle:
          lsb: 16
          width: 32
          access: rw
          reset: 0x89abcdef
          output: middle_o
        high:
          lsb: 55
          width: 9
          access: rw
          reset: 0x155
          output: high_o
)");
}

QSocModuleDefinition makeKnownAnswer32Definition()
{
    return makeDefinition(QStringLiteral("known_ctrl"), R"(
generator:
  kind: mmio
  bus: axi4_lite
  data_width: 32
  address_width: 32
  register:
    control:
      offset: 0x00000000
      field:
        low:
          lsb: 0
          width: 8
          access: rw
          reset: 0x5a
          output: low_o
        high:
          lsb: 16
          width: 8
          access: rw
          reset: 0xa5
          output: high_o
    identification:
      offset: 0x00000004
      field:
        value:
          lsb: 0
          width: 8
          access: ro
          value: 0x3c
)");
}

enum FormalFixture {
    Mixed32Fixture,
    Boundary64Fixture,
    Known32Fixture,
};

QSocModuleDefinition makeFormalFixture(int fixture)
{
    switch (fixture) {
    case Mixed32Fixture:
        return makeFormal32Definition();
    case Boundary64Fixture:
        return makeFormal64Definition();
    case Known32Fixture:
        return makeKnownAnswer32Definition();
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
    result.finished = process.waitForFinished(120000);
    if (!result.finished) {
        process.kill();
        process.waitForFinished();
    }
    result.exitStatus = process.exitStatus();
    result.exitCode   = process.exitCode();
    result.output     = process.readAll();
    return result;
}

class Test : public QObject
{
    Q_OBJECT

private slots:
    void failureClearsCollateral();
    void sourceOrderDoesNotChangeCollateral();
    void collateralNamesWidthsAndSidebands_data();
    void collateralNamesWidthsAndSidebands();
    void knownAnswerUsesIndependentConstants();
    void generatedCollateralPassesSby_data();
    void generatedCollateralPassesSby();
    void generatedMutantsFailBmc_data();
    void generatedMutantsFailBmc();
};

void Test::failureClearsCollateral()
{
    QSocMmioFormalCollateral collateral{QStringLiteral("sentinel"), QStringLiteral("sentinel")};
    QStringList              errors = {QStringLiteral("sentinel")};

    QVERIFY(
        !QSocMmioGenerator::generateFormalCollateral(makeInvalidDefinition(), &collateral, &errors));
    QVERIFY(collateral.systemVerilog.isEmpty());
    QVERIFY(collateral.sby.isEmpty());
    QVERIFY(!errors.isEmpty());
    QVERIFY(!errors.contains(QStringLiteral("sentinel")));
}

void Test::sourceOrderDoesNotChangeCollateral()
{
    QSocMmioFormalCollateral canonical;
    QSocMmioFormalCollateral reversed;
    QStringList              canonicalErrors;
    QStringList              reversedErrors;

    QVERIFY(
        QSocMmioGenerator::generateFormalCollateral(
            makeFormal32Definition(), &canonical, &canonicalErrors));
    QVERIFY(
        QSocMmioGenerator::generateFormalCollateral(
            makeFormal32ReverseDefinition(), &reversed, &reversedErrors));
    QVERIFY2(canonicalErrors.isEmpty(), qPrintable(canonicalErrors.join('\n')));
    QVERIFY2(reversedErrors.isEmpty(), qPrintable(reversedErrors.join('\n')));
    QCOMPARE(reversed.systemVerilog, canonical.systemVerilog);
    QCOMPARE(reversed.sby, canonical.sby);
}

void Test::collateralNamesWidthsAndSidebands_data()
{
    QTest::addColumn<bool>("wide");
    QTest::addColumn<QString>("moduleName");
    QTest::addColumn<quint32>("addressWidth");
    QTest::addColumn<quint32>("dataWidth");

    QTest::newRow("32-bit") << false << QStringLiteral("timer_ctrl") << quint32(8) << quint32(32);
    QTest::newRow("64-bit") << true << QStringLiteral("wide_ctrl") << quint32(64) << quint32(64);
}

void Test::collateralNamesWidthsAndSidebands()
{
    QFETCH(bool, wide);
    QFETCH(QString, moduleName);
    QFETCH(quint32, addressWidth);
    QFETCH(quint32, dataWidth);

    QSocMmioFormalCollateral   collateral;
    QStringList                errors;
    const QSocModuleDefinition definition = wide ? makeFormal64Definition()
                                                 : makeFormal32Definition();
    QVERIFY(QSocMmioGenerator::generateFormalCollateral(definition, &collateral, &errors));
    QVERIFY2(errors.isEmpty(), qPrintable(errors.join('\n')));

    const QString topName = moduleName + QStringLiteral("_formal");
    QVERIFY(collateral.systemVerilog.contains(QStringLiteral("module %1").arg(topName)));
    QVERIFY(collateral.systemVerilog.contains(
        QRegularExpression(QStringLiteral("\\[%1:0\\]\\s+s_axi_awaddr").arg(addressWidth - 1))));
    QVERIFY(collateral.systemVerilog.contains(
        QRegularExpression(QStringLiteral("\\[%1:0\\]\\s+s_axi_wdata").arg(dataWidth - 1))));
    QVERIFY(collateral.systemVerilog.contains(
        QRegularExpression(QStringLiteral("\\[%1:0\\]\\s+s_axi_wstrb").arg(dataWidth / 8 - 1))));
    QVERIFY(collateral.sby.contains(QStringLiteral("prep -top %1").arg(topName)));
    QVERIFY(collateral.sby.contains(moduleName + QStringLiteral(".v")));
    QVERIFY(collateral.sby.contains(moduleName + QStringLiteral("_formal.sv")));
    QVERIFY(collateral.sby.contains(QStringLiteral("bmc: mode bmc")));
    QVERIFY(collateral.sby.contains(QStringLiteral("prove: abc pdr")));
    QVERIFY(collateral.sby.contains(QStringLiteral("cover: smtbmc z3")));

    if (!wide) {
        for (const QString &sideband :
             {QStringLiteral("ref_field_0_q"),
              QStringLiteral("formal_past_valid"),
              QStringLiteral("ref_aw_pending_q"),
              QStringLiteral("ref_s_axi_rdata"),
              QStringLiteral("address")}) {
            QVERIFY2(
                collateral.systemVerilog.contains(
                    QRegularExpression(QStringLiteral("\\.%1\\s*\\(\\s*%1\\s*\\)").arg(sideband))),
                qPrintable(sideband));
        }
    }
}

void Test::knownAnswerUsesIndependentConstants()
{
    QString                    verilog;
    QSocMmioFormalCollateral   collateral;
    QStringList                errors;
    const QSocModuleDefinition definition = makeKnownAnswer32Definition();
    QVERIFY(QSocMmioGenerator::generateVerilog(definition, &verilog, &errors));
    QVERIFY2(errors.isEmpty(), qPrintable(errors.join('\n')));
    QVERIFY(QSocMmioGenerator::generateFormalCollateral(definition, &collateral, &errors));
    QVERIFY2(errors.isEmpty(), qPrintable(errors.join('\n')));

    for (const QString &expected : {
             QStringLiteral(
                 "        case (address)\n"
                 "            32'h00000000: address_is_mapped = 1'b1;\n"
                 "            32'h00000004: address_is_mapped = 1'b1;\n"
                 "            default: address_is_mapped = 1'b0;"),
             QStringLiteral(
                 "            32'h00000000: begin\n"
                 "                read_register[7:0] = mmio_field_0_q;\n"
                 "                read_register[23:16] = mmio_field_1_q;\n"
                 "            end\n"
                 "            32'h00000004: begin\n"
                 "                read_register[7:0] = 8'h3c;\n"
                 "            end"),
             QStringLiteral(
                 "        mmio_field_0_q <= 8'h5a;\n"
                 "        mmio_field_1_q <= 8'ha5;"),
         }) {
        QVERIFY2(verilog.contains(expected), qPrintable(expected));
    }
    for (const QString &expected : {
             QStringLiteral(
                 "        case (formal_mapped_address)\n"
                 "            32'h00000000: formal_address_is_mapped = 1'b1;\n"
                 "            32'h00000004: formal_address_is_mapped = 1'b1;\n"
                 "            default: formal_address_is_mapped = 1'b0;"),
             QStringLiteral(
                 "            32'h00000000: begin\n"
                 "                formal_read_register[7:0] = formal_field_0_q;\n"
                 "                formal_read_register[23:16] = formal_field_1_q;\n"
                 "            end\n"
                 "            32'h00000004: begin\n"
                 "                formal_read_register[7:0] = 8'h3c;\n"
                 "            end"),
             QStringLiteral(
                 "        formal_field_0_q <= 8'h5a;\n"
                 "        formal_field_1_q <= 8'ha5;"),
         }) {
        QVERIFY2(collateral.systemVerilog.contains(expected), qPrintable(expected));
    }
}

void Test::generatedCollateralPassesSby_data()
{
    QTest::addColumn<int>("fixture");
    QTest::addColumn<QString>("moduleName");

    QTest::newRow("32-bit-mixed-fields") << int(Mixed32Fixture) << QStringLiteral("timer_ctrl");
    QTest::newRow("64-bit-boundary-lanes") << int(Boundary64Fixture) << QStringLiteral("wide_ctrl");
    QTest::newRow("32-bit-known-answer") << int(Known32Fixture) << QStringLiteral("known_ctrl");
}

void Test::generatedCollateralPassesSby()
{
    QFETCH(int, fixture);
    QFETCH(QString, moduleName);

    const QString sby   = QStandardPaths::findExecutable(QStringLiteral("sby"));
    const QString yosys = QStandardPaths::findExecutable(QStringLiteral("yosys"));
    const QString z3    = QStandardPaths::findExecutable(QStringLiteral("z3"));
    if (sby.isEmpty() || yosys.isEmpty() || z3.isEmpty()) {
        QSOC_TEST_MISSING_DEPENDENCY(QStringLiteral("sby, yosys, and z3"));
    }

    const QSocModuleDefinition definition = makeFormalFixture(fixture);
    QString                    verilog;
    QSocMmioFormalCollateral   collateral;
    QStringList                errors;
    QVERIFY(QSocMmioGenerator::generateVerilog(definition, &verilog, &errors));
    QVERIFY2(errors.isEmpty(), qPrintable(errors.join('\n')));
    QVERIFY(QSocMmioGenerator::generateFormalCollateral(definition, &collateral, &errors));
    QVERIFY2(errors.isEmpty(), qPrintable(errors.join('\n')));

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    writeTextFile(QDir(directory.path()).filePath(moduleName + QStringLiteral(".v")), verilog);
    writeTextFile(
        QDir(directory.path()).filePath(moduleName + QStringLiteral("_formal.sv")),
        collateral.systemVerilog);
    const QString sbyFileName = moduleName + QStringLiteral("_formal.sby");
    writeTextFile(QDir(directory.path()).filePath(sbyFileName), collateral.sby);

    for (const QString &task :
         {QStringLiteral("prove"), QStringLiteral("bmc"), QStringLiteral("cover")}) {
        const CommandResult result
            = runCommand(directory.path(), sby, {QStringLiteral("-f"), sbyFileName, task});
        QVERIFY2(result.started, result.output.constData());
        QVERIFY2(result.finished, result.output.constData());
        QCOMPARE(result.exitStatus, QProcess::NormalExit);
        QVERIFY2(result.exitCode == 0, result.output.constData());
    }
}

void Test::generatedMutantsFailBmc_data()
{
    QTest::addColumn<QString>("needle");
    QTest::addColumn<QString>("replacement");

    QTest::newRow("wrong-reset") << QStringLiteral("        mmio_field_0_q <= 8'h5a;")
                                 << QStringLiteral("        mmio_field_0_q <= 8'h00;");
    QTest::newRow("ignore-write-strobe")
        << QStringLiteral("{8{write_strobe[0]}}") << QStringLiteral("8'hff");
    QTest::newRow("unmapped-alias")
        << QStringLiteral("            default: address_is_mapped = 1'b0;")
        << QStringLiteral("            default: address_is_mapped = 1'b1;");
    QTest::newRow("drop-write-response") << QStringLiteral("            s_axi_bvalid <= 1'b1;")
                                         << QStringLiteral("            s_axi_bvalid <= 1'b0;");
}

void Test::generatedMutantsFailBmc()
{
    QFETCH(QString, needle);
    QFETCH(QString, replacement);

    const QString sby   = QStandardPaths::findExecutable(QStringLiteral("sby"));
    const QString yosys = QStandardPaths::findExecutable(QStringLiteral("yosys"));
    const QString z3    = QStandardPaths::findExecutable(QStringLiteral("z3"));
    if (sby.isEmpty() || yosys.isEmpty() || z3.isEmpty()) {
        QSOC_TEST_MISSING_DEPENDENCY(QStringLiteral("sby, yosys, and z3"));
    }

    QString                    verilog;
    QSocMmioFormalCollateral   collateral;
    QStringList                errors;
    const QSocModuleDefinition definition = makeKnownAnswer32Definition();
    QVERIFY(QSocMmioGenerator::generateVerilog(definition, &verilog, &errors));
    QVERIFY2(errors.isEmpty(), qPrintable(errors.join('\n')));
    QVERIFY(QSocMmioGenerator::generateFormalCollateral(definition, &collateral, &errors));
    QVERIFY2(errors.isEmpty(), qPrintable(errors.join('\n')));

    QCOMPARE(verilog.count(needle), qsizetype(1));
    verilog.replace(needle, replacement);

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    writeTextFile(QDir(directory.path()).filePath(QStringLiteral("known_ctrl.v")), verilog);
    writeTextFile(
        QDir(directory.path()).filePath(QStringLiteral("known_ctrl_formal.sv")),
        collateral.systemVerilog);
    writeTextFile(
        QDir(directory.path()).filePath(QStringLiteral("known_ctrl_formal.sby")), collateral.sby);

    const CommandResult result = runCommand(
        directory.path(),
        sby,
        {QStringLiteral("-f"), QStringLiteral("known_ctrl_formal.sby"), QStringLiteral("bmc")});
    QVERIFY2(result.started, result.output.constData());
    QVERIFY2(result.finished, result.output.constData());
    QCOMPARE(result.exitStatus, QProcess::NormalExit);
    QCOMPARE(result.exitCode, 2);
    QVERIFY2(result.output.contains("DONE (FAIL, rc=2)"), result.output.constData());
    QVERIFY2(!result.output.contains("DONE (ERROR"), result.output.constData());
}

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocmmioformal.moc"

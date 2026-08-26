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
    void generatedCollateralPassesSby_data();
    void generatedCollateralPassesSby();
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

void Test::generatedCollateralPassesSby_data()
{
    QTest::addColumn<bool>("wide");
    QTest::addColumn<QString>("moduleName");

    QTest::newRow("32-bit-mixed-fields") << false << QStringLiteral("timer_ctrl");
    QTest::newRow("64-bit-boundary-lanes") << true << QStringLiteral("wide_ctrl");
}

void Test::generatedCollateralPassesSby()
{
    QFETCH(bool, wide);
    QFETCH(QString, moduleName);

    const QString sby   = QStandardPaths::findExecutable(QStringLiteral("sby"));
    const QString yosys = QStandardPaths::findExecutable(QStringLiteral("yosys"));
    const QString z3    = QStandardPaths::findExecutable(QStringLiteral("z3"));
    if (sby.isEmpty() || yosys.isEmpty() || z3.isEmpty()) {
        QSOC_TEST_MISSING_DEPENDENCY(QStringLiteral("sby, yosys, and z3"));
    }

    const QSocModuleDefinition definition = wide ? makeFormal64Definition()
                                                 : makeFormal32Definition();
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

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocmmioformal.moc"

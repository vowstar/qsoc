// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocgenerateprimitiveclock.h"
#include "common/qsocgenerateprimitivepower.h"
#include "common/qsocgenerateprimitivereset.h"
#include "qsoc_test.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtCore>
#include <QtTest>

#include <yaml-cpp/yaml.h>

namespace {

const char *const kResetYaml = R"(
name: rstctl
test_enable: test_en
source:
  por_rst_n: {active: low}
  wdt_rst: {active: high}
target:
  cpu_rst_n:
    active: low
    async: {clock: clk_cpu, stage: 3}
    link:
      por_rst_n:
        async: {clock: clk_sys, stage: 4}
      wdt_rst:
)";

const char *const kClockYaml = R"(
name: clkctl
clock: clk_sys
input:
  osc_24m: {freq: 24MHz}
  pll_a: {freq: 800MHz}
target:
  cpu_clk:
    freq: 800MHz
    select: cpu_sel
    link:
      osc_24m:
      pll_a:
        icg: {enable: cpu_en}
)";

const char *const kPowerYaml = R"(
name: pwrctl
host_clock: clk_ao
host_reset: rst_ao_n
domain:
  - {name: ao, v_mv: 900, wait_dep: 0, settle_on: 0, settle_off: 0, follow: []}
  - name: gpu
    depend: [{name: ao, type: hard}]
    v_mv: 900
    pgood: pgood_gpu
    wait_dep: 20
    settle_on: 12
    settle_off: 8
    follow: [{clock: clk_gpu, reset: rst_gpu_n, stage: 4}]
)";

class Test : public QObject
{
    Q_OBJECT

private slots:
    void generatedDiagramCompiles_data();
    void generatedDiagramCompiles();
};

void Test::generatedDiagramCompiles_data()
{
    QTest::addColumn<QString>("kind");
    QTest::newRow("reset") << QStringLiteral("reset");
    QTest::newRow("clock") << QStringLiteral("clock");
    QTest::newRow("power") << QStringLiteral("power");
}

void Test::generatedDiagramCompiles()
{
    QFETCH(QString, kind);

    const QString typst = QStandardPaths::findExecutable(QStringLiteral("typst"));
    if (typst.isEmpty()) {
        QSOC_TEST_MISSING_DEPENDENCY(QStringLiteral("typst"));
    }

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString source = QDir(dir.path()).filePath(kind + QStringLiteral(".typ"));
    const QString output = QDir(dir.path()).filePath(kind + QStringLiteral(".pdf"));

    bool generated = false;
    if (kind == QStringLiteral("reset")) {
        QSocResetPrimitive primitive;
        const auto         config = primitive.parseResetConfig(YAML::Load(kResetYaml));
        generated                 = primitive.generateTypstDiagram(config, source);
    } else if (kind == QStringLiteral("clock")) {
        QSocClockPrimitive primitive;
        const auto         config = primitive.parseClockConfig(YAML::Load(kClockYaml));
        generated                 = primitive.generateTypstDiagram(config, source);
    } else {
        QSocPowerPrimitive primitive;
        const auto         config = primitive.parsePowerConfig(YAML::Load(kPowerYaml));
        generated                 = primitive.generateTypstDiagram(config, source);
    }
    QVERIFY(generated);

    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(typst, {QStringLiteral("compile"), source, output});
    QVERIFY(process.waitForStarted());
    QVERIFY(process.waitForFinished(300000));
    const QString log = QString::fromUtf8(process.readAll());

    /* Packages come from the network on first use. */
    if (process.exitCode() != 0 && log.contains(QStringLiteral("failed to download package"))) {
        QSOC_TEST_MISSING_DEPENDENCY(QStringLiteral("typst package download"));
    }
    QVERIFY2(process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0, qPrintable(log));
    QVERIFY(QFileInfo(output).size() > 0);
}

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocgeneratetypstcompile.moc"

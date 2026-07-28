// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "cli/qsoccliworker.h"
#include "common/qsocgenerateartifact.h"
#include "common/qsocgeneratemanager.h"
#include "common/qsocgenerateprimitiveclock.h"
#include "common/qsocgenerateprimitivepower.h"
#include "common/qsocgenerateprimitivereset.h"
#include "common/qsocgeneratereportunconnected.h"
#include "common/qsocmodulemanager.h"
#include "common/qsocprojectmanager.h"
#include "qsoc_test.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QMap>
#include <QProcess>
#include <QProcessEnvironment>
#include <QScopeGuard>
#include <QStringList>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>
#include <QtTest>

#include <yaml-cpp/yaml.h>

#include <atomic>
#include <thread>
#include <vector>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

namespace {

YAML::Node controllerFixture(const QString &kind, const QString &name)
{
    YAML::Node node;
    if (kind == "clock") {
        node = YAML::Load(R"(
name: controller
clock: clk_sys
input:
  osc_24m:
    freq: 24MHz
target:
  adc_clk:
    freq: 24MHz
    link:
      osc_24m:
)");
    } else if (kind == "reset") {
        node = YAML::Load(R"(
name: controller
clock: clk_sys
source:
  por_rst_n:
    active: low
target:
  cpu_rst_n:
    active: low
    link:
      por_rst_n:
)");
    } else {
        node = YAML::Load(R"(
name: controller
host_clock: clk_ao
host_reset: rst_ao_n
domain:
  - name: ao
    v_mv: 900
    pgood: pgood_ao
    wait_dep: 0
    settle_on: 0
    settle_off: 0
    follow: []
)");
    }
    node["name"] = name.toStdString();
    return node;
}

bool generateController(
    const QString       &kind,
    const YAML::Node    &node,
    QSocGenerateManager &manager,
    bool                 force           = false,
    QString             *generatedOutput = nullptr)
{
    QString     generated;
    QTextStream out(&generated);
    bool        result = false;
    if (kind == "clock") {
        QSocClockPrimitive primitive(&manager);
        primitive.setForceOverwrite(force);
        result = primitive.generateClockController(node, out);
    } else if (kind == "reset") {
        QSocResetPrimitive primitive(&manager);
        primitive.setForceOverwrite(force);
        result = primitive.generateResetController(node, out);
    } else {
        QSocPowerPrimitive primitive(&manager);
        primitive.setForceOverwrite(force);
        result = primitive.generatePowerController(node, out);
    }
    if (generatedOutput) {
        *generatedOutput = generated;
    }
    return result;
}

bool generateDiagram(const QString &kind, const QString &path)
{
    if (kind == "clock") {
        QSocClockPrimitive primitive;
        return primitive.generateTypstDiagram({}, path);
    }
    if (kind == "reset") {
        QSocResetPrimitive primitive;
        return primitive.generateTypstDiagram({}, path);
    }
    QSocPowerPrimitive primitive;
    return primitive.generateTypstDiagram({}, path);
}

YAML::Node sequentialOrderingFixture()
{
    return YAML::Load(R"(
port:
  clk:
    direction: input
    type: logic
  source:
    direction: input
    type: logic
  seq_z:
    direction: output
    type: logic
  seq_a:
    direction: output
    type: logic
  seq_m:
    direction: output
    type: logic
  seq_b:
    direction: output
    type: logic
  seq_y:
    direction: output
    type: logic
  seq_c:
    direction: output
    type: logic
  seq_x:
    direction: output
    type: logic
  seq_d:
    direction: output
    type: logic
seq:
  - reg: seq_z
    clk: clk
    next: source
  - reg: seq_a
    clk: clk
    next: source
  - reg: seq_m
    clk: clk
    next: source
  - reg: seq_b
    clk: clk
    next: source
  - reg: seq_y
    clk: clk
    next: source
  - reg: seq_c
    clk: clk
    next: source
  - reg: seq_x
    clk: clk
    next: source
  - reg: seq_d
    clk: clk
    next: source
)");
}

class Test : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir      temporaryDirectory;
    QSocProjectManager projectManager;

    QByteArray readOutput(const QString &fileName)
    {
        QFile file(QDir(projectManager.getOutputPath()).filePath(fileName));
        if (!file.open(QIODevice::ReadOnly)) {
            return {};
        }
        return file.readAll();
    }

    bool generateArtifacts(QMap<QString, QByteArray> &artifacts)
    {
        const YAML::Node netlist = YAML::Load(R"(
port:
  source:
    direction: input
    type: logic
  result:
    direction: output
    type: logic
comb:
  - out: result
    expr: source
)");

        QSocGenerateManager manager(nullptr, &projectManager);
        if (!manager.setNetlistData(netlist) || !manager.processNetlist()
            || !manager.generateVerilog("stable")
            || !manager.generateVerilogStub("stable_stub", {})) {
            return false;
        }

        QSocGenerateReportUnconnected                      reporter;
        QSocGenerateReportUnconnected::UnconnectedPortInfo port;
        port.instanceName = "u_source";
        port.moduleName   = "source";
        port.portName     = "unused";
        port.direction    = "input";
        port.type         = "logic";
        reporter.addUnconnectedPort(port);
        if (!reporter.generateReport(projectManager.getOutputPath(), "stable")) {
            return false;
        }

        const QString clockPath = QDir(projectManager.getOutputPath()).filePath("clock.typ");
        const QString resetPath = QDir(projectManager.getOutputPath()).filePath("reset.typ");
        const QString powerPath = QDir(projectManager.getOutputPath()).filePath("power.typ");

        QSocClockPrimitive clock;
        QSocResetPrimitive reset;
        QSocPowerPrimitive power;
        if (!clock.generateTypstDiagram({}, clockPath) || !reset.generateTypstDiagram({}, resetPath)
            || !power.generateTypstDiagram({}, powerPath)) {
            return false;
        }

        const QStringList fileNames{
            "stable.v", "stable_stub.v", "stable.nc.rpt", "clock.typ", "reset.typ", "power.typ"};
        for (const QString &fileName : fileNames) {
            const QByteArray content = readOutput(fileName);
            if (content.isEmpty()) {
                return false;
            }
            artifacts.insert(fileName, content);
        }
        return true;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(temporaryDirectory.isValid());
        projectManager.setProjectName("reproducibility");
        projectManager.setCurrentPath(QDir(temporaryDirectory.path()).filePath("reproducibility"));
        QVERIFY(projectManager.mkpath());
    }

    void cleanupTestCase() { QVERIFY(QDir(temporaryDirectory.path()).removeRecursively()); }

    void applicationMetadataDoesNotChangeArtifacts()
    {
        const QString originalName    = QCoreApplication::applicationName();
        const QString originalVersion = QCoreApplication::applicationVersion();
        const auto    restoreMetadata = qScopeGuard([&]() {
            QCoreApplication::setApplicationName(originalName);
            QCoreApplication::setApplicationVersion(originalVersion);
        });

        QCoreApplication::setApplicationName("first-name");
        QCoreApplication::setApplicationVersion("1.2.3");
        QMap<QString, QByteArray> first;
        QVERIFY(generateArtifacts(first));

        QCoreApplication::setApplicationName("second-name");
        QCoreApplication::setApplicationVersion("9.8.7");
        QMap<QString, QByteArray> second;
        QVERIFY(generateArtifacts(second));

        QCOMPARE(second, first);
        for (auto it = first.cbegin(); it != first.cend(); ++it) {
            QVERIFY2(
                it.value().contains("Generated by QSoC."),
                qPrintable(it.key() + " lacks stable generator metadata"));
            QVERIFY(!it.value().contains("first-name"));
            QVERIFY(!it.value().contains("second-name"));
            QVERIFY(!it.value().contains("1.2.3"));
            QVERIFY(!it.value().contains("9.8.7"));
            QVERIFY(!it.value().contains("# Generated:"));
            QVERIFY(!it.value().contains('\r'));
        }
    }

    void sequentialOutputIsStableAcrossProcessHashSeeds()
    {
        const QString mode = qEnvironmentVariable("QSOC_TEST_REPRO_SEQ_MODE");
        if (!mode.isEmpty()) {
            QVERIFY(mode == "zero" || mode == "random");

            const QString projectPath = qEnvironmentVariable("QSOC_TEST_REPRO_SEQ_PROJECT");
            QVERIFY(!projectPath.isEmpty());
            QSocProjectManager manager;
            manager.setCurrentPath(projectPath);
            QVERIFY(manager.mkpath());
            QSocGenerateManager generator(nullptr, &manager);
            QVERIFY(generator.setNetlistData(sequentialOrderingFixture()));
            QVERIFY(generator.processNetlist());
            QVERIFY(generator.generateVerilog("seq_order"));
            return;
        }

        QTemporaryDir runDirectory;
        QVERIFY(runDirectory.isValid());
        QByteArray baseline;
        for (int run = 0; run < 9; ++run) {
            const QString runPath = QDir(runDirectory.path()).filePath(QString("run_%1").arg(run));
            const QString projectPath = QDir(runPath).filePath("project");
            QVERIFY(QDir().mkpath(runPath));

            QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
            environment.insert("QSOC_TEST_REPRO_SEQ_MODE", run == 0 ? "zero" : "random");
            environment.insert("QSOC_TEST_REPRO_SEQ_PROJECT", projectPath);
            environment.insert("TMPDIR", runPath);
            if (run == 0) {
                environment.insert("QT_HASH_SEED", "0");
            } else {
                environment.remove("QT_HASH_SEED");
            }

            QProcess child;
            child.setProcessEnvironment(environment);
            child.setProcessChannelMode(QProcess::MergedChannels);
            child.setWorkingDirectory(runPath);
            child.start(
                QCoreApplication::applicationFilePath(),
                {"sequentialOutputIsStableAcrossProcessHashSeeds"});
            QVERIFY(child.waitForStarted(5000));
            const bool stopped = child.waitForFinished(30000);
            if (!stopped) {
                child.kill();
                child.waitForFinished(1000);
            }
            const QByteArray childOutput = child.readAll();
            QVERIFY2(stopped, childOutput.constData());
            QVERIFY2(
                child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0,
                childOutput.constData());

            QFile outputFile(QDir(projectPath).filePath("output/seq_order.v"));
            QVERIFY(outputFile.open(QIODevice::ReadOnly));
            const QByteArray output = outputFile.readAll();
            QVERIFY(!output.isEmpty());
            if (baseline.isEmpty()) {
                baseline = output;
            } else {
                QCOMPARE(output, baseline);
            }
        }

        const QStringList
            expectedOrder{"seq_z", "seq_a", "seq_m", "seq_b", "seq_y", "seq_c", "seq_x", "seq_d"};
        qsizetype previousDeclaration = -1;
        qsizetype previousAssignment  = -1;
        for (const QString &name : expectedOrder) {
            const QByteArray declaration = "    reg " + name.toUtf8() + "_reg;";
            const QByteArray assignment  = "    assign " + name.toUtf8() + " = " + name.toUtf8()
                                           + "_reg;";
            QCOMPARE(baseline.count(declaration), 1);
            QCOMPARE(baseline.count(assignment), 1);
            const qsizetype declarationPosition = baseline.indexOf(declaration);
            const qsizetype assignmentPosition  = baseline.indexOf(assignment);
            QVERIFY(declarationPosition > previousDeclaration);
            QVERIFY(assignmentPosition > previousAssignment);
            previousDeclaration = declarationPosition;
            previousAssignment  = assignmentPosition;
        }
    }

    void pathFormatterIsExplicitOnly()
    {
        QTemporaryDir formatterDirectory;
        QTemporaryDir emptyPathDirectory;
        QTemporaryDir baselineDirectory;
        QTemporaryDir projectDirectory;
        QVERIFY(formatterDirectory.isValid());
        QVERIFY(emptyPathDirectory.isValid());
        QVERIFY(baselineDirectory.isValid());
        QVERIFY(projectDirectory.isValid());

#ifdef Q_OS_WIN
        const QString formatterName = "verible-verilog-format.exe";
#else
        const QString formatterName = "verible-verilog-format";
#endif
        const QString formatterPath = QDir(formatterDirectory.path()).filePath(formatterName);
        QVERIFY(QFile::copy(QString::fromUtf8(QSOC_FORMATTER_PROBE_PATH), formatterPath));
#ifndef Q_OS_WIN
        QVERIFY(
            QFile::setPermissions(
                formatterPath,
                QFile::permissions(formatterPath) | QFileDevice::ExeOwner | QFileDevice::ExeUser
                    | QFileDevice::ExeGroup | QFileDevice::ExeOther));
#endif

        const QByteArray originalPath     = qgetenv("PATH");
        const bool       hadOriginalPath  = qEnvironmentVariableIsSet("PATH");
        const QByteArray originalSentinel = qgetenv("QSOC_FORMATTER_PROBE_SENTINEL");
        const bool       hadSentinel = qEnvironmentVariableIsSet("QSOC_FORMATTER_PROBE_SENTINEL");
        const QByteArray originalProbeExit = qgetenv("QSOC_FORMATTER_PROBE_EXIT_CODE");
        const bool       hadProbeExit = qEnvironmentVariableIsSet("QSOC_FORMATTER_PROBE_EXIT_CODE");
        const auto       restoreEnvironment = qScopeGuard([&]() {
            if (hadOriginalPath) {
                qputenv("PATH", originalPath);
            } else {
                qunsetenv("PATH");
            }
            if (hadSentinel) {
                qputenv("QSOC_FORMATTER_PROBE_SENTINEL", originalSentinel);
            } else {
                qunsetenv("QSOC_FORMATTER_PROBE_SENTINEL");
            }
            if (hadProbeExit) {
                qputenv("QSOC_FORMATTER_PROBE_EXIT_CODE", originalProbeExit);
            } else {
                qunsetenv("QSOC_FORMATTER_PROBE_EXIT_CODE");
            }
        });

        QByteArray testPath = QFile::encodeName(formatterDirectory.path());
        if (!originalPath.isEmpty()) {
            testPath += QDir::listSeparator().toLatin1();
            testPath += originalPath;
        }
        qputenv("PATH", QFile::encodeName(emptyPathDirectory.path()));
        const QString sentinelPath = QDir(formatterDirectory.path()).filePath("called");
        qputenv("QSOC_FORMATTER_PROBE_SENTINEL", QFile::encodeName(sentinelPath));

        const YAML::Node netlist = YAML::Load(R"(
port:
  clk_sys:
    direction: input
    type: logic
  osc_24m:
    direction: input
    type: logic
  por_rst_n:
    direction: input
    type: logic
  test_en:
    direction: input
    type: logic
  adc_clk:
    direction: output
    type: logic
  cpu_rst_n:
    direction: output
    type: logic
clock:
  - name: clock_controller
    clock: clk_sys
    input:
      osc_24m:
        freq: 24MHz
    target:
      adc_clk:
        freq: 24MHz
        link:
          osc_24m:
reset:
  - name: reset_controller
    clock: clk_sys
    test_enable: test_en
    source:
      por_rst_n:
        active: low
    target:
      cpu_rst_n:
        active: low
        link:
          por_rst_n:
)");

        QMap<QString, QByteArray> baselineFiles;
        {
            QSocProjectManager baselineManager;
            baselineManager.setCurrentPath(QDir(baselineDirectory.path()).filePath("project"));
            QVERIFY(baselineManager.mkpath());
            QSocGenerateManager baselineGenerator(nullptr, &baselineManager);
            QVERIFY(baselineGenerator.setNetlistData(netlist));
            QVERIFY(baselineGenerator.processNetlist());
            QVERIFY(baselineGenerator.generateVerilog("canonical"));
            const QStringList fileNames{"canonical.v", "clock_cell.v", "reset_cell.v"};
            for (const QString &fileName : fileNames) {
                QFile file(QDir(baselineManager.getOutputPath()).filePath(fileName));
                QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(fileName));
                baselineFiles.insert(fileName, file.readAll());
            }
        }

        qputenv("PATH", testPath);
        const QString      projectPath = QDir(projectDirectory.path()).filePath("project");
        QSocProjectManager manager;
        manager.setCurrentPath(projectPath);
        QVERIFY(manager.mkpath());
        QVERIFY(manager.save("formatter"));
        QSocGenerateManager generator(nullptr, &manager);
        QVERIFY(generator.setNetlistData(netlist));
        QVERIFY(generator.processNetlist());
        QVERIFY(generator.generateVerilog("canonical"));

        const QStringList defaultFiles{"canonical.v", "clock_cell.v", "reset_cell.v"};
        for (const QString &fileName : defaultFiles) {
            QFile file(QDir(manager.getOutputPath()).filePath(fileName));
            QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(fileName));
            const QByteArray bytes = file.readAll();
            QVERIFY2(!bytes.contains("formatter probe"), qPrintable(fileName));
            QCOMPARE(bytes, baselineFiles.value(fileName));
        }
        QVERIFY(!QFileInfo::exists(sentinelPath));

        const QString netlistPath = QDir(projectPath).filePath("formatted.soc_net");
        QFile         netlistFile(netlistPath);
        QVERIFY(netlistFile.open(QIODevice::WriteOnly));
        const QByteArray netlistBytes = QByteArray::fromStdString(YAML::Dump(netlist));
        QCOMPARE(netlistFile.write(netlistBytes), netlistBytes.size());
        netlistFile.close();

        QSocCliWorker invalidWorker;
        QSignalSpy    invalidExitSpy(&invalidWorker, &QSocCliWorker::exit);
        invalidWorker.setup(
            {"qsoc", "generate", "verilog", "--format=bad", "-d", projectPath, netlistPath}, true);
        QVERIFY(invalidExitSpy.wait());
        QCOMPARE(invalidExitSpy.count(), 1);
        QCOMPARE(invalidExitSpy.takeFirst().at(0).toInt(), 1);
        QVERIFY(!QFileInfo::exists(sentinelPath));
        QVERIFY(!QFileInfo::exists(QDir(manager.getOutputPath()).filePath("formatted.v")));

        QSocCliWorker worker;
        QSignalSpy    exitSpy(&worker, &QSocCliWorker::exit);
        worker
            .setup({"qsoc", "generate", "verilog", "--format", "-d", projectPath, netlistPath}, true);
        QVERIFY(exitSpy.wait());
        QCOMPARE(exitSpy.count(), 1);
        QCOMPARE(exitSpy.takeFirst().at(0).toInt(), 0);

        QVERIFY(QFileInfo::exists(sentinelPath));
        QFile formattedFile(QDir(manager.getOutputPath()).filePath("formatted.v"));
        QVERIFY(formattedFile.open(QIODevice::ReadOnly));
        QVERIFY(formattedFile.readAll().contains("formatter probe"));
        formattedFile.close();

        QVERIFY(QFile::remove(sentinelPath));
        qputenv("PATH", QFile::encodeName(emptyPathDirectory.path()));
        QSocCliWorker missingWorker;
        QSignalSpy    missingExitSpy(&missingWorker, &QSocCliWorker::exit);
        missingWorker
            .setup({"qsoc", "generate", "verilog", "--format", "-d", projectPath, netlistPath}, true);
        QVERIFY(missingExitSpy.wait());
        QCOMPARE(missingExitSpy.count(), 1);
        QCOMPARE(missingExitSpy.takeFirst().at(0).toInt(), 1);
        QVERIFY(!QFileInfo::exists(sentinelPath));

        qputenv("PATH", testPath);
        qputenv("QSOC_FORMATTER_PROBE_EXIT_CODE", "9");
        QSocTestCapture capture;
        QVERIFY(!QSocGenerateManager::formatVerilogFile(
            QDir(manager.getOutputPath()).filePath("formatted.v")));
        const QString diagnostic = capture.text();
        QVERIFY(diagnostic.contains("exit code 9"));
        QVERIFY(diagnostic.contains("formatter probe failure"));
    }

    void projectMetadataDoesNotChangeBytes()
    {
        const QString originalVersion = QCoreApplication::applicationVersion();
        const auto    restoreVersion  = qScopeGuard(
            [&]() { QCoreApplication::setApplicationVersion(originalVersion); });
        QTemporaryDir firstDirectory;
        QTemporaryDir secondDirectory;
        QVERIFY(firstDirectory.isValid());
        QVERIFY(secondDirectory.isValid());

        const QString firstProjectPath = QDir(firstDirectory.path()).filePath("project");
        QCoreApplication::setApplicationVersion("1.2.3");
        QSocProjectManager firstManager;
        firstManager.setCurrentPath(firstProjectPath);
        firstManager.setEnv("QSOC_NOISE", firstProjectPath);
        QVERIFY(firstManager.create("stable"));
        QFile firstFile(QDir(firstProjectPath).filePath("stable.soc_pro"));
        QVERIFY(firstFile.open(QIODevice::ReadOnly));
        const QByteArray firstBytes = firstFile.readAll();

        const QString secondProjectPath = QDir(secondDirectory.path()).filePath("project");
        QCoreApplication::setApplicationVersion("9.8.7");
        QSocProjectManager secondManager;
        secondManager.setCurrentPath(secondProjectPath);
        secondManager.setEnv("QSOC_NOISE", QDir(secondDirectory.path()).filePath("unrelated"));
        QVERIFY(secondManager.create("stable"));
        QFile secondFile(QDir(secondProjectPath).filePath("stable.soc_pro"));
        QVERIFY(secondFile.open(QIODevice::ReadOnly));
        const QByteArray secondBytes = secondFile.readAll();

        QCOMPARE(secondBytes, firstBytes);
        QVERIFY(!firstBytes.contains("version"));
        QVERIFY(!firstBytes.contains("QSOC_NOISE"));
        QVERIFY(!firstBytes.contains('\r'));
        QVERIFY(firstBytes.endsWith('\n'));
        QVERIFY(!firstBytes.chopped(1).endsWith('\n'));
    }

    void legacyProjectMetadataIsIgnored()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QByteArray label = QStringLiteral("芯片").toUtf8();
        QByteArray       input = "version: 9999.0.0\n"
                                 "bus: ${QSOC_PROJECT_DIR}/bus\n"
                                 "module: ${QSOC_PROJECT_DIR}/module\n"
                                 "schematic: ${QSOC_PROJECT_DIR}/schematic\n"
                                 "output: ${QSOC_PROJECT_DIR}/output\n"
                                 "extension:\n"
                                 "  label: \"";
        input += label;
        input += "\"\n"
                 "  note: |+\n"
                 "    first\n"
                 "\n";

        const QString projectFilePath = QDir(directory.path()).filePath("legacy.soc_pro");
        QFile         projectFile(projectFilePath);
        QVERIFY(projectFile.open(QIODevice::WriteOnly));
        QCOMPARE(projectFile.write(input), input.size());
        projectFile.close();

        QSocProjectManager manager;
        manager.setCurrentPath(directory.path());
        QVERIFY(manager.load("legacy"));
        QVERIFY(manager.save("legacy"));

        QVERIFY(projectFile.open(QIODevice::ReadOnly));
        const QByteArray savedBytes = projectFile.readAll();
        QVERIFY(!savedBytes.contains("version"));
        QVERIFY(savedBytes.contains(label));
        QVERIFY(!savedBytes.contains('\r'));
        QVERIFY(savedBytes.endsWith('\n'));
        QVERIFY(!savedBytes.chopped(1).endsWith('\n'));

        const YAML::Node savedNode = YAML::Load(
            std::string(savedBytes.constData(), savedBytes.size()));
        QVERIFY(!savedNode["version"].IsDefined());
        QCOMPARE(
            QString::fromStdString(savedNode["extension"]["label"].as<std::string>()),
            QStringLiteral("芯片"));
        QCOMPARE(
            QString::fromStdString(savedNode["extension"]["note"].as<std::string>()),
            QString("first\n\n"));
    }

    void newProjectDoesNotInheritLoadedExtensions()
    {
        QTemporaryDir sourceDirectory;
        QTemporaryDir targetDirectory;
        QVERIFY(sourceDirectory.isValid());
        QVERIFY(targetDirectory.isValid());
        const QByteArray sourceBytes = "bus: bus\n"
                                       "module: module\n"
                                       "schematic: schematic\n"
                                       "output: output\n"
                                       "extension: retained-only-by-save\n";
        QFile            sourceFile(QDir(sourceDirectory.path()).filePath("source.soc_pro"));
        QVERIFY(sourceFile.open(QIODevice::WriteOnly));
        QCOMPARE(sourceFile.write(sourceBytes), sourceBytes.size());
        sourceFile.close();

        QSocProjectManager manager;
        manager.setCurrentPath(sourceDirectory.path());
        QVERIFY(manager.load("source"));
        manager.setCurrentPath(targetDirectory.path());
        QVERIFY(manager.create("target"));

        QFile targetFile(QDir(targetDirectory.path()).filePath("target.soc_pro"));
        QVERIFY(targetFile.open(QIODevice::ReadOnly));
        const QByteArray targetBytes = targetFile.readAll();
        const YAML::Node targetNode  = YAML::Load(
            std::string(targetBytes.constData(), targetBytes.size()));
        QVERIFY(!targetNode["extension"].IsDefined());
    }

    void relativeProjectPathUsesProjectVariable()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString previousPath = QDir::currentPath();
        const auto    restorePath = qScopeGuard([&]() { QVERIFY(QDir::setCurrent(previousPath)); });
        QVERIFY(QDir::setCurrent(directory.path()));

        QSocProjectManager manager;
        manager.setCurrentPath(".");
        QVERIFY(manager.create("relative"));

        QFile projectFile(QDir(directory.path()).filePath("relative.soc_pro"));
        QVERIFY(projectFile.open(QIODevice::ReadOnly));
        const QByteArray projectBytes = projectFile.readAll();
        const YAML::Node projectNode  = YAML::Load(
            std::string(projectBytes.constData(), projectBytes.size()));
        QCOMPARE(
            QString::fromStdString(projectNode["bus"].as<std::string>()),
            QString("${QSOC_PROJECT_DIR}/bus"));
        QCOMPARE(
            QString::fromStdString(projectNode["module"].as<std::string>()),
            QString("${QSOC_PROJECT_DIR}/module"));
        QCOMPARE(
            QString::fromStdString(projectNode["schematic"].as<std::string>()),
            QString("${QSOC_PROJECT_DIR}/schematic"));
        QCOMPARE(
            QString::fromStdString(projectNode["output"].as<std::string>()),
            QString("${QSOC_PROJECT_DIR}/output"));
    }

    void invalidProjectSchemaDoesNotChangeState_data()
    {
        QTest::addColumn<QByteArray>("projectBytes");

        QTest::newRow("malformed") << QByteArray("bus: [\n");
        QTest::newRow("sequence-root") << QByteArray("- bus\n- module\n");
        QTest::newRow("missing-output")
            << QByteArray("bus: bus\nmodule: module\nschematic: schematic\nversion: 1.0\n");
        QTest::newRow("non-scalar")
            << QByteArray("bus: []\nmodule: module\nschematic: schematic\noutput: output\n");
        QTest::newRow("empty-path")
            << QByteArray("bus: ''\nmodule: module\nschematic: schematic\noutput: output\n");
    }

    void invalidProjectSchemaDoesNotChangeState()
    {
        QFETCH(QByteArray, projectBytes);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        QSocProjectManager manager;
        manager.setCurrentPath(directory.path());
        manager.setProjectName("before");
        manager.setEnv("QSOC_NOISE", "unchanged");
        const YAML::Node initialNode = YAML::Load(R"(
bus: bus
module: module
schematic: schematic
output: output
extension: retained
)");
        manager.setProjectNode(initialNode);
        const auto previousState = manager.captureState();

        QFile projectFile(QDir(directory.path()).filePath("invalid.soc_pro"));
        QVERIFY(projectFile.open(QIODevice::WriteOnly));
        QCOMPARE(projectFile.write(projectBytes), projectBytes.size());
        projectFile.close();

        QVERIFY(!manager.load("invalid"));
        const auto currentState = manager.captureState();
        QCOMPARE(currentState.env, previousState.env);
        QCOMPARE(currentState.projectName, previousState.projectName);
        QCOMPARE(currentState.projectPath, previousState.projectPath);
        QCOMPARE(currentState.busPath, previousState.busPath);
        QCOMPARE(currentState.modulePath, previousState.modulePath);
        QCOMPARE(currentState.schematicPath, previousState.schematicPath);
        QCOMPARE(currentState.outputPath, previousState.outputPath);
        QCOMPARE(currentState.currentPath, previousState.currentPath);
        QCOMPARE(
            QString::fromStdString(YAML::Dump(currentState.projectNode)),
            QString::fromStdString(YAML::Dump(previousState.projectNode)));
    }

    void loadFirstReportsInvalidProject()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QByteArray invalidBytes
            = "bus: bus\nmodule: module\nschematic: schematic\noutput: []\n";
        QFile projectFile(QDir(directory.path()).filePath("first.soc_pro"));
        QVERIFY(projectFile.open(QIODevice::WriteOnly));
        QCOMPARE(projectFile.write(invalidBytes), invalidBytes.size());
        projectFile.close();

        QSocProjectManager manager;
        manager.setCurrentPath(directory.path());
        QVERIFY(!manager.loadFirst(true));
    }

    void projectPathSubstitutionUsesDirectoryBoundary()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString projectPath = QDir(directory.path()).filePath("project");
        const QString siblingPath = QDir(directory.path()).filePath("project_evil/bus");

        QSocProjectManager manager;
        manager.setCurrentPath(projectPath);
        manager.setBusPath(siblingPath);
        const YAML::Node &projectYaml = manager.getProjectYaml();

        QCOMPARE(
            QString::fromStdString(projectYaml["module"].as<std::string>()),
            QString("${QSOC_PROJECT_DIR}/module"));
        QCOMPARE(QString::fromStdString(projectYaml["bus"].as<std::string>()), siblingPath);
    }

    void concurrentProjectCreateHasOneWinner()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString projectPath = QDir(directory.path()).filePath("project");

        std::atomic<int>          startState   = 0;
        std::atomic<int>          successCount = 0;
        std::vector<std::jthread> workers;
        workers.reserve(2);

        try {
            for (int worker = 0; worker < 2; ++worker) {
                workers.emplace_back([&]() {
                    QSocProjectManager manager;
                    manager.setCurrentPath(projectPath);
                    startState.wait(0);
                    if (startState.load() == 1 && manager.create("shared")) {
                        ++successCount;
                    }
                });
            }
        } catch (...) {
            startState.store(2);
            startState.notify_all();
            throw;
        }

        startState.store(1);
        startState.notify_all();
        workers.clear();

        QCOMPARE(successCount.load(), 1);
        QFile projectFile(QDir(projectPath).filePath("shared.soc_pro"));
        QVERIFY(projectFile.open(QIODevice::ReadOnly));
        QVERIFY(!projectFile.readAll().isEmpty());
        QVERIFY(!QFile::exists(projectFile.fileName() + ".lock"));
    }

    void heldProjectLockBlocksCreate()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString projectPath = QDir(directory.path()).filePath("project");
        QVERIFY(QDir().mkpath(projectPath));

        const QString projectFilePath = QDir(projectPath).filePath("locked.soc_pro");
        QLockFile     projectLock(projectFilePath + ".lock");
        QVERIFY(projectLock.tryLock());

        QSocProjectManager manager;
        manager.setCurrentPath(projectPath);
        QVERIFY(!manager.create("locked"));
        QVERIFY(!QFileInfo::exists(projectFilePath));

        projectLock.unlock();
        QVERIFY(manager.create("locked"));
        QVERIFY(QFileInfo::exists(projectFilePath));
    }

    void projectNameCannotEscapeDirectory()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString projectPath = QDir(directory.path()).filePath("project");

        QSocProjectManager manager;
        manager.setCurrentPath(projectPath);

        QVERIFY(!manager.create("../escaped"));
        QVERIFY(!QFileInfo::exists(QDir(directory.path()).filePath("escaped.soc_pro")));
        QVERIFY(!QFileInfo::exists(projectPath));
    }

    void existingMarkerLinkPreservesTarget()
    {
#ifndef Q_OS_UNIX
        QSKIP("This platform does not provide Unix symbolic-link semantics.");
#else
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString projectPath = QDir(directory.path()).filePath("project");
        const QString busPath     = QDir(projectPath).filePath("bus");
        QVERIFY(QDir().mkpath(busPath));

        const QByteArray sentinelBytes(4096, 's');
        const QString    sentinelPath = QDir(directory.path()).filePath("sentinel");
        QFile            sentinelFile(sentinelPath);
        QVERIFY(sentinelFile.open(QIODevice::WriteOnly));
        QCOMPARE(sentinelFile.write(sentinelBytes), sentinelBytes.size());
        sentinelFile.close();

        const QString markerPath = QDir(busPath).filePath(".gitkeep");
        QVERIFY(QFile::link(sentinelPath, markerPath));

        QSocProjectManager manager;
        manager.setCurrentPath(projectPath);
        QVERIFY(manager.create("safe"));

        QVERIFY(sentinelFile.open(QIODevice::ReadOnly));
        QCOMPARE(sentinelFile.readAll(), sentinelBytes);
        QVERIFY(QFileInfo(markerPath).isSymLink());
#endif
    }

    void lateProjectLinkCannotClobberTarget()
    {
#ifndef Q_OS_UNIX
        QSKIP("This platform does not provide Unix symbolic-link semantics.");
#else
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString projectPath = QDir(directory.path()).filePath("project");
        QString       deepBusPath = QDir(projectPath).filePath("bus");
        for (int depth = 0; depth < 256; ++depth) {
            deepBusPath = QDir(deepBusPath).filePath("d");
        }

        const QByteArray sentinelBytes(4096, 's');
        const QString    sentinelPath = QDir(directory.path()).filePath("sentinel");
        QFile            sentinelFile(sentinelPath);
        QVERIFY(sentinelFile.open(QIODevice::WriteOnly));
        QCOMPARE(sentinelFile.write(sentinelBytes), sentinelBytes.size());
        sentinelFile.close();

        std::atomic_bool createResult = false;
        std::atomic_bool finished     = false;
        std::jthread     worker([&]() {
            QSocProjectManager manager;
            manager.setCurrentPath(projectPath);
            manager.setBusPath(deepBusPath);
            createResult.store(manager.create("race"));
            finished.store(true);
        });

        const QString  gitignorePath  = QDir(projectPath).filePath(".gitignore");
        QDeadlineTimer waitForPrepare = QDeadlineTimer(5000);
        while (!QFileInfo::exists(gitignorePath) && !finished.load()
               && !waitForPrepare.hasExpired()) {
            QThread::yieldCurrentThread();
        }
        QVERIFY2(QFileInfo::exists(gitignorePath), "Project preparation did not start.");
        QVERIFY2(!finished.load(), "Project preparation completed before conflict injection.");

        const QString projectFilePath = QDir(projectPath).filePath("race.soc_pro");
        QVERIFY(QFile::link(sentinelPath, projectFilePath));
        worker.join();

        QVERIFY(!createResult.load());
        QVERIFY(sentinelFile.open(QIODevice::ReadOnly));
        QCOMPARE(sentinelFile.readAll(), sentinelBytes);
        QVERIFY(QFileInfo(projectFilePath).isSymLink());
#endif
    }

    void failedProjectSavePreservesTarget()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        QSocProjectManager manager;
        manager.setCurrentPath(directory.path());
        QVERIFY(manager.mkpath());

        const QString projectFilePath = QDir(directory.path()).filePath("blocked.soc_pro");
        QVERIFY(QDir().mkpath(projectFilePath));
        const QByteArray sentinelBytes(4096, 's');
        QFile            sentinelFile(QDir(projectFilePath).filePath("sentinel"));
        QVERIFY(sentinelFile.open(QIODevice::WriteOnly));
        QCOMPARE(sentinelFile.write(sentinelBytes), sentinelBytes.size());
        sentinelFile.close();

        const QStringList entriesBefore
            = QDir(directory.path())
                  .entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot, QDir::Name);
        QVERIFY(!manager.save("blocked"));

        const QStringList entriesAfter
            = QDir(directory.path())
                  .entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot, QDir::Name);
        QCOMPARE(entriesAfter, entriesBefore);
        QVERIFY(sentinelFile.open(QIODevice::ReadOnly));
        QCOMPARE(sentinelFile.readAll(), sentinelBytes);
    }

    void primitiveCellLinkIsRejected_data()
    {
        QTest::addColumn<QString>("kind");
        for (const QString &kind : {"clock", "reset", "power"}) {
            QTest::newRow(qPrintable(kind)) << kind;
        }
    }

    void directTypstWriterPreservesRelativePath_data()
    {
        QTest::addColumn<QString>("kind");
        QTest::newRow("clock") << QStringLiteral("clock");
        QTest::newRow("reset") << QStringLiteral("reset");
        QTest::newRow("power") << QStringLiteral("power");
    }

    void primitiveCellOwnership_data()
    {
        QTest::addColumn<QString>("kind");
        QTest::addColumn<QString>("state");
        QTest::addColumn<bool>("force");
        for (const QString &kind : {"clock", "reset", "power"}) {
            for (const QString &state : {"absent", "canonical", "opaque"}) {
                for (const bool force : {false, true}) {
                    const QString rowName
                        = QString("%1-%2-%3").arg(kind, state, force ? "force" : "default");
                    QTest::newRow(qPrintable(rowName)) << kind << state << force;
                }
            }
        }
    }

    void primitiveCellOwnership()
    {
        QFETCH(QString, kind);
        QFETCH(QString, state);
        QFETCH(bool, force);

        QTemporaryDir canonicalDirectory;
        QTemporaryDir targetDirectory;
        QVERIFY(canonicalDirectory.isValid());
        QVERIFY(targetDirectory.isValid());

        QSocProjectManager canonicalProject;
        canonicalProject.setCurrentPath(
            QDir(canonicalDirectory.path()).filePath("canonical_project"));
        QVERIFY(canonicalProject.mkpath());
        QSocGenerateManager canonicalGenerator(nullptr, &canonicalProject);
        QVERIFY(generateController(
            kind, controllerFixture(kind, "canonical_controller"), canonicalGenerator));

        const QString cellName = kind + "_cell.v";
        QFile         canonicalFile(QDir(canonicalProject.getOutputPath()).filePath(cellName));
        QVERIFY(canonicalFile.open(QIODevice::ReadOnly));
        const QByteArray canonicalBytes = canonicalFile.readAll();
        canonicalFile.close();
        QVERIFY(!canonicalBytes.isEmpty());

        QSocProjectManager targetProject;
        targetProject.setCurrentPath(QDir(targetDirectory.path()).filePath("target_project"));
        QVERIFY(targetProject.mkpath());
        QSocGenerateManager targetGenerator(nullptr, &targetProject);
        const QString       targetCellPath = QDir(targetProject.getOutputPath()).filePath(cellName);

        QByteArray seededBytes;
        if (state == "canonical") {
            seededBytes = canonicalBytes + "// user-owned cell\n";
        } else if (state == "opaque") {
            seededBytes = "`include \"technology_cells.vh\"\n";
        }
        if (!seededBytes.isEmpty()) {
            QFile targetCell(targetCellPath);
            QVERIFY(targetCell.open(QIODevice::WriteOnly));
            QCOMPARE(targetCell.write(seededBytes), seededBytes.size());
            targetCell.close();
        }

        const QDir        outputDirectory(targetProject.getOutputPath());
        const QStringList entriesBefore
            = outputDirectory
                  .entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot, QDir::Name);
        const bool result = generateController(
            kind, controllerFixture(kind, "ownership_controller"), targetGenerator, force);
        const QStringList entriesAfter
            = outputDirectory
                  .entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot, QDir::Name);

        QStringList expectedEntries = entriesBefore;
        if (!expectedEntries.contains(cellName)) {
            expectedEntries.append(cellName);
        }
        expectedEntries.append("ownership_controller.typ");
        expectedEntries.sort();
        QCOMPARE(entriesAfter, expectedEntries);

        QFile targetCell(targetCellPath);
        QVERIFY(targetCell.open(QIODevice::ReadOnly));
        const QByteArray actualBytes = targetCell.readAll();
        if (state != "absent" && !force) {
            QCOMPARE(actualBytes, seededBytes);
        } else {
            QCOMPARE(actualBytes, canonicalBytes);
        }
        QVERIFY(result);
    }

    void primitiveCellForcedHardLinkReplacementIsIsolated_data()
    {
        primitiveCellLinkIsRejected_data();
    }

    void primitiveCellForcedHardLinkReplacementIsIsolated()
    {
#ifndef Q_OS_UNIX
        QSKIP("This platform does not provide POSIX hard-link semantics.");
#else
        QFETCH(QString, kind);
        QTemporaryDir canonicalDirectory;
        QTemporaryDir targetDirectory;
        QVERIFY(canonicalDirectory.isValid());
        QVERIFY(targetDirectory.isValid());

        QSocProjectManager canonicalProject;
        canonicalProject.setCurrentPath(
            QDir(canonicalDirectory.path()).filePath("canonical_project"));
        QVERIFY(canonicalProject.mkpath());
        QSocGenerateManager canonicalGenerator(nullptr, &canonicalProject);
        QVERIFY(generateController(
            kind, controllerFixture(kind, "canonical_controller"), canonicalGenerator));

        const QString cellName = kind + "_cell.v";
        QFile         canonicalFile(QDir(canonicalProject.getOutputPath()).filePath(cellName));
        QVERIFY(canonicalFile.open(QIODevice::ReadOnly));
        const QByteArray canonicalBytes = canonicalFile.readAll();
        canonicalFile.close();

        QSocProjectManager targetProject;
        targetProject.setCurrentPath(QDir(targetDirectory.path()).filePath("target_project"));
        QVERIFY(targetProject.mkpath());
        QSocGenerateManager targetGenerator(nullptr, &targetProject);

        const QByteArray sentinelBytes("hard-link target sentinel\n");
        const QString sentinelPath = QDir(targetProject.getCurrentPath()).filePath("cell_target");
        const QString cellPath     = QDir(targetProject.getOutputPath()).filePath(cellName);
        QFile         sentinel(sentinelPath);
        QVERIFY(sentinel.open(QIODevice::WriteOnly));
        QCOMPARE(sentinel.write(sentinelBytes), sentinelBytes.size());
        sentinel.close();
        const QByteArray encodedSentinel = QFile::encodeName(sentinelPath);
        const QByteArray encodedCell     = QFile::encodeName(cellPath);
        QCOMPARE(::link(encodedSentinel.constData(), encodedCell.constData()), 0);

        QVERIFY(generateController(
            kind, controllerFixture(kind, "hard_link_controller"), targetGenerator, true));
        QVERIFY(sentinel.open(QIODevice::ReadOnly));
        QCOMPARE(sentinel.readAll(), sentinelBytes);
        sentinel.close();

        QFile generatedCell(cellPath);
        QVERIFY(generatedCell.open(QIODevice::ReadOnly));
        QCOMPARE(generatedCell.readAll(), canonicalBytes);
        const QStringList entries
            = QDir(targetProject.getOutputPath())
                  .entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot, QDir::Name);
        QStringList expectedEntries{".gitkeep", cellName, "hard_link_controller.typ"};
        expectedEntries.sort();
        QCOMPARE(entries, expectedEntries);
#endif
    }

    void primitiveCellOpenFailurePreservesBytes_data() { primitiveCellLinkIsRejected_data(); }

    void primitiveCellOpenFailurePreservesBytes()
    {
#ifndef Q_OS_UNIX
        QSKIP("This platform does not provide POSIX directory permissions.");
#else
        QFETCH(QString, kind);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        QSocProjectManager project;
        project.setCurrentPath(QDir(directory.path()).filePath("project"));
        QVERIFY(project.mkpath());
        QSocGenerateManager generator(nullptr, &project);
        QVERIFY(generateController(kind, controllerFixture(kind, "seed_controller"), generator));

        const QString cellPath = QDir(project.getOutputPath()).filePath(kind + "_cell.v");
        QFile         cell(cellPath);
        QVERIFY(cell.open(QIODevice::Append));
        const QByteArray sentinelBytes("// open failure sentinel\n");
        QCOMPARE(cell.write(sentinelBytes), sentinelBytes.size());
        cell.close();
        QVERIFY(cell.open(QIODevice::ReadOnly));
        const QByteArray bytesBefore = cell.readAll();
        cell.close();

        const QDir        outputDirectory(project.getOutputPath());
        const QStringList entriesBefore
            = outputDirectory
                  .entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot, QDir::Name);
        const QFileDevice::Permissions originalPermissions = QFile::permissions(
            project.getOutputPath());
        const auto restorePermissions = qScopeGuard(
            [&]() { QFile::setPermissions(project.getOutputPath(), originalPermissions); });
        QVERIFY(
            QFile::setPermissions(
                project.getOutputPath(), QFileDevice::ReadOwner | QFileDevice::ExeOwner));

        const QString probePath = outputDirectory.filePath("permission_probe");
        QFile         probe(probePath);
        if (probe.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
            probe.close();
            QVERIFY(QFile::setPermissions(project.getOutputPath(), originalPermissions));
            QVERIFY(QFile::remove(probePath));
            QSKIP("Directory write permissions are not enforced for this test process.");
        }

        const bool result
            = generateController(kind, controllerFixture(kind, "failed_controller"), generator, true);
        QVERIFY(cell.open(QIODevice::ReadOnly));
        QCOMPARE(cell.readAll(), bytesBefore);
        QCOMPARE(
            outputDirectory
                .entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot, QDir::Name),
            entriesBefore);
        QVERIFY(!result);
#endif
    }

    void primitiveCellEmptyCanonicalHonorsOwnership()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QSocGenerateArtifact::PrimitiveCellSpec spec{"cell.v", {}};
        const QString    cellPath = QDir(directory.path()).filePath("cell.v");
        const QByteArray existingBytes("user-owned cell\n");
        QFile            existing(cellPath);
        QVERIFY(existing.open(QIODevice::WriteOnly));
        QCOMPARE(existing.write(existingBytes), existingBytes.size());
        existing.close();

        const auto preserved
            = QSocGenerateArtifact::ensurePrimitiveCell(directory.path(), spec, false);
        QVERIFY(preserved.success);
        QVERIFY(!preserved.written);
        QVERIFY(existing.open(QIODevice::ReadOnly));
        QCOMPARE(existing.readAll(), existingBytes);
        existing.close();

        QVERIFY(QFile::remove(cellPath));
        const auto missing
            = QSocGenerateArtifact::ensurePrimitiveCell(directory.path(), spec, false);
        QVERIFY(!missing.success);
        QVERIFY(!missing.written);
        QVERIFY(missing.error.contains("content is empty"));
        QVERIFY(!QFileInfo::exists(cellPath));
    }

    void directTypstWriterPreservesRelativePath()
    {
        QFETCH(QString, kind);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString originalPath = QDir::currentPath();
        QVERIFY(QDir::setCurrent(directory.path()));
        const auto restorePath = qScopeGuard([&]() { QDir::setCurrent(originalPath); });

        QVERIFY(QDir().mkdir("nested"));
        const QString relativePath = "nested/" + kind + ".typ";
        QVERIFY(generateDiagram(kind, relativePath));
        QVERIFY(QFileInfo::exists(relativePath));
        QVERIFY(!QFileInfo::exists("nested/nested/" + kind + ".typ"));
    }

    void primaryArtifactsRejectEscapes()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString projectPath = QDir(directory.path()).filePath("project");

        QSocProjectManager manager;
        manager.setCurrentPath(projectPath);
        QVERIFY(manager.mkpath());
        QSocGenerateManager generator(nullptr, &manager);
        const YAML::Node    netlist = YAML::Load(R"(
port:
  source:
    direction: input
    type: logic
  result:
    direction: output
    type: logic
comb:
  - out: result
    expr: source
)");
        QVERIFY(generator.setNetlistData(netlist));
        QVERIFY(generator.processNetlist());

        const QByteArray sentinelBytes("primary artifact sentinel\n");
        const QString    sentinelPath = QDir(projectPath).filePath("escaped.v");
        QFile            sentinel(sentinelPath);
        QVERIFY(sentinel.open(QIODevice::WriteOnly));
        QCOMPARE(sentinel.write(sentinelBytes), sentinelBytes.size());
        sentinel.close();

        QVERIFY(!generator.generateVerilog("../escaped"));
        QVERIFY(sentinel.open(QIODevice::ReadOnly));
        QCOMPARE(sentinel.readAll(), sentinelBytes);
        sentinel.close();
    }

    void relativeProjectOutputIsNotRepeated()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString originalPath = QDir::currentPath();
        QVERIFY(QDir::setCurrent(directory.path()));
        const auto restorePath = qScopeGuard([&]() { QDir::setCurrent(originalPath); });

        QSocProjectManager manager;
        manager.setCurrentPath("relative-project");
        QVERIFY(manager.mkpath());
        QSocGenerateManager generator(nullptr, &manager);
        const YAML::Node    netlist = YAML::Load(R"(
port:
  source:
    direction: input
    type: logic
  result:
    direction: output
    type: logic
comb:
  - out: result
    expr: source
)");
        QVERIFY(generator.setNetlistData(netlist));
        QVERIFY(generator.processNetlist());
        QVERIFY(generator.generateVerilog("stable"));

        const QString expectedPath
            = QDir(directory.path()).filePath("relative-project/output/stable.v");
        const QString repeatedPath = QDir(directory.path())
                                         .filePath(
                                             "relative-project/output/"
                                             "relative-project/output/stable.v");
        QVERIFY(QFileInfo::exists(expectedPath));
        QVERIFY(!QFileInfo::exists(repeatedPath));
    }
};

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocgeneratereproducibility.moc"

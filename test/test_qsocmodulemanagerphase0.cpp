// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocmodulemanager.h"
#include "qsoc_test.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTextStream>
#include <QtTest>

namespace {

void writeTextFile(const QString &filePath, const QString &content)
{
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
    QTextStream stream(&file);
    stream << content;
}

void initProject(QTemporaryDir &tempDir, QSocProjectManager &projectManager)
{
    QVERIFY(tempDir.isValid());
    const QString projectPath = QDir(tempDir.path()).filePath("phase0");
    projectManager.setProjectName("phase0");
    projectManager.setCurrentPath(projectPath);
    QVERIFY(projectManager.mkpath());
    QVERIFY(projectManager.save("phase0"));
    QVERIFY(projectManager.load("phase0"));
}

bool hasProblem(
    const QList<QSocModuleProblem> &problems,
    const QString                  &code,
    QSocModuleProblemSeverity       severity)
{
    for (const QSocModuleProblem &problem : problems) {
        if (problem.code == code && problem.severity == severity) {
            return true;
        }
    }
    return false;
}

bool hasError(const QList<QSocModuleProblem> &problems)
{
    for (const QSocModuleProblem &problem : problems) {
        if (problem.severity == QSocModuleProblemSeverity::Error) {
            return true;
        }
    }
    return false;
}

class Test : public QObject
{
    Q_OBJECT

private slots:
    void definitionRoundTripPreservesFieldsAndExtras();
    void nullModuleRoundTripStaysNullUntilEdited();
    void librarySpecificSaveDoesNotSerializeOverlayWinner();
    void crossLibraryDuplicateIsWarningOnly();
    void validationChecksLoadedBusMappings();
    void scanModuleUsagesFindsNetlistsAndSchematics();
};

void Test::definitionRoundTripPreservesFieldsAndExtras()
{
    QSocModuleManager manager;
    const YAML::Node  yaml = YAML::Load(R"(
kind: imported
port:
  clk:
    type: logic
    direction: in
    visible: true
    group: timing
    role: root
parameter:
  WIDTH:
    type: int
    value: 32
    unit: bits
bus:
  apbs:
    bus: apb4
    mode: slave
    note: local
    mapping:
      paddr: paddr
      pprot: ""
)");

    const QSocModuleDefinition definition = manager.moduleYamlToDefinition("lib", "uart0", yaml);
    QCOMPARE(definition.libraryName, "lib");
    QCOMPARE(definition.moduleName, "uart0");
    QVERIFY(!definition.isNullDefinition);
    QCOMPARE(QString::fromStdString(definition.extraAttributes["kind"].as<std::string>()), "imported");
    QCOMPARE(definition.ports.size(), 1);
    QCOMPARE(definition.ports.first().name, "clk");
    QCOMPARE(definition.ports.first().direction, "in");
    QCOMPARE(definition.ports.first().type, "logic");
    QVERIFY(definition.ports.first().hasVisible);
    QVERIFY(definition.ports.first().visible);
    QCOMPARE(definition.ports.first().group, "timing");
    QCOMPARE(
        QString::fromStdString(definition.ports.first().extraAttributes["role"].as<std::string>()),
        "root");
    QCOMPARE(definition.parameters.size(), 1);
    QCOMPARE(definition.parameters.first().name, "WIDTH");
    QCOMPARE(definition.parameters.first().value, "32");
    QCOMPARE(
        QString::fromStdString(
            definition.parameters.first().extraAttributes["unit"].as<std::string>()),
        "bits");
    QCOMPARE(definition.busInterfaces.size(), 1);
    QCOMPARE(definition.busInterfaces.first().name, "apbs");
    QCOMPARE(definition.busInterfaces.first().busName, "apb4");
    QCOMPARE(definition.busInterfaces.first().mode, "slave");
    QCOMPARE(definition.busInterfaces.first().mapping.size(), 2);
    QCOMPARE(definition.busInterfaces.first().mapping.last().busSignal, "pprot");
    QCOMPARE(definition.busInterfaces.first().mapping.last().modulePort, "");

    const YAML::Node saved = manager.moduleDefinitionToYaml(definition);
    QCOMPARE(QString::fromStdString(saved["kind"].as<std::string>()), "imported");
    QCOMPARE(QString::fromStdString(saved["port"]["clk"]["role"].as<std::string>()), "root");
    QVERIFY(saved["port"]["clk"]["visible"].as<bool>());
    QCOMPARE(QString::fromStdString(saved["parameter"]["WIDTH"]["unit"].as<std::string>()), "bits");
    QCOMPARE(QString::fromStdString(saved["bus"]["apbs"]["note"].as<std::string>()), "local");
    QCOMPARE(QString::fromStdString(saved["bus"]["apbs"]["mapping"]["pprot"].as<std::string>()), "");
}

void Test::nullModuleRoundTripStaysNullUntilEdited()
{
    QTemporaryDir      tempDir;
    QSocProjectManager projectManager;
    initProject(tempDir, projectManager);

    const QString moduleDir = projectManager.getModulePath();
    writeTextFile(QDir(moduleDir).filePath("lib.soc_mod"), "EMPTY: ~\n");

    QSocModuleManager manager(nullptr, &projectManager);
    QVERIFY(manager.load("lib"));

    QSocModuleDefinition definition = manager.getModuleDefinition("lib", "EMPTY");
    QVERIFY(definition.isNullDefinition);
    const YAML::Node nullYaml = manager.moduleDefinitionToYaml(definition);
    QVERIFY(!nullYaml || nullYaml.IsNull());

    QVERIFY(manager.replaceModuleDefinition(definition));
    YAML::Node savedLibrary = YAML::LoadFile(QDir(moduleDir).filePath("lib.soc_mod").toStdString());
    QVERIFY(savedLibrary["EMPTY"].IsNull());

    QSocModulePort port;
    port.name      = "clk";
    port.type      = "logic";
    port.direction = "in";
    definition.ports.append(port);
    QVERIFY(manager.replaceModuleDefinition(definition));

    savedLibrary = YAML::LoadFile(QDir(moduleDir).filePath("lib.soc_mod").toStdString());
    QVERIFY(savedLibrary["EMPTY"].IsMap());
    QCOMPARE(
        QString::fromStdString(savedLibrary["EMPTY"]["port"]["clk"]["direction"].as<std::string>()),
        "in");
}

void Test::librarySpecificSaveDoesNotSerializeOverlayWinner()
{
    QTemporaryDir      tempDir;
    QSocProjectManager projectManager;
    initProject(tempDir, projectManager);

    const QString moduleDir = projectManager.getModulePath();
    writeTextFile(
        QDir(moduleDir).filePath("a.soc_mod"),
        R"(shared:
  port:
    a_in:
      type: logic
      direction: in
only_a: ~
)");
    writeTextFile(
        QDir(moduleDir).filePath("b.soc_mod"),
        R"(shared:
  port:
    b_out:
      type: logic
      direction: out
)");

    QSocModuleManager manager(nullptr, &projectManager);
    QVERIFY(manager.load(QRegularExpression(".*")));

    YAML::Node activeShared = manager.getModuleYaml(QStringLiteral("shared"));
    QCOMPARE(QString::fromStdString(activeShared["library"].as<std::string>()), "b");
    QVERIFY(activeShared["port"]["b_out"]);

    QSocModuleDefinition shadowed = manager.getModuleDefinition("a", "shared");
    QCOMPARE(shadowed.ports.size(), 1);
    QCOMPARE(shadowed.ports.first().name, "a_in");
    shadowed.ports.first().description = "edited shadow";
    QVERIFY(manager.replaceModuleDefinition(shadowed));

    const YAML::Node aYaml = YAML::LoadFile(QDir(moduleDir).filePath("a.soc_mod").toStdString());
    const YAML::Node bYaml = YAML::LoadFile(QDir(moduleDir).filePath("b.soc_mod").toStdString());
    QVERIFY(aYaml["shared"]["port"]["a_in"]);
    QCOMPARE(
        QString::fromStdString(aYaml["shared"]["port"]["a_in"]["description"].as<std::string>()),
        "edited shadow");
    QVERIFY(!aYaml["shared"]["port"]["b_out"]);
    QVERIFY(bYaml["shared"]["port"]["b_out"]);

    activeShared = manager.getModuleYaml(QStringLiteral("shared"));
    QCOMPARE(QString::fromStdString(activeShared["library"].as<std::string>()), "b");
    QVERIFY(activeShared["port"]["b_out"]);

    QSocModuleDefinition nullDefinition = manager.getModuleDefinition("a", "only_a");
    QVERIFY(nullDefinition.isNullDefinition);
    QVERIFY(manager.replaceModuleDefinition(nullDefinition));
    const YAML::Node savedA = YAML::LoadFile(QDir(moduleDir).filePath("a.soc_mod").toStdString());
    QVERIFY(savedA["only_a"].IsNull());
}

void Test::crossLibraryDuplicateIsWarningOnly()
{
    QTemporaryDir      tempDir;
    QSocProjectManager projectManager;
    initProject(tempDir, projectManager);

    const QString moduleDir = projectManager.getModulePath();
    writeTextFile(
        QDir(moduleDir).filePath("a.soc_mod"),
        R"(shared:
  port:
    a_in:
      type: logic
      direction: in
)");
    writeTextFile(
        QDir(moduleDir).filePath("b.soc_mod"),
        R"(shared:
  port:
    b_out:
      type: logic
      direction: out
)");

    QSocModuleManager manager(nullptr, &projectManager);
    QVERIFY(manager.load(QRegularExpression(".*")));

    const QList<QSocModuleOverlay> overlays = manager.scanModuleOverlays("shared");
    QCOMPARE(overlays.size(), 1);
    QCOMPARE(overlays.first().activeLibrary, "b");
    QCOMPARE(overlays.first().shadowedLibraries, QStringList({"a"}));

    const QSocModuleDefinition     definition = manager.getModuleDefinition("a", "shared");
    const QList<QSocModuleProblem> problems   = manager.validateModuleDefinition(definition);
    QVERIFY(hasProblem(problems, "module-overlay", QSocModuleProblemSeverity::Warning));
    QVERIFY(!hasError(problems));
}

void Test::validationChecksLoadedBusMappings()
{
    QTemporaryDir      tempDir;
    QSocProjectManager projectManager;
    initProject(tempDir, projectManager);

    writeTextFile(
        QDir(projectManager.getBusPath()).filePath("ctrl.soc_bus"),
        R"(ctrl:
  port:
    ready:
      master:
        direction: in
    data:
      master:
        direction: in
)");
    writeTextFile(
        QDir(projectManager.getModulePath()).filePath("uart.soc_mod"),
        R"(uart:
  port:
    uart_ready:
      type: logic
      direction: in
  bus:
    ctrl:
      bus: ctrl
      mode: master
      mapping:
        ready: missing_port
        old: uart_ready
        data:
)");

    QSocBusManager    busManager(nullptr, &projectManager);
    QSocModuleManager moduleManager(nullptr, &projectManager, &busManager);
    QVERIFY(busManager.load("ctrl"));
    QVERIFY(moduleManager.load("uart"));

    const QSocModuleDefinition     definition = moduleManager.getModuleDefinition("uart", "uart");
    const QList<QSocModuleProblem> problems   = moduleManager.validateModuleDefinition(definition);
    QVERIFY(hasProblem(problems, "unknown-mapping-port", QSocModuleProblemSeverity::Error));
    QVERIFY(hasProblem(problems, "unknown-mapping-signal", QSocModuleProblemSeverity::Error));
    QVERIFY(hasProblem(problems, "empty-mapping-target", QSocModuleProblemSeverity::Warning));
}

void Test::scanModuleUsagesFindsNetlistsAndSchematics()
{
    QTemporaryDir      tempDir;
    QSocProjectManager projectManager;
    initProject(tempDir, projectManager);

    writeTextFile(
        QDir(projectManager.getOutputPath()).filePath("top.soc_net"),
        R"(instance:
  u_uart_net:
    module: uart
    port:
      clk: clk
    bus:
      ctrl: ctrl_bus
  u_gpio_net:
    module: gpio
)");
    writeTextFile(
        QDir(projectManager.getSchematicPath()).filePath("top.soc_sch"),
        R"(scene:
  items:
    - module_name: uart
      instance_name: u_uart_sch
      module_yaml: |
        port:
          clk:
            direction: in
        bus:
          ctrl:
            bus: ctrl
)");

    QSocModuleManager            manager(nullptr, &projectManager);
    QStringList                  scanErrors;
    const QList<QSocModuleUsage> usages = manager.scanModuleUsages("uart", &scanErrors);
    QVERIFY(scanErrors.isEmpty());
    QCOMPARE(usages.size(), 2);
    QCOMPARE(usages.at(0).sourceType, QString("netlist"));
    QCOMPARE(usages.at(0).filePath, QString("output/top.soc_net"));
    QCOMPARE(usages.at(0).instanceName, QString("u_uart_net"));
    QCOMPARE(usages.at(0).portNames, QStringList({"clk"}));
    QCOMPARE(usages.at(0).busInterfaces, QStringList({"ctrl"}));
    QCOMPARE(usages.at(1).sourceType, QString("schematic"));
    QCOMPARE(usages.at(1).filePath, QString("schematic/top.soc_sch"));
    QCOMPARE(usages.at(1).instanceName, QString("u_uart_sch"));
    QCOMPARE(usages.at(1).portNames, QStringList({"clk"}));
    QCOMPARE(usages.at(1).busInterfaces, QStringList({"ctrl"}));
}

} // namespace

QSOC_TEST_MAIN(Test)

#include "test_qsocmodulemanagerphase0.moc"

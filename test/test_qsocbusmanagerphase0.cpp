// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocbusmanager.h"
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

QSocBusSignalMode makeRow(
    const QString &signal,
    const QString &mode,
    const QString &direction   = {},
    const QString &width       = {},
    const QString &qualifier   = {},
    const QString &description = {})
{
    QSocBusSignalMode row;
    row.signal      = signal;
    row.mode        = mode;
    row.direction   = direction;
    row.width       = width;
    row.qualifier   = qualifier;
    row.description = description;
    return row;
}

bool hasProblemCode(const QList<QSocBusProblem> &problems, const QString &code)
{
    for (const QSocBusProblem &problem : problems) {
        if (problem.code == code) {
            return true;
        }
    }
    return false;
}

bool hasWarningProblemCode(const QList<QSocBusProblem> &problems, const QString &code)
{
    for (const QSocBusProblem &problem : problems) {
        if (problem.code == code && problem.severity == QSocBusProblemSeverity::Warning) {
            return true;
        }
    }
    return false;
}

class Test : public QObject
{
    Q_OBJECT

private slots:
    void yamlRoundTripPreservesRowsAndExtras();
    void pendingEmptyLibraryDoesNotWriteFile();
    void replacePreservesOtherBusAndDropsStaleRows();
    void csvParserPreservesDescriptionAndModes();
    void validationReportsRowProblems();
    void usageScannerAndReferenceValidation();
    void duplicateBusNameLoadFails();
    void removeLibraryIfEmptyDoesNotDeleteUnloadedFile();
    void referenceRenameUpdatesModuleYaml();
};

void Test::yamlRoundTripPreservesRowsAndExtras()
{
    QSocBusManager   manager;
    const YAML::Node yaml = YAML::Load(R"(
kind: protocol
port:
  clk:
    group: timing
    master:
      direction: out
      width: 1
      description: source clock
      role: source
    slave:
      direction: in
      width: 1
      role: sink
)");

    const QSocBusDefinition definition = manager.busYamlToDefinition("lib", "clockbus", yaml);
    QCOMPARE(definition.rows.size(), 2);
    QCOMPARE(QString::fromStdString(definition.extraAttributes["kind"].as<std::string>()), "protocol");

    const QSocBusSignalMode masterRow = definition.rows.first();
    QCOMPARE(masterRow.signal, "clk");
    QCOMPARE(masterRow.mode, "master");
    QCOMPARE(masterRow.direction, "out");
    QCOMPARE(masterRow.width, "1");
    QCOMPARE(masterRow.description, "source clock");
    QCOMPARE(
        QString::fromStdString(masterRow.signalExtraAttributes["group"].as<std::string>()),
        "timing");
    QCOMPARE(
        QString::fromStdString(masterRow.modeExtraAttributes["role"].as<std::string>()), "source");

    const YAML::Node saved = manager.busDefinitionToYaml(definition);
    QCOMPARE(QString::fromStdString(saved["kind"].as<std::string>()), "protocol");
    QCOMPARE(QString::fromStdString(saved["port"]["clk"]["group"].as<std::string>()), "timing");
    QCOMPARE(
        QString::fromStdString(saved["port"]["clk"]["master"]["role"].as<std::string>()), "source");
    QCOMPARE(
        QString::fromStdString(saved["port"]["clk"]["master"]["description"].as<std::string>()),
        "source clock");
}

void Test::pendingEmptyLibraryDoesNotWriteFile()
{
    QTemporaryDir      tempDir;
    QSocProjectManager projectManager;
    initProject(tempDir, projectManager);

    QSocBusManager manager(nullptr, &projectManager);
    QVERIFY(manager.createLibrary("scratch"));
    QVERIFY(manager.listLoadedLibraries().contains("scratch"));
    QVERIFY(!QFile::exists(QDir(projectManager.getBusPath()).filePath("scratch.soc_bus")));
}

void Test::replacePreservesOtherBusAndDropsStaleRows()
{
    QTemporaryDir      tempDir;
    QSocProjectManager projectManager;
    initProject(tempDir, projectManager);

    writeTextFile(QDir(projectManager.getBusPath()).filePath("lib.soc_bus"), R"(
apb4:
  port:
    paddr:
      master:
        direction: out
    pwrite:
      master:
        direction: out
axi4:
  port:
    awvalid:
      master:
        direction: out
)");

    QSocBusManager manager(nullptr, &projectManager);
    QVERIFY(manager.load("lib"));

    QSocBusDefinition definition;
    definition.libraryName = "lib";
    definition.busName     = "apb4";
    definition.rows.append(makeRow("paddr", "master", "out"));
    QVERIFY(manager.replaceBusDefinition(definition));

    const YAML::Node saved = YAML::LoadFile(
        QDir(projectManager.getBusPath()).filePath("lib.soc_bus").toStdString());
    QVERIFY(saved["axi4"]);
    QVERIFY(saved["apb4"]["port"]["paddr"]["master"]);
    QVERIFY(!saved["apb4"]["port"]["pwrite"]);
}

void Test::csvParserPreservesDescriptionAndModes()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString csvPath = QDir(tempDir.path()).filePath("bus.csv");
    writeTextFile(
        csvPath,
        "Name,Mode,Direction,Width,Qualifier,Description\n"
        "ready,system,in,1,flag,Ready line\n");

    QSocBusManager                 manager;
    QStringList                    warnings;
    const QList<QSocBusSignalMode> rows = manager.parseBusCsvFiles({csvPath}, &warnings);
    QVERIFY(warnings.isEmpty());
    QCOMPARE(rows.size(), 1);
    QCOMPARE(rows.first().mode, "system");
    QCOMPARE(rows.first().description, "Ready line");

    const YAML::Node yaml = manager.rowsToBusYaml("ctrl", rows);
    QCOMPARE(
        QString::fromStdString(
            yaml["ctrl"]["port"]["ready"]["system"]["description"].as<std::string>()),
        "Ready line");
}

void Test::validationReportsRowProblems()
{
    QSocBusDefinition definition;
    definition.libraryName = "lib";
    definition.busName     = "apb4";
    definition.rows.append(makeRow("paddr", "master", "sideways"));
    definition.rows.append(makeRow("paddr", "master", "out", "ADDR_WIDTH"));
    definition.rows.append(makeRow("", "slave"));
    definition.rows.append(makeRow("pwrite", ""));
    QSocBusSignalMode conflictingRow               = makeRow("pready", "master", "in");
    conflictingRow.signalExtraAttributes["master"] = "legacy";
    definition.rows.append(conflictingRow);

    QSocBusManager              manager;
    const QList<QSocBusProblem> problems = manager.validateBusDefinition(definition);
    QVERIFY(hasProblemCode(problems, "duplicate-signal-mode"));
    QVERIFY(hasProblemCode(problems, "invalid-direction"));
    QVERIFY(hasProblemCode(problems, "non-integer-width"));
    QVERIFY(hasProblemCode(problems, "missing-signal"));
    QVERIFY(hasProblemCode(problems, "missing-mode"));
    QVERIFY(hasProblemCode(problems, "signal-attribute-mode-conflict"));
}

void Test::usageScannerAndReferenceValidation()
{
    QTemporaryDir      tempDir;
    QSocProjectManager projectManager;
    initProject(tempDir, projectManager);

    writeTextFile(QDir(projectManager.getModulePath()).filePath("periph.soc_mod"), R"(
uart:
  bus:
    reg:
      bus: apb4
      mode: slave
      mapping:
        paddr: addr
        missing: none
        empty: ''
)");

    QSocBusManager            manager(nullptr, &projectManager);
    QStringList               scanErrors;
    const QList<QSocBusUsage> usages = manager.scanBusUsages("apb4", &scanErrors);
    QVERIFY(scanErrors.isEmpty());
    QCOMPARE(usages.size(), 1);
    QCOMPARE(usages.first().moduleLibrary, "periph");
    QCOMPARE(usages.first().moduleName, "uart");
    QCOMPARE(usages.first().interfaceName, "reg");
    QCOMPARE(usages.first().mode, "slave");
    QVERIFY(usages.first().mappingSignals.contains("paddr"));
    QVERIFY(usages.first().emptyMappingSignals.contains("empty"));

    QSocBusDefinition definition;
    definition.libraryName = "lib";
    definition.busName     = "apb4";
    definition.rows.append(makeRow("paddr", "master", "out"));

    const QList<QSocBusProblem> problems = manager.validateBusReferences(definition, &scanErrors);
    QVERIFY(hasProblemCode(problems, "missing-referenced-mode"));
    QVERIFY(hasProblemCode(problems, "missing-mapping-signal"));
    QVERIFY(hasWarningProblemCode(problems, "empty-mapping-value"));
}

void Test::duplicateBusNameLoadFails()
{
    QTemporaryDir      tempDir;
    QSocProjectManager projectManager;
    initProject(tempDir, projectManager);

    writeTextFile(QDir(projectManager.getBusPath()).filePath("liba.soc_bus"), R"(
apb4:
  port:
    paddr:
      master:
        direction: out
)");
    writeTextFile(QDir(projectManager.getBusPath()).filePath("libb.soc_bus"), R"(
apb4:
  port:
    paddr:
      slave:
        direction: in
)");

    QSocBusManager manager(nullptr, &projectManager);
    QVERIFY(manager.load("liba"));
    QVERIFY(!manager.load("libb"));
}

void Test::removeLibraryIfEmptyDoesNotDeleteUnloadedFile()
{
    QTemporaryDir      tempDir;
    QSocProjectManager projectManager;
    initProject(tempDir, projectManager);

    const QString libraryPath = QDir(projectManager.getBusPath()).filePath("lib.soc_bus");
    writeTextFile(libraryPath, R"(
apb4:
  port:
    paddr:
      master:
        direction: out
)");

    QSocBusManager manager(nullptr, &projectManager);
    QVERIFY(!manager.removeLibraryIfEmpty("lib"));
    QVERIFY(QFile::exists(libraryPath));
    QVERIFY(manager.load("lib"));
    QVERIFY(manager.removeBusFromLibrary("lib", "apb4"));
    QVERIFY(!QFile::exists(libraryPath));
}

void Test::referenceRenameUpdatesModuleYaml()
{
    QTemporaryDir      tempDir;
    QSocProjectManager projectManager;
    initProject(tempDir, projectManager);

    const QString modulePath = QDir(projectManager.getModulePath()).filePath("periph.soc_mod");
    writeTextFile(modulePath, R"(
uart:
  bus:
    reg:
      bus: apb4
      mode: slave
      mapping:
        paddr: addr_i
        pwrite: write_i
    trace:
      bus: debug
      mode: slave
      mapping:
        paddr: trace_addr
)");

    QSocBusManager manager(nullptr, &projectManager);
    QStringList    changedModules;
    QStringList    errors;
    QVERIFY(manager.renameBusReferences("apb4", "apb5", &changedModules, &errors));
    QVERIFY(errors.isEmpty());
    QCOMPARE(changedModules, QStringList({"periph/uart/reg"}));

    YAML::Node saved = YAML::LoadFile(modulePath.toStdString());
    QCOMPARE(QString::fromStdString(saved["uart"]["bus"]["reg"]["bus"].as<std::string>()), "apb5");
    QCOMPARE(QString::fromStdString(saved["uart"]["bus"]["trace"]["bus"].as<std::string>()), "debug");

    changedModules.clear();
    QVERIFY(manager.renameSignalReferences("apb5", "paddr", "addr_sig", &changedModules, &errors));
    QVERIFY(errors.isEmpty());
    QCOMPARE(changedModules, QStringList({"periph/uart/reg"}));

    saved = YAML::LoadFile(modulePath.toStdString());
    QVERIFY(saved["uart"]["bus"]["reg"]["mapping"]["addr_sig"]);
    QVERIFY(!saved["uart"]["bus"]["reg"]["mapping"]["paddr"]);
    QVERIFY(saved["uart"]["bus"]["trace"]["mapping"]["paddr"]);

    changedModules.clear();
    QVERIFY(manager.renameModeReferences("apb5", "slave", "target", &changedModules, &errors));
    QVERIFY(errors.isEmpty());
    QCOMPARE(changedModules, QStringList({"periph/uart/reg"}));

    saved = YAML::LoadFile(modulePath.toStdString());
    QCOMPARE(QString::fromStdString(saved["uart"]["bus"]["reg"]["mode"].as<std::string>()), "target");
    QCOMPARE(QString::fromStdString(saved["uart"]["bus"]["trace"]["mode"].as<std::string>()), "slave");

    errors.clear();
    QVERIFY(!manager.renameSignalReferences("apb5", "pwrite", "addr_sig", nullptr, &errors));
    QVERIFY(!errors.isEmpty());

    saved = YAML::LoadFile(modulePath.toStdString());
    QVERIFY(saved["uart"]["bus"]["reg"]["mapping"]["pwrite"]);
    QVERIFY(saved["uart"]["bus"]["reg"]["mapping"]["addr_sig"]);
}

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocbusmanagerphase0.moc"

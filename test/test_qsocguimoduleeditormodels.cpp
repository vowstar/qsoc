// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "gui/moduleeditorwindow/modulebusinterfacemodel.h"
#include "gui/moduleeditorwindow/modulebusmappingmodel.h"
#include "gui/moduleeditorwindow/modulelibrarymodel.h"
#include "gui/moduleeditorwindow/moduleparametermodel.h"
#include "gui/moduleeditorwindow/moduleportmodel.h"
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
    const QString projectPath = QDir(tempDir.path()).filePath("module_gui_models");
    projectManager.setProjectName("module_gui_models");
    projectManager.setCurrentPath(projectPath);
    QVERIFY(projectManager.mkpath());
    QVERIFY(projectManager.save("module_gui_models"));
    QVERIFY(projectManager.load("module_gui_models"));
}

class Test : public QObject
{
    Q_OBJECT

private slots:
    void libraryModelShowsModulesAndOverlays();
    void portAndParameterModelsEditRows();
    void busInterfaceModelEditsInterfaceRows();
    void busMappingModelRebuildsAndAutoMatchesRows();
};

void Test::libraryModelShowsModulesAndOverlays()
{
    QTemporaryDir      tempDir;
    QSocProjectManager projectManager;
    initProject(tempDir, projectManager);

    writeTextFile(QDir(projectManager.getModulePath()).filePath("a.soc_mod"), R"(
shared:
  port:
    clk:
      direction: in
solo: ~
)");
    writeTextFile(QDir(projectManager.getModulePath()).filePath("b.soc_mod"), R"(
shared:
  bus:
    ctrl:
      bus: reg
      mode: master
)");

    QSocModuleManager manager(nullptr, &projectManager);
    for (const QString &libraryName : manager.listLibrary())
        QVERIFY(manager.load(libraryName));

    ModuleLibraryModel model;
    model.setModuleManager(&manager);

    const QModelIndex sharedA = model.indexForModule(QStringLiteral("a"), QStringLiteral("shared"));
    const QModelIndex sharedB = model.indexForModule(QStringLiteral("b"), QStringLiteral("shared"));
    const QModelIndex solo    = model.indexForModule(QStringLiteral("a"), QStringLiteral("solo"));
    QVERIFY(sharedA.isValid());
    QVERIFY(sharedB.isValid());
    QVERIFY(solo.isValid());
    QCOMPARE(ModuleLibraryModel::status(sharedA), QString("Shadowed"));
    QCOMPARE(ModuleLibraryModel::status(sharedB), QString("Active overlay"));
    QCOMPARE(ModuleLibraryModel::status(solo), QString("Null"));

    QVERIFY(model.setLibraryEnabled(QStringLiteral("a"), false));
    QVERIFY(!ModuleLibraryModel::isEnabled(sharedA));
}

void Test::portAndParameterModelsEditRows()
{
    ModulePortModel portModel;
    QSocModulePort  clk;
    clk.name      = QStringLiteral("clk");
    clk.direction = QStringLiteral("in");
    clk.type      = QStringLiteral("logic");
    portModel.setPorts({clk});

    QCOMPARE(portModel.rowCount(), 1);
    QVERIFY(portModel.setData(
        portModel.index(0, ModulePortModel::VisibleColumn), Qt::Checked, Qt::CheckStateRole));
    QVERIFY(portModel.ports().first().hasVisible);
    QVERIFY(portModel.ports().first().visible);
    QVERIFY(portModel.isDirty());

    portModel.duplicateRows({0});
    QCOMPARE(portModel.rowCount(), 2);
    QCOMPARE(
        portModel.index(1, ModulePortModel::ProblemColumn).data().toString(),
        QString("Duplicate name"));
    portModel.removeRowIndices({1});
    QCOMPARE(portModel.rowCount(), 1);

    ModuleParameterModel parameterModel;
    QSocModuleParameter  width;
    width.name  = QStringLiteral("WIDTH");
    width.type  = QStringLiteral("int");
    width.value = QStringLiteral("32");
    parameterModel.setParameters({width});
    QVERIFY(parameterModel.setData(
        parameterModel.index(0, ModuleParameterModel::ValueColumn), QStringLiteral("64")));
    QCOMPARE(parameterModel.parameters().first().value, QString("64"));
    QVERIFY(parameterModel.isDirty());
}

void Test::busInterfaceModelEditsInterfaceRows()
{
    ModuleBusInterfaceModel model;
    model.addInterface(QStringLiteral("m_axi"), QStringLiteral("axi4"));
    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(model.interfaceAt(0).mode, QString("master"));
    QVERIFY(
        model.setData(model.index(0, ModuleBusInterfaceModel::ModeColumn), QStringLiteral("slave")));
    QCOMPARE(model.interfaceAt(0).mode, QString("slave"));
    QCOMPARE(model.index(0, ModuleBusInterfaceModel::StatusColumn).data().toString(), QString("OK"));

    QSocModuleBusInterface interface = model.interfaceAt(0);
    interface.mapping.append({QStringLiteral("araddr"), QString()});
    QVERIFY(model.setInterfaceAt(0, interface));
    QCOMPARE(model.index(0, ModuleBusInterfaceModel::EmptyCountColumn).data().toInt(), 1);
    QCOMPARE(
        model.index(0, ModuleBusInterfaceModel::StatusColumn).data().toString(),
        QString("Incomplete mapping"));
}

void Test::busMappingModelRebuildsAndAutoMatchesRows()
{
    QSocBusDefinition bus;
    bus.busName = QStringLiteral("axi4");
    bus.rows.append(
        {QStringLiteral("araddr"),
         QStringLiteral("master"),
         QStringLiteral("out"),
         QStringLiteral("32"),
         {},
         {}});
    bus.rows.append(
        {QStringLiteral("arready"),
         QStringLiteral("master"),
         QStringLiteral("in"),
         QStringLiteral("1"),
         {},
         {}});

    QSocModulePort araddr;
    araddr.name      = QStringLiteral("m_axi_araddr");
    araddr.direction = QStringLiteral("out");
    araddr.type      = QStringLiteral("logic[31:0]");
    QSocModulePort arready;
    arready.name      = QStringLiteral("m_axi_arready");
    arready.direction = QStringLiteral("in");
    arready.type      = QStringLiteral("logic");

    QSocModuleBusInterface interface;
    interface.name    = QStringLiteral("m_axi");
    interface.busName = QStringLiteral("axi4");
    interface.mode    = QStringLiteral("master");
    interface.mapping.append({QStringLiteral("oldsig"), QStringLiteral("old_port")});

    ModuleBusMappingModel model;
    model.setContext(interface, bus, {araddr, arready});
    model.rebuildRowsFromBusDefinition();
    QCOMPARE(model.rowCount(), 3);
    QCOMPARE(
        model.index(0, ModuleBusMappingModel::BusSignalColumn).data().toString(), QString("araddr"));
    QCOMPARE(
        model.index(1, ModuleBusMappingModel::BusSignalColumn).data().toString(),
        QString("arready"));
    QCOMPARE(
        model.index(2, ModuleBusMappingModel::ProblemColumn).data().toString(),
        QString("Unknown bus signal"));

    model.autoMatchByInterfacePrefix();
    QSocModuleBusInterface edited = model.interfaceDefinition();
    QCOMPARE(edited.mapping.at(0).modulePort, QString("m_axi_araddr"));
    QCOMPARE(edited.mapping.at(1).modulePort, QString("m_axi_arready"));
    QCOMPARE(model.index(0, ModuleBusMappingModel::ProblemColumn).data().toString(), QString());
    QCOMPARE(model.index(1, ModuleBusMappingModel::ProblemColumn).data().toString(), QString());
}

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocguimoduleeditormodels.moc"

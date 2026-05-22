// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "gui/buseditorwindow/buseditorwindow.h"
#include "gui/mainwindow/mainwindow.h"
#include "gui/moduleeditorwindow/moduleeditorwindow.h"
#include "gui/schematicwindow/schematicmodule.h"

#include <QAction>
#include <QDir>
#include <QFile>
#include <QGraphicsItem>
#include <QGraphicsView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QPlainTextEdit>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QTabWidget>
#include <QTableView>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTreeView>
#include <QtTest>

namespace {

void writeTextFile(const QString &filePath, const QString &content)
{
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
    QTextStream stream(&file);
    stream << content;
}

QString readTextFile(const QString &filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(file.readAll());
}

void initProject(QTemporaryDir &tempDir, QSocProjectManager &projectManager)
{
    QVERIFY(tempDir.isValid());
    const QString projectPath = QDir(tempDir.path()).filePath("module_gui");
    projectManager.setProjectName("module_gui");
    projectManager.setCurrentPath(projectPath);
    QVERIFY(projectManager.mkpath());
    QVERIFY(projectManager.save("module_gui"));
    QVERIFY(projectManager.load("module_gui"));
}

class Test : public QObject
{
    Q_OBJECT

private slots:
    void opensWithoutProject();
    void loadsProjectModuleLibrary();
    void createsDuplicatesRenamesAndDeletesModules();
    void importsVerilogFiles();
    void showsModuleUsages();
    void rendersSymbolPreviewFromSchematicModule();
    void busModeEditsRebuildMappingRows();
    void savesRenamedBusInterface();
    void rowActionsEditCurrentTable();
    void mappingToolsManageMissingRowsAndFilters();
    void openBusEditorActionShowsBusEditor();
    void savesEditedMapping();
    void mainWindowDoubleClickOpensModuleEditor();
};

void Test::opensWithoutProject()
{
    ModuleEditorWindow window;

    QCOMPARE(window.portModelForTest()->rowCount(), 0);
    QCOMPARE(window.libraryModelForTest()->rowCount(), 1);

    auto *summary = window.findChild<QLabel *>(QStringLiteral("moduleSummaryLabel"));
    QVERIFY(summary);
    QCOMPARE(summary->text(), QString("No module selected."));
}

void Test::loadsProjectModuleLibrary()
{
    QTemporaryDir      tempDir;
    QSocProjectManager projectManager;
    initProject(tempDir, projectManager);

    const QString busFile = QDir(projectManager.getBusPath()).filePath("ctrl.soc_bus");
    writeTextFile(busFile, R"(
ctrl:
  port:
    ready:
      master:
        direction: in
        width: 1
)");
    const QString moduleFile = QDir(projectManager.getModulePath()).filePath("uart.soc_mod");
    writeTextFile(moduleFile, R"(
uart:
  port:
    clk:
      direction: in
      type: logic
    uart_ready:
      direction: in
      type: logic
  bus:
    ctrl:
      bus: ctrl
      mode: master
      mapping:
        ready: uart_ready
)");

    ModuleEditorWindow window(nullptr, &projectManager);
    window.openFile(moduleFile);

    QCOMPARE(window.portModelForTest()->rowCount(), 2);
    QCOMPARE(window.busInterfaceModelForTest()->rowCount(), 1);
    QCOMPARE(window.busMappingModelForTest()->rowCount(), 1);
    QCOMPARE(
        window.busMappingModelForTest()
            ->index(0, ModuleBusMappingModel::ModulePortColumn)
            .data()
            .toString(),
        QString("uart_ready"));

    auto *preview = window.findChild<QPlainTextEdit *>(QStringLiteral("moduleYamlPreview"));
    QVERIFY(preview);
    QVERIFY(preview->toPlainText().contains(QStringLiteral("mapping")));
}

void Test::createsDuplicatesRenamesAndDeletesModules()
{
    QTemporaryDir      tempDir;
    QSocProjectManager projectManager;
    initProject(tempDir, projectManager);

    ModuleEditorWindow window(nullptr, &projectManager);
    QVERIFY(window.findChild<QAction *>(QStringLiteral("moduleNewLibraryAction")));
    QVERIFY(window.findChild<QAction *>(QStringLiteral("moduleNewModuleAction")));
    QVERIFY(window.findChild<QAction *>(QStringLiteral("moduleDuplicateModuleAction")));
    QVERIFY(window.findChild<QAction *>(QStringLiteral("moduleRenameModuleAction")));
    QVERIFY(window.findChild<QAction *>(QStringLiteral("moduleDeleteModuleAction")));
    QVERIFY(window.findChild<QAction *>(QStringLiteral("moduleDeleteLibraryAction")));

    QVERIFY(window.createLibrary(QStringLiteral("scratch")));
    QVERIFY(!window.createLibrary(QStringLiteral("scratch")));
    QVERIFY(window.createModule(QStringLiteral("scratch"), QStringLiteral("uart")));
    QVERIFY(QMetaObject::invokeMethod(&window, "handleSave", Qt::DirectConnection));

    const QString moduleFile = QDir(projectManager.getModulePath()).filePath("scratch.soc_mod");
    QVERIFY(readTextFile(moduleFile).contains(QStringLiteral("uart")));

    QVERIFY(window.duplicateCurrentModule(QStringLiteral("uart_copy")));
    QString saved = readTextFile(moduleFile);
    QVERIFY(saved.contains(QStringLiteral("uart:")));
    QVERIFY(saved.contains(QStringLiteral("uart_copy:")));

    QVERIFY(window.renameCurrentModule(QStringLiteral("uart2")));
    saved = readTextFile(moduleFile);
    QVERIFY(saved.contains(QStringLiteral("uart:")));
    QVERIFY(saved.contains(QStringLiteral("uart2:")));
    QVERIFY(!saved.contains(QStringLiteral("uart_copy:")));

    QVERIFY(window.deleteCurrentModule());
    saved = readTextFile(moduleFile);
    QVERIFY(saved.contains(QStringLiteral("uart:")));
    QVERIFY(!saved.contains(QStringLiteral("uart2:")));

    QVERIFY(window.createLibrary(QStringLiteral("empty")));
    QVERIFY(window.deleteLibrary(QStringLiteral("empty")));
}

void Test::importsVerilogFiles()
{
    QTemporaryDir      tempDir;
    QSocProjectManager projectManager;
    initProject(tempDir, projectManager);

    const QString verilogFile = QDir(tempDir.path()).filePath("uart_import.v");
    writeTextFile(verilogFile, R"(
module uart_import (
    input  logic clk,
    output logic tx
);
endmodule
)");

    ModuleEditorWindow window(nullptr, &projectManager);
    QVERIFY(window.findChild<QAction *>(QStringLiteral("moduleImportVerilogAction")));
    QVERIFY(window.importVerilogFiles(
        {verilogFile}, QStringLiteral("imported"), QStringLiteral("uart_import")));

    const QString moduleFile = QDir(projectManager.getModulePath()).filePath("imported.soc_mod");
    const QString saved      = readTextFile(moduleFile);
    QVERIFY(saved.contains(QStringLiteral("uart_import:")));
    QVERIFY(saved.contains(QStringLiteral("clk:")));
    QVERIFY(saved.contains(QStringLiteral("tx:")));
}

void Test::showsModuleUsages()
{
    QTemporaryDir      tempDir;
    QSocProjectManager projectManager;
    initProject(tempDir, projectManager);

    const QString moduleFile = QDir(projectManager.getModulePath()).filePath("uart.soc_mod");
    writeTextFile(moduleFile, R"(
uart:
  port:
    clk:
      direction: in
      type: logic
)");
    writeTextFile(QDir(projectManager.getOutputPath()).filePath("top.soc_net"), R"(
instance:
  u_uart_net:
    module: uart
    port:
      clk: clk
)");
    writeTextFile(QDir(projectManager.getSchematicPath()).filePath("top.soc_sch"), R"(
scene:
  items:
    - module_name: uart
      instance_name: u_uart_sch
      module_yaml: |
        port:
          clk:
            direction: in
)");

    ModuleEditorWindow window(nullptr, &projectManager);
    window.openFile(moduleFile);

    auto *usageTable = window.findChild<QTableWidget *>(QStringLiteral("moduleUsageTable"));
    QVERIFY(usageTable);
    QCOMPARE(usageTable->rowCount(), 2);
    QCOMPARE(usageTable->item(0, 0)->text(), QString("netlist"));
    QCOMPARE(usageTable->item(0, 2)->text(), QString("u_uart_net"));
    QCOMPARE(usageTable->item(1, 0)->text(), QString("schematic"));
    QCOMPARE(usageTable->item(1, 2)->text(), QString("u_uart_sch"));
}

void Test::rendersSymbolPreviewFromSchematicModule()
{
    QTemporaryDir      tempDir;
    QSocProjectManager projectManager;
    initProject(tempDir, projectManager);

    const QString moduleFile = QDir(projectManager.getModulePath()).filePath("uart.soc_mod");
    writeTextFile(moduleFile, R"(
uart:
  port:
    clk:
      direction: in
      type: logic
    tx:
      direction: out
      type: logic
  bus:
    ctrl:
      bus: ctrl
      mode: master
      mapping:
        ready: clk
)");

    ModuleEditorWindow window(nullptr, &projectManager);
    window.openFile(moduleFile);

    auto *previewView = window.findChild<QGraphicsView *>(QStringLiteral("moduleSymbolPreviewView"));
    QVERIFY(previewView);
    QVERIFY(previewView->scene());

    SchematicModule *previewModule = nullptr;
    for (QGraphicsItem *item : previewView->scene()->items()) {
        if (auto *module = dynamic_cast<SchematicModule *>(item)) {
            previewModule = module;
            break;
        }
    }

    QVERIFY(previewModule);
    QCOMPARE(previewModule->moduleName(), QString("uart"));
    QVERIFY(!previewModule->sceneBoundingRect().isEmpty());
}

void Test::busModeEditsRebuildMappingRows()
{
    QTemporaryDir      tempDir;
    QSocProjectManager projectManager;
    initProject(tempDir, projectManager);

    writeTextFile(QDir(projectManager.getBusPath()).filePath("ctrl.soc_bus"), R"(
ctrl:
  port:
    ready:
      master:
        direction: in
)");
    writeTextFile(QDir(projectManager.getBusPath()).filePath("alt.soc_bus"), R"(
alt:
  port:
    valid:
      slave:
        direction: in
)");
    const QString moduleFile = QDir(projectManager.getModulePath()).filePath("uart.soc_mod");
    writeTextFile(moduleFile, R"(
uart:
  port:
    uart_ready:
      direction: in
      type: logic
  bus:
    ctrl:
      bus: ctrl
      mode: master
      mapping:
        ready: uart_ready
)");

    ModuleEditorWindow window(nullptr, &projectManager);
    window.openFile(moduleFile);

    QVERIFY(window.busInterfaceModelForTest()->setData(
        window.busInterfaceModelForTest()->index(0, ModuleBusInterfaceModel::BusColumn),
        QStringLiteral("alt")));
    QVERIFY(window.busInterfaceModelForTest()->setData(
        window.busInterfaceModelForTest()->index(0, ModuleBusInterfaceModel::ModeColumn),
        QStringLiteral("slave")));

    QCOMPARE(window.busMappingModelForTest()->rowCount(), 2);
    QCOMPARE(
        window.busMappingModelForTest()
            ->index(0, ModuleBusMappingModel::BusSignalColumn)
            .data()
            .toString(),
        QString("valid"));
    QCOMPARE(
        window.busMappingModelForTest()
            ->index(1, ModuleBusMappingModel::ProblemColumn)
            .data()
            .toString(),
        QString("Unknown bus signal"));
    QVERIFY(window.busInterfaceModelForTest()->isDirty());
}

void Test::savesRenamedBusInterface()
{
    QTemporaryDir      tempDir;
    QSocProjectManager projectManager;
    initProject(tempDir, projectManager);

    writeTextFile(QDir(projectManager.getBusPath()).filePath("ctrl.soc_bus"), R"(
ctrl:
  port:
    ready:
      master:
        direction: in
)");
    const QString moduleFile = QDir(projectManager.getModulePath()).filePath("uart.soc_mod");
    writeTextFile(moduleFile, R"(
uart:
  port:
    uart_ready:
      direction: in
      type: logic
  bus:
    ctrl:
      bus: ctrl
      mode: master
      mapping:
        ready: uart_ready
)");

    ModuleEditorWindow window(nullptr, &projectManager);
    window.openFile(moduleFile);
    QVERIFY(window.busInterfaceModelForTest()->setData(
        window.busInterfaceModelForTest()->index(0, ModuleBusInterfaceModel::InterfaceColumn),
        QStringLiteral("ctrl_main")));
    QVERIFY(QMetaObject::invokeMethod(&window, "handleSave", Qt::DirectConnection));

    const QString saved = readTextFile(moduleFile);
    QVERIFY(saved.contains(QStringLiteral("ctrl_main:")));
    QVERIFY(!saved.contains(QStringLiteral("    ctrl:")));
    QVERIFY(saved.contains(QStringLiteral("bus: ctrl")));
}

void Test::rowActionsEditCurrentTable()
{
    QTemporaryDir      tempDir;
    QSocProjectManager projectManager;
    initProject(tempDir, projectManager);

    writeTextFile(QDir(projectManager.getBusPath()).filePath("ctrl.soc_bus"), R"(
ctrl:
  port:
    ready:
      master:
        direction: in
)");
    const QString moduleFile = QDir(projectManager.getModulePath()).filePath("uart.soc_mod");
    writeTextFile(moduleFile, R"(
uart:
  port:
    uart_ready:
      direction: in
      type: logic
  bus:
    ctrl:
      bus: ctrl
      mode: master
      mapping:
        ready: uart_ready
)");

    ModuleEditorWindow window(nullptr, &projectManager);
    window.openFile(moduleFile);

    auto *tabs        = window.findChild<QTabWidget *>(QStringLiteral("moduleEditorTabs"));
    auto *mappingView = window.findChild<QTableView *>(QStringLiteral("moduleBusMappingTableView"));
    QVERIFY(tabs);
    QVERIFY(mappingView);
    tabs->setCurrentWidget(mappingView);

    auto *addRow       = window.findChild<QAction *>(QStringLiteral("moduleAddRowAction"));
    auto *duplicateRow = window.findChild<QAction *>(QStringLiteral("moduleDuplicateRowAction"));
    auto *deleteRow    = window.findChild<QAction *>(QStringLiteral("moduleDeleteRowAction"));
    QVERIFY(addRow);
    QVERIFY(duplicateRow);
    QVERIFY(deleteRow);

    QCOMPARE(window.busMappingModelForTest()->rowCount(), 1);
    addRow->trigger();
    QCOMPARE(window.busMappingModelForTest()->rowCount(), 2);

    const QModelIndex proxyRow = mappingView->model()->index(1, 0);
    QVERIFY(proxyRow.isValid());
    mappingView->selectionModel()
        ->select(proxyRow, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    mappingView->setCurrentIndex(proxyRow);
    duplicateRow->trigger();
    QCOMPARE(window.busMappingModelForTest()->rowCount(), 3);

    deleteRow->trigger();
    QCOMPARE(window.busMappingModelForTest()->rowCount(), 2);
    QVERIFY(window.busInterfaceModelForTest()->isDirty());
}

void Test::mappingToolsManageMissingRowsAndFilters()
{
    QTemporaryDir      tempDir;
    QSocProjectManager projectManager;
    initProject(tempDir, projectManager);

    writeTextFile(QDir(projectManager.getBusPath()).filePath("ctrl.soc_bus"), R"(
ctrl:
  port:
    ready:
      master:
        direction: in
        width: 1
    data:
      master:
        direction: out
        width: 8
)");
    const QString moduleFile = QDir(projectManager.getModulePath()).filePath("uart.soc_mod");
    writeTextFile(moduleFile, R"(
uart:
  port:
    ready:
      direction: in
      type: logic
  bus:
    ctrl:
      bus: ctrl
      mode: master
      mapping:
        ready:
        data:
        stale: old_port
)");

    ModuleEditorWindow window(nullptr, &projectManager);
    window.openFile(moduleFile);

    auto *tabs        = window.findChild<QTabWidget *>(QStringLiteral("moduleEditorTabs"));
    auto *mappingView = window.findChild<QTableView *>(QStringLiteral("moduleBusMappingTableView"));
    QVERIFY(tabs);
    QVERIFY(mappingView);
    tabs->setCurrentWidget(mappingView);

    auto *showEmpty = window.findChild<QAction *>(QStringLiteral("moduleShowEmptyMappingsAction"));
    auto *showProblems = window.findChild<QAction *>(QStringLiteral("moduleShowOnlyProblemsAction"));
    auto *createPorts = window.findChild<QAction *>(
        QStringLiteral("moduleCreateMissingPortsAction"));
    auto *clearMissing = window.findChild<QAction *>(QStringLiteral("moduleClearMissingAction"));
    QVERIFY(showEmpty);
    QVERIFY(showProblems);
    QVERIFY(createPorts);
    QVERIFY(clearMissing);

    QCOMPARE(window.busMappingModelForTest()->rowCount(), 3);
    showEmpty->setChecked(true);
    QCOMPARE(mappingView->model()->rowCount(), 2);
    showEmpty->setChecked(false);

    createPorts->trigger();
    QCOMPARE(window.portModelForTest()->rowCount(), 2);
    QCOMPARE(
        window.busMappingModelForTest()
            ->index(0, ModuleBusMappingModel::ModulePortColumn)
            .data()
            .toString(),
        QString("ready"));
    QCOMPARE(
        window.busMappingModelForTest()
            ->index(1, ModuleBusMappingModel::ModulePortColumn)
            .data()
            .toString(),
        QString("ctrl_data"));
    QCOMPARE(
        window.portModelForTest()->index(1, ModulePortModel::TypeColumn).data().toString(),
        QString("logic[7:0]"));

    showProblems->setChecked(true);
    QCOMPARE(mappingView->model()->rowCount(), 1);
    showProblems->setChecked(false);

    clearMissing->trigger();
    QCOMPARE(window.busMappingModelForTest()->rowCount(), 2);
    QCOMPARE(mappingView->model()->rowCount(), 2);
}

void Test::openBusEditorActionShowsBusEditor()
{
    QTemporaryDir      tempDir;
    QSocProjectManager projectManager;
    initProject(tempDir, projectManager);

    writeTextFile(QDir(projectManager.getBusPath()).filePath("ctrl.soc_bus"), R"(
ctrl:
  port:
    ready:
      master:
        direction: in
)");
    const QString moduleFile = QDir(projectManager.getModulePath()).filePath("uart.soc_mod");
    writeTextFile(moduleFile, R"(
uart:
  port:
    uart_ready:
      direction: in
      type: logic
  bus:
    ctrl:
      bus: ctrl
      mode: master
      mapping:
        ready: uart_ready
)");

    ModuleEditorWindow window(nullptr, &projectManager);
    window.openFile(moduleFile);

    auto *action = window.findChild<QAction *>(QStringLiteral("moduleOpenBusEditorAction"));
    QVERIFY(action);
    QVERIFY(action->isEnabled());
    action->trigger();

    auto *busEditor = window.findChild<BusEditorWindow *>();
    QVERIFY(busEditor);
    QVERIFY(busEditor->isVisible());
}

void Test::savesEditedMapping()
{
    QTemporaryDir      tempDir;
    QSocProjectManager projectManager;
    initProject(tempDir, projectManager);

    writeTextFile(QDir(projectManager.getBusPath()).filePath("ctrl.soc_bus"), R"(
ctrl:
  port:
    ready:
      master:
        direction: in
)");
    const QString moduleFile = QDir(projectManager.getModulePath()).filePath("uart.soc_mod");
    writeTextFile(moduleFile, R"(
uart:
  port:
    uart_ready:
      direction: in
      type: logic
  bus:
    ctrl:
      bus: ctrl
      mode: master
      mapping:
        ready:
)");

    ModuleEditorWindow window(nullptr, &projectManager);
    window.openFile(moduleFile);
    QVERIFY(window.busMappingModelForTest()->setData(
        window.busMappingModelForTest()->index(0, ModuleBusMappingModel::ModulePortColumn),
        QStringLiteral("uart_ready")));
    QVERIFY(QMetaObject::invokeMethod(&window, "handleSave", Qt::DirectConnection));

    const QString saved = readTextFile(moduleFile);
    QVERIFY(saved.contains(QStringLiteral("ready: uart_ready")));
}

void Test::mainWindowDoubleClickOpensModuleEditor()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString moduleFile = QDir(tempDir.path()).filePath("uart.soc_mod");
    writeTextFile(moduleFile, R"(
uart:
  port:
    clk:
      direction: in
)");

    MainWindow window;
    auto      *tree = window.findChild<QTreeView *>(QStringLiteral("treeViewProjectFile"));
    QVERIFY(tree);

    auto *model = new QStandardItemModel(&window);
    auto *item  = new QStandardItem(QStringLiteral("uart.soc_mod"));
    item->setData(moduleFile, Qt::UserRole);
    model->appendRow(item);
    tree->setModel(model);

    QVERIFY(
        QMetaObject::invokeMethod(
            &window,
            "handleTreeDoubleClick",
            Qt::DirectConnection,
            Q_ARG(QModelIndex, model->indexFromItem(item))));

    auto *editor = window.findChild<ModuleEditorWindow *>();
    QVERIFY(editor);
    QVERIFY(editor->isVisible());
}

} // namespace

QTEST_MAIN(Test)
#include "test_qsocguimoduleeditorwindow.moc"

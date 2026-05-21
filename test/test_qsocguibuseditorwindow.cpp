// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "gui/buseditorwindow/buscsvimportdialog.h"
#include "gui/buseditorwindow/buseditorwindow.h"
#include "gui/mainwindow/mainwindow.h"

#include <yaml-cpp/yaml.h>

#include <QAbstractButton>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QTableView>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>
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

void initProject(QTemporaryDir &tempDir, QSocProjectManager &projectManager)
{
    QVERIFY(tempDir.isValid());
    const QString projectPath = QDir(tempDir.path()).filePath("bus_gui");
    projectManager.setProjectName("bus_gui");
    projectManager.setCurrentPath(projectPath);
    QVERIFY(projectManager.mkpath());
    QVERIFY(projectManager.save("bus_gui"));
    QVERIFY(projectManager.load("bus_gui"));
}

void clickNextMessageBox(QMessageBox::StandardButton button)
{
    QTimer::singleShot(0, [button]() {
        auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        if (!box)
            return;
        if (QAbstractButton *messageButton = box->button(button))
            messageButton->click();
    });
}

class Test : public QObject
{
    Q_OBJECT

private slots:
    void opensWithoutProject();
    void opensEmptyProject();
    void opensFirstInvalidBus();
    void mainWindowDoubleClickOpensBusEditor();
    void loadsProjectBusLibrary();
    void editsAndSavesBus();
    void csvImportDialogShowsPreview();
    void importsCsvRowsBeforeSave();
    void findShortcutFocusesSearch();
    void scanFailureAllowsOrdinarySaveButBlocksSignalRename();
    void inspectorShowsUsagesAndSelectsProblemRows();
    void duplicatesBusAndBlocksDeletingNonEmptyLibrary();
    void deletesUnusedBusAndBlocksUsedBus();
    void deletesEmptyLibrary();
    void renamesBusSignalAndModeReferences();
};

void Test::opensWithoutProject()
{
    BusEditorWindow window;

    QCOMPARE(window.signalModeModelForTest()->rowCount(), 0);
    QCOMPARE(window.libraryModelForTest()->rowCount(), 1);

    auto *summary = window.findChild<QLabel *>(QStringLiteral("busSummaryLabel"));
    QVERIFY(summary);
    QCOMPARE(summary->text(), QString("No bus selected."));
}

void Test::opensEmptyProject()
{
    QTemporaryDir      tempDir;
    QSocProjectManager projectManager;
    initProject(tempDir, projectManager);

    BusEditorWindow window(nullptr, &projectManager);

    QCOMPARE(window.signalModeModelForTest()->rowCount(), 0);
    QCOMPARE(window.libraryModelForTest()->rowCount(), 1);

    auto *summary = window.findChild<QLabel *>(QStringLiteral("busSummaryLabel"));
    QVERIFY(summary);
    QCOMPARE(summary->text(), QString("No bus selected."));
}

void Test::opensFirstInvalidBus()
{
    QTemporaryDir      tempDir;
    QSocProjectManager projectManager;
    initProject(tempDir, projectManager);

    writeTextFile(QDir(projectManager.getBusPath()).filePath("aaa.soc_bus"), R"(
aaa:
  port:
    ready:
      master:
        direction: in
        width: 1
)");
    writeTextFile(QDir(projectManager.getBusPath()).filePath("zzz.soc_bus"), R"(
zzz:
  port:
    broken:
      master:
        direction: sideways
        width: 1
)");

    BusEditorWindow window(nullptr, &projectManager);

    const QSocBusDefinition definition = window.signalModeModelForTest()->definition();
    QCOMPARE(definition.libraryName, QString("zzz"));
    QCOMPARE(definition.busName, QString("zzz"));
}

void Test::mainWindowDoubleClickOpensBusEditor()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString busFile = QDir(tempDir.path()).filePath("ctrl.soc_bus");
    writeTextFile(busFile, R"(
ctrl:
  port:
    ready:
      master:
        direction: in
)");

    MainWindow window;
    auto      *tree = window.findChild<QTreeView *>(QStringLiteral("treeViewProjectFile"));
    QVERIFY(tree);

    auto *model = new QStandardItemModel(&window);
    auto *item  = new QStandardItem(QStringLiteral("ctrl.soc_bus"));
    item->setData(busFile, Qt::UserRole);
    model->appendRow(item);
    tree->setModel(model);

    QVERIFY(
        QMetaObject::invokeMethod(
            &window,
            "handleTreeDoubleClick",
            Qt::DirectConnection,
            Q_ARG(QModelIndex, model->indexFromItem(item))));

    auto *editor = window.findChild<BusEditorWindow *>();
    QVERIFY(editor);
    QVERIFY(editor->isVisible());
}

void Test::loadsProjectBusLibrary()
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
        description: Ready
)");

    BusEditorWindow window(nullptr, &projectManager);
    window.openFile(busFile);

    QCOMPARE(window.signalModeModelForTest()->rowCount(), 1);
    const QSocBusDefinition definition = window.signalModeModelForTest()->definition();
    QCOMPARE(definition.libraryName, QString("ctrl"));
    QCOMPARE(definition.busName, QString("ctrl"));
    QCOMPARE(definition.rows.first().signal, QString("ready"));
    QCOMPARE(definition.rows.first().description, QString("Ready"));

    auto      *libraryModel = window.libraryModelForTest();
    const auto libraryIndex = libraryModel->indexForBus("ctrl", {});
    const auto busIndex     = libraryModel->indexForBus("ctrl", "ctrl");
    const auto pathIndex
        = libraryModel->index(libraryIndex.row(), BusLibraryModel::PathColumn, libraryIndex.parent());
    const auto countIndex
        = libraryModel
              ->index(libraryIndex.row(), BusLibraryModel::BusCountColumn, libraryIndex.parent());
    const auto statusIndex
        = libraryModel
              ->index(libraryIndex.row(), BusLibraryModel::StatusColumn, libraryIndex.parent());
    QVERIFY(busIndex.isValid());
    QCOMPARE(pathIndex.data().toString(), QString("bus/ctrl.soc_bus"));
    QCOMPARE(countIndex.data().toString(), QString("1"));
    QCOMPARE(statusIndex.data().toString(), QString("Loaded"));
    QVERIFY(BusLibraryModel::isEnabled(busIndex));
    QVERIFY(libraryModel->setLibraryEnabled("ctrl", false));
    QVERIFY(!BusLibraryModel::isEnabled(busIndex));
    QVERIFY(!libraryModel->flags(busIndex).testFlag(Qt::ItemIsEnabled));

    auto *preview = window.findChild<QPlainTextEdit *>(QStringLiteral("busYamlPreview"));
    QVERIFY(preview);
    QVERIFY(preview->toPlainText().contains(QStringLiteral("ready")));
}

void Test::editsAndSavesBus()
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

    BusEditorWindow window(nullptr, &projectManager);
    window.openFile(busFile);

    auto *model = window.signalModeModelForTest();
    QVERIFY(
        model->setData(model->index(0, BusSignalModeModel::DescriptionColumn), QString("Ready line")));
    QVERIFY(model->isDirty());
    QVERIFY(QMetaObject::invokeMethod(&window, "handleSave"));
    QVERIFY(!model->isDirty());

    const YAML::Node saved = YAML::LoadFile(busFile.toStdString());
    QCOMPARE(
        QString::fromStdString(
            saved["ctrl"]["port"]["ready"]["master"]["description"].as<std::string>()),
        QString("Ready line"));
}

void Test::csvImportDialogShowsPreview()
{
    QSocBusSignalMode row;
    row.signal      = QStringLiteral("ready");
    row.mode        = QStringLiteral("master");
    row.direction   = QStringLiteral("in");
    row.width       = QStringLiteral("1");
    row.qualifier   = QStringLiteral("flag");
    row.description = QStringLiteral("Ready line");

    BusCsvImportDialog dialog({row}, {QStringLiteral("checked warning")}, nullptr);
    auto *table = dialog.findChild<QTableWidget *>(QStringLiteral("busCsvPreviewTable"));
    QVERIFY(table);
    QCOMPARE(table->rowCount(), 1);
    QCOMPARE(table->item(0, BusSignalModeModel::SignalColumn)->text(), QString("ready"));
    QCOMPARE(table->item(0, BusSignalModeModel::DescriptionColumn)->text(), QString("Ready line"));

    auto *warnings = dialog.findChild<QListWidget *>(QStringLiteral("busCsvWarningList"));
    QVERIFY(warnings);
    QCOMPARE(warnings->count(), 1);
}

void Test::importsCsvRowsBeforeSave()
{
    QTemporaryDir      tempDir;
    QSocProjectManager projectManager;
    initProject(tempDir, projectManager);

    const QString csvFile = QDir(tempDir.path()).filePath("ctrl.csv");
    writeTextFile(
        csvFile,
        "Name,Mode,Direction,Width,Qualifier,Description\n"
        "ready,master,in,1,flag,Ready line\n");

    BusEditorWindow window(nullptr, &projectManager);
    QStringList     warnings;
    QVERIFY(window.importCsvFiles(
        {csvFile},
        QStringLiteral("ctrl"),
        QStringLiteral("ctrl"),
        BusCsvMergeMode::Replace,
        &warnings));
    QVERIFY(warnings.isEmpty());

    auto *model = window.signalModeModelForTest();
    QCOMPARE(model->rowCount(), 1);
    QVERIFY(model->isDirty());
    QCOMPARE(model->definition().rows.first().description, QString("Ready line"));
    QVERIFY(QMetaObject::invokeMethod(&window, "handleSave"));

    const QString busFile = QDir(projectManager.getBusPath()).filePath("ctrl.soc_bus");
    QVERIFY(QFile::exists(busFile));
    const YAML::Node saved = YAML::LoadFile(busFile.toStdString());
    QCOMPARE(
        QString::fromStdString(
            saved["ctrl"]["port"]["ready"]["master"]["description"].as<std::string>()),
        QString("Ready line"));
}

void Test::findShortcutFocusesSearch()
{
    BusEditorWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    auto *search = window.findChild<QLineEdit *>(QStringLiteral("busSearchEdit"));
    QVERIFY(search);
    search->setText(QStringLiteral("ready"));
    search->clearFocus();

    QAction *findAction = nullptr;
    for (QAction *action : window.actions()) {
        if (action->shortcut().matches(QKeySequence::Find) == QKeySequence::ExactMatch) {
            findAction = action;
            break;
        }
    }
    QVERIFY(findAction);

    findAction->trigger();
    QVERIFY(search->hasFocus());
    QCOMPARE(search->selectedText(), QString("ready"));
}

void Test::scanFailureAllowsOrdinarySaveButBlocksSignalRename()
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
    writeTextFile(QDir(projectManager.getModulePath()).filePath("broken.soc_mod"), "uart: [\n");

    BusEditorWindow window(nullptr, &projectManager);
    window.openFile(busFile);

    auto *model = window.signalModeModelForTest();
    QVERIFY(
        model->setData(model->index(0, BusSignalModeModel::DescriptionColumn), QString("Ready")));
    QVERIFY(QMetaObject::invokeMethod(&window, "handleSave"));
    QVERIFY(!model->isDirty());

    YAML::Node saved = YAML::LoadFile(busFile.toStdString());
    QCOMPARE(
        QString::fromStdString(
            saved["ctrl"]["port"]["ready"]["master"]["description"].as<std::string>()),
        QString("Ready"));

    QVERIFY(model->setData(model->index(0, BusSignalModeModel::SignalColumn), QString("valid")));
    clickNextMessageBox(QMessageBox::Ok);
    QVERIFY(QMetaObject::invokeMethod(&window, "handleSave"));
    QVERIFY(model->isDirty());

    saved = YAML::LoadFile(busFile.toStdString());
    QVERIFY(saved["ctrl"]["port"]["ready"]);
    QVERIFY(!saved["ctrl"]["port"]["valid"]);
}

void Test::inspectorShowsUsagesAndSelectsProblemRows()
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
    broken:
      master:
        direction: sideways
        width: 1
)");

    writeTextFile(QDir(projectManager.getModulePath()).filePath("periph.soc_mod"), R"(
uart:
  bus:
    reg:
      bus: ctrl
      mode: master
      mapping:
        ready: ready_i
        broken: ''
)");

    BusEditorWindow window(nullptr, &projectManager);
    window.openFile(busFile);

    auto *usageTable = window.findChild<QTableWidget *>(QStringLiteral("busUsageTable"));
    QVERIFY(usageTable);
    QCOMPARE(usageTable->rowCount(), 1);
    QCOMPARE(usageTable->columnCount(), 7);
    QCOMPARE(usageTable->item(0, 3)->text(), QString("ctrl"));
    QCOMPARE(usageTable->item(0, 5)->text(), QString("2"));
    QVERIFY(usageTable->item(0, 6)->text().contains(QString("Empty")));

    auto *summary = window.findChild<QLabel *>(QStringLiteral("busSummaryLabel"));
    QVERIFY(summary);
    QVERIFY(summary->text().contains(QStringLiteral("Signals: 2")));
    QVERIFY(summary->text().contains(QStringLiteral("Modes: 1")));
    QVERIFY(summary->text().contains(QStringLiteral("Usages: 1")));

    auto *problems = window.findChild<QListWidget *>(QStringLiteral("busProblemList"));
    QVERIFY(problems);
    QListWidgetItem *invalidDirection = nullptr;
    QListWidgetItem *emptyMapping     = nullptr;
    for (int i = 0; i < problems->count(); ++i) {
        if (problems->item(i)->text().contains(QStringLiteral("invalid-direction"))) {
            invalidDirection = problems->item(i);
        }
        if (problems->item(i)->text().contains(QStringLiteral("empty-mapping-value"))) {
            emptyMapping = problems->item(i);
        }
    }
    QVERIFY(invalidDirection);
    QVERIFY(emptyMapping);

    bool      ok         = false;
    const int sourceRow  = invalidDirection->data(Qt::UserRole).toInt(&ok);
    auto     *signalView = window.findChild<QTableView *>(QStringLiteral("busSignalTableView"));
    QVERIFY(ok);
    QVERIFY(signalView);
    QVERIFY(
        QMetaObject::invokeMethod(
            &window,
            "handleProblemActivated",
            Qt::DirectConnection,
            Q_ARG(QListWidgetItem *, invalidDirection)));
    QCOMPARE(signalView->currentIndex().row(), sourceRow);
}

void Test::duplicatesBusAndBlocksDeletingNonEmptyLibrary()
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
        description: Ready
)");

    BusEditorWindow window(nullptr, &projectManager);
    window.openFile(busFile);

    QVERIFY(window.duplicateCurrentBus(QStringLiteral("ctrl_copy")));
    const YAML::Node saved = YAML::LoadFile(busFile.toStdString());
    QVERIFY(saved["ctrl"]);
    QVERIFY(saved["ctrl_copy"]);
    QCOMPARE(
        QString::fromStdString(
            saved["ctrl_copy"]["port"]["ready"]["master"]["description"].as<std::string>()),
        QString("Ready"));
    QVERIFY(!window.duplicateCurrentBus(QStringLiteral("ctrl")));
    QVERIFY(!window.deleteLibrary(QStringLiteral("ctrl")));
    QVERIFY(QFile::exists(busFile));
}

void Test::deletesUnusedBusAndBlocksUsedBus()
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

    BusEditorWindow window(nullptr, &projectManager);
    window.openFile(busFile);

    clickNextMessageBox(QMessageBox::Yes);
    QVERIFY(QMetaObject::invokeMethod(&window, "handleDeleteBus", Qt::DirectConnection));
    QVERIFY(!QFile::exists(busFile));

    writeTextFile(busFile, R"(
ctrl:
  port:
    ready:
      master:
        direction: in
        width: 1
)");
    writeTextFile(QDir(projectManager.getModulePath()).filePath("periph.soc_mod"), R"(
uart:
  bus:
    reg:
      bus: ctrl
      mode: master
      mapping:
        ready: ready_i
)");

    window.openFile(busFile);
    clickNextMessageBox(QMessageBox::Ok);
    QVERIFY(QMetaObject::invokeMethod(&window, "handleDeleteBus", Qt::DirectConnection));
    QVERIFY(QFile::exists(busFile));

    const YAML::Node saved = YAML::LoadFile(busFile.toStdString());
    QVERIFY(saved["ctrl"]);
}

void Test::deletesEmptyLibrary()
{
    QTemporaryDir      tempDir;
    QSocProjectManager projectManager;
    initProject(tempDir, projectManager);

    BusEditorWindow window(nullptr, &projectManager);
    QVERIFY(window.createLibrary(QStringLiteral("scratch")));
    QVERIFY(!window.createLibrary(QStringLiteral("scratch")));
    QVERIFY(window.libraryModelForTest()->indexForBus(QStringLiteral("scratch"), {}).isValid());

    const QString libraryFile = QDir(projectManager.getBusPath()).filePath("scratch.soc_bus");
    QVERIFY(!QFile::exists(libraryFile));
    QVERIFY(window.deleteLibrary(QStringLiteral("scratch")));
    QVERIFY(!window.libraryModelForTest()->indexForBus(QStringLiteral("scratch"), {}).isValid());
    QVERIFY(!QFile::exists(libraryFile));
}

void Test::renamesBusSignalAndModeReferences()
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

    const QString moduleFile = QDir(projectManager.getModulePath()).filePath("periph.soc_mod");
    writeTextFile(moduleFile, R"(
uart:
  bus:
    reg:
      bus: ctrl
      mode: master
      mapping:
        ready: ready_i
)");

    BusEditorWindow window(nullptr, &projectManager);
    window.openFile(busFile);

    QVERIFY(window.renameCurrentBus(QStringLiteral("ctrl2"), true));
    YAML::Node savedModule = YAML::LoadFile(moduleFile.toStdString());
    QCOMPARE(
        QString::fromStdString(savedModule["uart"]["bus"]["reg"]["bus"].as<std::string>()),
        QString("ctrl2"));

    auto *model = window.signalModeModelForTest();
    QVERIFY(model->setData(model->index(0, BusSignalModeModel::SignalColumn), QString("valid")));
    clickNextMessageBox(QMessageBox::Yes);
    QVERIFY(QMetaObject::invokeMethod(&window, "handleSave"));
    QVERIFY(!model->isDirty());

    savedModule = YAML::LoadFile(moduleFile.toStdString());
    QVERIFY(savedModule["uart"]["bus"]["reg"]["mapping"]["valid"]);
    QVERIFY(!savedModule["uart"]["bus"]["reg"]["mapping"]["ready"]);

    QVERIFY(model->setData(model->index(0, BusSignalModeModel::ModeColumn), QString("target")));
    clickNextMessageBox(QMessageBox::Yes);
    QVERIFY(QMetaObject::invokeMethod(&window, "handleSave"));
    QVERIFY(!model->isDirty());

    savedModule = YAML::LoadFile(moduleFile.toStdString());
    QCOMPARE(
        QString::fromStdString(savedModule["uart"]["bus"]["reg"]["mode"].as<std::string>()),
        QString("target"));

    const YAML::Node savedBus = YAML::LoadFile(busFile.toStdString());
    QVERIFY(savedBus["ctrl2"]["port"]["valid"]["target"]);
}

} // namespace

QTEST_MAIN(Test)
#include "test_qsocguibuseditorwindow.moc"

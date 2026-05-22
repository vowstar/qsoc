// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "moduleeditorwindow.h"

#include "gui/schematicwindow/schematicmodule.h"

#include <qschematic/scene.hpp>

#include <functional>
#include <QAction>
#include <QCloseEvent>
#include <QColor>
#include <QComboBox>
#include <QGraphicsView>
#include <QHeaderView>
#include <QIcon>
#include <QItemSelectionModel>
#include <QKeySequence>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QSplitter>
#include <QStatusBar>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QTableWidget>
#include <QToolBar>
#include <QVBoxLayout>

namespace {

class ModuleComboDelegate : public QStyledItemDelegate
{
public:
    using OptionProvider = std::function<QStringList(const QModelIndex &)>;

    ModuleComboDelegate(OptionProvider optionProvider, QObject *parent = nullptr)
        : QStyledItemDelegate(parent)
        , optionProvider(std::move(optionProvider))
    {}

    QWidget *createEditor(
        QWidget *parent, const QStyleOptionViewItem &, const QModelIndex &index) const override
    {
        auto *combo = new QComboBox(parent);
        combo->setEditable(true);
        combo->addItems(optionProvider ? optionProvider(index) : QStringList());
        return combo;
    }

    void setEditorData(QWidget *editor, const QModelIndex &index) const override
    {
        auto *combo = qobject_cast<QComboBox *>(editor);
        if (!combo)
            return;
        combo->setCurrentText(index.data(Qt::EditRole).toString());
    }

    void setModelData(
        QWidget *editor, QAbstractItemModel *model, const QModelIndex &index) const override
    {
        auto *combo = qobject_cast<QComboBox *>(editor);
        if (!combo)
            return;
        model->setData(index, combo->currentText().trimmed(), Qt::EditRole);
    }

private:
    OptionProvider optionProvider;
};

} // namespace

ModuleEditorWindow::ModuleEditorWindow(QWidget *parent, QSocProjectManager *projectManager)
    : QMainWindow(parent)
    , projectManager(projectManager)
    , busManager(this, projectManager)
    , moduleManager(this, projectManager, &busManager)
{
    setupUi();
    setupActions();
    reloadProject();
}

ModuleEditorWindow::~ModuleEditorWindow() = default;

void ModuleEditorWindow::setupUi()
{
    setWindowTitle(tr("QSoC Module Editor"));
    resize(1280, 760);

    libraryModel        = new ModuleLibraryModel(this);
    portModel           = new ModulePortModel(this);
    parameterModel      = new ModuleParameterModel(this);
    busInterfaceModel   = new ModuleBusInterfaceModel(this);
    busMappingModel     = new ModuleBusMappingModel(this);
    portProxyModel      = new QSortFilterProxyModel(this);
    parameterProxyModel = new QSortFilterProxyModel(this);
    interfaceProxyModel = new QSortFilterProxyModel(this);
    mappingProxyModel   = new ModuleMappingProxyModel(this);

    portProxyModel->setSourceModel(portModel);
    parameterProxyModel->setSourceModel(parameterModel);
    interfaceProxyModel->setSourceModel(busInterfaceModel);
    mappingProxyModel->setSourceModel(busMappingModel);
    const QList<QSortFilterProxyModel *> proxyModels
        = {portProxyModel, parameterProxyModel, interfaceProxyModel, mappingProxyModel};
    for (QSortFilterProxyModel *proxy : proxyModels) {
        proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
        proxy->setFilterKeyColumn(-1);
    }

    auto *splitter = new QSplitter(this);
    setCentralWidget(splitter);

    libraryView = new QTreeView(splitter);
    libraryView->setObjectName(QStringLiteral("moduleLibraryView"));
    libraryView->setModel(libraryModel);
    libraryView->setRootIsDecorated(true);
    libraryView->header()->setStretchLastSection(false);
    libraryView->header()
        ->setSectionResizeMode(ModuleLibraryModel::LibraryColumn, QHeaderView::Stretch);
    libraryView->header()
        ->setSectionResizeMode(ModuleLibraryModel::EnabledColumn, QHeaderView::ResizeToContents);
    libraryView->header()
        ->setSectionResizeMode(ModuleLibraryModel::PathColumn, QHeaderView::ResizeToContents);
    libraryView->header()
        ->setSectionResizeMode(ModuleLibraryModel::ModuleCountColumn, QHeaderView::ResizeToContents);
    libraryView->header()
        ->setSectionResizeMode(ModuleLibraryModel::StatusColumn, QHeaderView::ResizeToContents);
    splitter->addWidget(libraryView);

    auto *editorPanel  = new QWidget(splitter);
    auto *editorLayout = new QVBoxLayout(editorPanel);
    editorLayout->setContentsMargins(6, 0, 6, 0);

    searchEdit = new QLineEdit(editorPanel);
    searchEdit->setObjectName(QStringLiteral("moduleSearchEdit"));
    searchEdit->setPlaceholderText(tr("Search module rows"));
    editorLayout->addWidget(searchEdit);

    editorTabs = new QTabWidget(editorPanel);
    editorTabs->setObjectName(QStringLiteral("moduleEditorTabs"));

    portView = new QTableView(editorTabs);
    portView->setObjectName(QStringLiteral("modulePortTableView"));
    portView->setModel(portProxyModel);
    portView->setSelectionBehavior(QAbstractItemView::SelectRows);
    portView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    portView->setEditTriggers(
        QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed
        | QAbstractItemView::SelectedClicked);
    portView->horizontalHeader()->setStretchLastSection(true);
    portView->verticalHeader()->setVisible(false);
    editorTabs->addTab(portView, tr("Ports"));

    parameterView = new QTableView(editorTabs);
    parameterView->setObjectName(QStringLiteral("moduleParameterTableView"));
    parameterView->setModel(parameterProxyModel);
    parameterView->setSelectionBehavior(QAbstractItemView::SelectRows);
    parameterView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    parameterView->setEditTriggers(
        QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed
        | QAbstractItemView::SelectedClicked);
    parameterView->horizontalHeader()->setStretchLastSection(true);
    parameterView->verticalHeader()->setVisible(false);
    editorTabs->addTab(parameterView, tr("Parameters"));

    busInterfaceView = new QTableView(editorTabs);
    busInterfaceView->setObjectName(QStringLiteral("moduleBusInterfaceTableView"));
    busInterfaceView->setModel(interfaceProxyModel);
    busInterfaceView->setSelectionBehavior(QAbstractItemView::SelectRows);
    busInterfaceView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    busInterfaceView->setEditTriggers(
        QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed
        | QAbstractItemView::SelectedClicked);
    busInterfaceView->horizontalHeader()->setStretchLastSection(true);
    busInterfaceView->verticalHeader()->setVisible(false);
    busInterfaceView->setItemDelegateForColumn(
        ModuleBusInterfaceModel::BusColumn,
        new ModuleComboDelegate(
            [this](const QModelIndex &) { return loadedBusNames(); }, busInterfaceView));
    busInterfaceView->setItemDelegateForColumn(
        ModuleBusInterfaceModel::ModeColumn,
        new ModuleComboDelegate(
            [this](const QModelIndex &index) {
                const QModelIndex source = interfaceProxyModel->mapToSource(index);
                if (!source.isValid())
                    return QStringList();
                return modesForBus(busInterfaceModel->interfaceAt(source.row()).busName);
            },
            busInterfaceView));
    editorTabs->addTab(busInterfaceView, tr("Bus Interfaces"));

    busMappingView = new QTableView(editorTabs);
    busMappingView->setObjectName(QStringLiteral("moduleBusMappingTableView"));
    busMappingView->setModel(mappingProxyModel);
    busMappingView->setSelectionBehavior(QAbstractItemView::SelectRows);
    busMappingView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    busMappingView->setEditTriggers(
        QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed
        | QAbstractItemView::SelectedClicked);
    busMappingView->horizontalHeader()->setStretchLastSection(true);
    busMappingView->verticalHeader()->setVisible(false);
    editorTabs->addTab(busMappingView, tr("Mapping"));

    editorLayout->addWidget(editorTabs);
    splitter->addWidget(editorPanel);

    inspectorTabs = new QTabWidget(splitter);
    inspectorTabs->setObjectName(QStringLiteral("moduleInspectorTabs"));

    summaryLabel = new QLabel(inspectorTabs);
    summaryLabel->setObjectName(QStringLiteral("moduleSummaryLabel"));
    summaryLabel->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    summaryLabel->setWordWrap(true);
    inspectorTabs->addTab(summaryLabel, tr("Summary"));

    problemList = new QListWidget(inspectorTabs);
    problemList->setObjectName(QStringLiteral("moduleProblemList"));
    inspectorTabs->addTab(problemList, tr("Problems"));

    usageTable = new QTableWidget(inspectorTabs);
    usageTable->setObjectName(QStringLiteral("moduleUsageTable"));
    usageTable->setColumnCount(7);
    usageTable->setHorizontalHeaderLabels(
        {tr("Type"),
         tr("File"),
         tr("Instance"),
         tr("Module"),
         tr("Ports"),
         tr("Bus Interfaces"),
         tr("Status")});
    usageTable->horizontalHeader()->setStretchLastSection(true);
    usageTable->verticalHeader()->setVisible(false);
    usageTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    inspectorTabs->addTab(usageTable, tr("Usages"));

    symbolPreviewScene = new QSchematic::Scene(this);
    QSchematic::Settings previewSettings;
    previewSettings.showGrid = false;
    symbolPreviewScene->setSettings(previewSettings);
    symbolPreviewScene->setSceneRect(QRectF(0, 0, 240, 180));

    symbolPreviewView = new QGraphicsView(inspectorTabs);
    symbolPreviewView->setObjectName(QStringLiteral("moduleSymbolPreviewView"));
    symbolPreviewView->setScene(symbolPreviewScene);
    symbolPreviewView->setRenderHint(QPainter::Antialiasing, true);
    symbolPreviewView->setInteractive(false);
    symbolPreviewView->setMinimumHeight(180);
    symbolPreviewView->setBackgroundBrush(QColor(252, 252, 248));
    inspectorTabs->addTab(symbolPreviewView, tr("Preview"));

    yamlPreview = new QPlainTextEdit(inspectorTabs);
    yamlPreview->setObjectName(QStringLiteral("moduleYamlPreview"));
    yamlPreview->setReadOnly(true);
    inspectorTabs->addTab(yamlPreview, tr("YAML"));
    splitter->addWidget(inspectorTabs);

    splitter->setSizes({280, 680, 320});

    statusLabel = new QLabel(this);
    statusBar()->addPermanentWidget(statusLabel, 1);
}

void ModuleEditorWindow::setupActions()
{
    auto *toolBar = addToolBar(tr("Module Editor"));
    toolBar->setMovable(false);

    newLibraryAction = toolBar->addAction(QIcon::fromTheme("folder-new"), tr("New Library"));
    newModuleAction  = toolBar->addAction(QIcon::fromTheme("document-new"), tr("New Module"));
    duplicateModuleAction
        = toolBar->addAction(QIcon::fromTheme("edit-copy"), tr("Duplicate Module"));
    renameModuleAction  = toolBar->addAction(QIcon::fromTheme("edit-rename"), tr("Rename Module"));
    deleteModuleAction  = toolBar->addAction(QIcon::fromTheme("edit-delete"), tr("Delete Module"));
    deleteLibraryAction = toolBar->addAction(QIcon::fromTheme("edit-delete"), tr("Delete Library"));
    toolBar->addSeparator();
    importVerilogAction
        = toolBar->addAction(QIcon::fromTheme("document-open"), tr("Import Verilog"));
    toolBar->addSeparator();
    addRowAction       = toolBar->addAction(QIcon::fromTheme("list-add"), tr("Add Row"));
    duplicateRowAction = toolBar->addAction(QIcon::fromTheme("edit-copy"), tr("Duplicate Row"));
    deleteRowAction    = toolBar->addAction(QIcon::fromTheme("edit-delete"), tr("Delete Row"));
    toolBar->addSeparator();
    addInterfaceAction = toolBar->addAction(QIcon::fromTheme("list-add"), tr("Add Bus Interface"));
    duplicateInterfaceAction
        = toolBar->addAction(QIcon::fromTheme("edit-copy"), tr("Duplicate Bus Interface"));
    renameInterfaceAction
        = toolBar->addAction(QIcon::fromTheme("edit-rename"), tr("Rename Bus Interface"));
    deleteInterfaceAction
        = toolBar->addAction(QIcon::fromTheme("edit-delete"), tr("Delete Bus Interface"));
    autoMatchNameAction
        = toolBar->addAction(QIcon::fromTheme("edit-find-replace"), tr("Auto Match Name"));
    autoMatchAction
        = toolBar->addAction(QIcon::fromTheme("edit-find-replace"), tr("Auto Match Prefix"));
    clearMissingAction = toolBar->addAction(QIcon::fromTheme("edit-clear"), tr("Clear Missing"));
    createMissingPortsAction
        = toolBar->addAction(QIcon::fromTheme("list-add"), tr("Create Missing Ports"));
    showOnlyProblemsAction  = toolBar->addAction(QIcon::fromTheme("view-filter"), tr("Problems"));
    showEmptyMappingsAction = toolBar->addAction(QIcon::fromTheme("view-filter"), tr("Empty"));
    openBusEditorAction
        = toolBar->addAction(QIcon::fromTheme("document-edit"), tr("Open Bus Editor"));
    toolBar->addSeparator();
    saveAction     = toolBar->addAction(QIcon::fromTheme("document-save"), tr("Save"));
    revertAction   = toolBar->addAction(QIcon::fromTheme("document-revert"), tr("Revert"));
    validateAction = toolBar->addAction(QIcon::fromTheme("emblem-ok"), tr("Validate"));
    refreshAction  = toolBar->addAction(QIcon::fromTheme("view-refresh"), tr("Refresh"));

    addAction(newLibraryAction);
    addAction(newModuleAction);
    addAction(duplicateModuleAction);
    addAction(renameModuleAction);
    addAction(deleteModuleAction);
    addAction(deleteLibraryAction);
    addAction(importVerilogAction);
    addAction(addRowAction);
    addAction(duplicateRowAction);
    addAction(deleteRowAction);
    addAction(addInterfaceAction);
    addAction(duplicateInterfaceAction);
    addAction(renameInterfaceAction);
    addAction(deleteInterfaceAction);
    addAction(autoMatchNameAction);
    addAction(autoMatchAction);
    addAction(clearMissingAction);
    addAction(createMissingPortsAction);
    addAction(showOnlyProblemsAction);
    addAction(showEmptyMappingsAction);
    addAction(openBusEditorAction);
    addAction(saveAction);
    addAction(revertAction);

    newLibraryAction->setObjectName(QStringLiteral("moduleNewLibraryAction"));
    newModuleAction->setObjectName(QStringLiteral("moduleNewModuleAction"));
    duplicateModuleAction->setObjectName(QStringLiteral("moduleDuplicateModuleAction"));
    renameModuleAction->setObjectName(QStringLiteral("moduleRenameModuleAction"));
    deleteModuleAction->setObjectName(QStringLiteral("moduleDeleteModuleAction"));
    deleteLibraryAction->setObjectName(QStringLiteral("moduleDeleteLibraryAction"));
    importVerilogAction->setObjectName(QStringLiteral("moduleImportVerilogAction"));
    addRowAction->setObjectName(QStringLiteral("moduleAddRowAction"));
    duplicateRowAction->setObjectName(QStringLiteral("moduleDuplicateRowAction"));
    deleteRowAction->setObjectName(QStringLiteral("moduleDeleteRowAction"));
    addInterfaceAction->setObjectName(QStringLiteral("moduleAddInterfaceAction"));
    duplicateInterfaceAction->setObjectName(QStringLiteral("moduleDuplicateInterfaceAction"));
    renameInterfaceAction->setObjectName(QStringLiteral("moduleRenameInterfaceAction"));
    deleteInterfaceAction->setObjectName(QStringLiteral("moduleDeleteInterfaceAction"));
    autoMatchNameAction->setObjectName(QStringLiteral("moduleAutoMatchNameAction"));
    autoMatchAction->setObjectName(QStringLiteral("moduleAutoMatchAction"));
    clearMissingAction->setObjectName(QStringLiteral("moduleClearMissingAction"));
    createMissingPortsAction->setObjectName(QStringLiteral("moduleCreateMissingPortsAction"));
    showOnlyProblemsAction->setObjectName(QStringLiteral("moduleShowOnlyProblemsAction"));
    showEmptyMappingsAction->setObjectName(QStringLiteral("moduleShowEmptyMappingsAction"));
    openBusEditorAction->setObjectName(QStringLiteral("moduleOpenBusEditorAction"));
    saveAction->setObjectName(QStringLiteral("moduleSaveAction"));
    revertAction->setObjectName(QStringLiteral("moduleRevertAction"));
    validateAction->setObjectName(QStringLiteral("moduleValidateAction"));
    refreshAction->setObjectName(QStringLiteral("moduleRefreshAction"));

    addRowAction->setShortcut(QKeySequence(Qt::Key_Insert));
    duplicateRowAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_D));
    renameInterfaceAction->setShortcut(QKeySequence(Qt::Key_F2));
    deleteRowAction->setShortcut(QKeySequence::Delete);
    autoMatchAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_M));
    showOnlyProblemsAction->setCheckable(true);
    showEmptyMappingsAction->setCheckable(true);
    saveAction->setShortcut(QKeySequence::Save);
    revertAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_R));
    searchEdit->addAction(QIcon::fromTheme("edit-find"), QLineEdit::LeadingPosition);
    auto *focusSearchAction = new QAction(this);
    focusSearchAction->setShortcut(QKeySequence::Find);
    addAction(focusSearchAction);

    connect(newLibraryAction, &QAction::triggered, this, &ModuleEditorWindow::handleNewLibrary);
    connect(newModuleAction, &QAction::triggered, this, &ModuleEditorWindow::handleNewModule);
    connect(
        duplicateModuleAction,
        &QAction::triggered,
        this,
        &ModuleEditorWindow::handleDuplicateModule);
    connect(renameModuleAction, &QAction::triggered, this, &ModuleEditorWindow::handleRenameModule);
    connect(deleteModuleAction, &QAction::triggered, this, &ModuleEditorWindow::handleDeleteModule);
    connect(deleteLibraryAction, &QAction::triggered, this, &ModuleEditorWindow::handleDeleteLibrary);
    connect(importVerilogAction, &QAction::triggered, this, &ModuleEditorWindow::handleImportVerilog);
    connect(addRowAction, &QAction::triggered, this, &ModuleEditorWindow::handleAddRow);
    connect(duplicateRowAction, &QAction::triggered, this, &ModuleEditorWindow::handleDuplicateRow);
    connect(deleteRowAction, &QAction::triggered, this, &ModuleEditorWindow::handleDeleteRow);
    connect(addInterfaceAction, &QAction::triggered, this, &ModuleEditorWindow::handleAddInterface);
    connect(
        duplicateInterfaceAction,
        &QAction::triggered,
        this,
        &ModuleEditorWindow::handleDuplicateInterface);
    connect(
        renameInterfaceAction,
        &QAction::triggered,
        this,
        &ModuleEditorWindow::handleRenameInterface);
    connect(
        deleteInterfaceAction,
        &QAction::triggered,
        this,
        &ModuleEditorWindow::handleDeleteInterface);
    connect(
        autoMatchNameAction, &QAction::triggered, this, &ModuleEditorWindow::handleAutoMatchByName);
    connect(autoMatchAction, &QAction::triggered, this, &ModuleEditorWindow::handleAutoMatchByPrefix);
    connect(
        clearMissingAction,
        &QAction::triggered,
        this,
        &ModuleEditorWindow::handleClearMissingMappings);
    connect(
        createMissingPortsAction,
        &QAction::triggered,
        this,
        &ModuleEditorWindow::handleCreateMissingPorts);
    connect(
        showOnlyProblemsAction,
        &QAction::toggled,
        this,
        &ModuleEditorWindow::handleShowOnlyProblems);
    connect(
        showEmptyMappingsAction,
        &QAction::toggled,
        this,
        &ModuleEditorWindow::handleShowEmptyMappings);
    connect(openBusEditorAction, &QAction::triggered, this, &ModuleEditorWindow::handleOpenBusEditor);
    connect(saveAction, &QAction::triggered, this, &ModuleEditorWindow::handleSave);
    connect(revertAction, &QAction::triggered, this, &ModuleEditorWindow::handleRevert);
    connect(validateAction, &QAction::triggered, this, &ModuleEditorWindow::handleValidate);
    connect(refreshAction, &QAction::triggered, this, &ModuleEditorWindow::handleRefresh);
    connect(searchEdit, &QLineEdit::textChanged, this, &ModuleEditorWindow::handleSearchChanged);
    connect(editorTabs, &QTabWidget::currentChanged, this, [this]() { updateActions(); });
    connect(focusSearchAction, &QAction::triggered, this, [this]() {
        searchEdit->setFocus();
        searchEdit->selectAll();
    });
    connect(problemList, &QListWidget::itemClicked, this, &ModuleEditorWindow::handleProblemActivated);
    connect(
        problemList, &QListWidget::itemActivated, this, &ModuleEditorWindow::handleProblemActivated);

    for (auto *model :
         {static_cast<QAbstractItemModel *>(portModel),
          static_cast<QAbstractItemModel *>(parameterModel),
          static_cast<QAbstractItemModel *>(busInterfaceModel),
          static_cast<QAbstractItemModel *>(busMappingModel)}) {
        connect(model, &QAbstractItemModel::dataChanged, this, &ModuleEditorWindow::updateInspector);
        connect(model, &QAbstractItemModel::rowsInserted, this, &ModuleEditorWindow::updateInspector);
        connect(model, &QAbstractItemModel::rowsRemoved, this, &ModuleEditorWindow::updateInspector);
    }
    connect(portModel, &ModulePortModel::dirtyChanged, this, &ModuleEditorWindow::handleDirtyChanged);
    connect(
        parameterModel,
        &ModuleParameterModel::dirtyChanged,
        this,
        &ModuleEditorWindow::handleDirtyChanged);
    connect(
        busInterfaceModel,
        &ModuleBusInterfaceModel::dirtyChanged,
        this,
        &ModuleEditorWindow::handleDirtyChanged);
    connect(
        busMappingModel,
        &ModuleBusMappingModel::dirtyChanged,
        this,
        &ModuleEditorWindow::handleDirtyChanged);
    connect(
        libraryView->selectionModel(),
        &QItemSelectionModel::currentChanged,
        this,
        &ModuleEditorWindow::handleLibrarySelectionChanged);
    connect(
        busInterfaceView->selectionModel(),
        &QItemSelectionModel::currentChanged,
        this,
        &ModuleEditorWindow::handleInterfaceSelectionChanged);
    connect(
        busInterfaceModel,
        &QAbstractItemModel::dataChanged,
        this,
        &ModuleEditorWindow::handleInterfaceDataChanged);

    updateActions();
}

ModuleLibraryModel *ModuleEditorWindow::libraryModelForTest() const
{
    return libraryModel;
}

ModulePortModel *ModuleEditorWindow::portModelForTest() const
{
    return portModel;
}

ModuleParameterModel *ModuleEditorWindow::parameterModelForTest() const
{
    return parameterModel;
}

ModuleBusInterfaceModel *ModuleEditorWindow::busInterfaceModelForTest() const
{
    return busInterfaceModel;
}

ModuleBusMappingModel *ModuleEditorWindow::busMappingModelForTest() const
{
    return busMappingModel;
}

void ModuleEditorWindow::closeEvent(QCloseEvent *event)
{
    if (!checkSaveBeforeDiscard()) {
        event->ignore();
        return;
    }
    event->accept();
}

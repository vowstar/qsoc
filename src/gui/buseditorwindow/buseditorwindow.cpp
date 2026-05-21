// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "buseditorwindow.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <sstream>
#include <utility>
#include <QAction>
#include <QCloseEvent>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QItemSelectionModel>
#include <QKeySequence>
#include <QListWidget>
#include <QSplitter>
#include <QStatusBar>
#include <QStyledItemDelegate>
#include <QToolBar>
#include <QVBoxLayout>

namespace {

class ComboBoxDelegate : public QStyledItemDelegate
{
public:
    ComboBoxDelegate(QStringList items, bool editable, QObject *parent = nullptr)
        : QStyledItemDelegate(parent)
        , m_items(std::move(items))
        , m_editable(editable)
    {}

    QWidget *createEditor(
        QWidget *parent, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        Q_UNUSED(option)
        Q_UNUSED(index)
        auto *combo = new QComboBox(parent);
        combo->setEditable(m_editable);
        combo->addItems(m_items);
        return combo;
    }

    void setEditorData(QWidget *editor, const QModelIndex &index) const override
    {
        auto *combo = qobject_cast<QComboBox *>(editor);
        if (!combo)
            return;
        const QString value = index.data(Qt::EditRole).toString();
        const int     found = combo->findText(value);
        if (found >= 0) {
            combo->setCurrentIndex(found);
        } else {
            combo->setEditText(value);
        }
    }

    void setModelData(
        QWidget *editor, QAbstractItemModel *model, const QModelIndex &index) const override
    {
        auto *combo = qobject_cast<QComboBox *>(editor);
        if (!combo)
            return;
        model->setData(index, combo->currentText(), Qt::EditRole);
    }

private:
    QStringList m_items;
    bool        m_editable = false;
};

} // namespace

BusEditorWindow::BusEditorWindow(QWidget *parent, QSocProjectManager *projectManager)
    : QMainWindow(parent)
    , projectManager(projectManager)
    , busManager(this, projectManager)
{
    setupUi();
    setupActions();
    reloadProject();
}

void BusEditorWindow::setupUi()
{
    setWindowTitle(tr("QSoC Bus Editor"));
    resize(1180, 720);

    libraryModel     = new BusLibraryModel(this);
    signalModeModel  = new BusSignalModeModel(this);
    signalProxyModel = new QSortFilterProxyModel(this);
    signalProxyModel->setSourceModel(signalModeModel);
    signalProxyModel->setFilterCaseSensitivity(Qt::CaseInsensitive);
    signalProxyModel->setFilterKeyColumn(-1);

    auto *splitter = new QSplitter(this);
    setCentralWidget(splitter);

    libraryView = new QTreeView(splitter);
    libraryView->setObjectName(QStringLiteral("busLibraryView"));
    libraryView->setModel(libraryModel);
    libraryView->setRootIsDecorated(true);
    libraryView->header()->setStretchLastSection(false);
    libraryView->header()->setSectionResizeMode(BusLibraryModel::LibraryColumn, QHeaderView::Stretch);
    libraryView->header()
        ->setSectionResizeMode(BusLibraryModel::EnabledColumn, QHeaderView::ResizeToContents);
    libraryView->header()
        ->setSectionResizeMode(BusLibraryModel::PathColumn, QHeaderView::ResizeToContents);
    libraryView->header()
        ->setSectionResizeMode(BusLibraryModel::BusCountColumn, QHeaderView::ResizeToContents);
    libraryView->header()
        ->setSectionResizeMode(BusLibraryModel::StatusColumn, QHeaderView::ResizeToContents);
    splitter->addWidget(libraryView);

    auto *tablePanel  = new QWidget(splitter);
    auto *tableLayout = new QVBoxLayout(tablePanel);
    tableLayout->setContentsMargins(6, 0, 6, 0);

    searchEdit = new QLineEdit(tablePanel);
    searchEdit->setObjectName(QStringLiteral("busSearchEdit"));
    searchEdit->setPlaceholderText(tr("Search signals"));
    tableLayout->addWidget(searchEdit);

    tableView = new QTableView(tablePanel);
    tableView->setObjectName(QStringLiteral("busSignalTableView"));
    tableView->setModel(signalProxyModel);
    tableView->setSelectionBehavior(QAbstractItemView::SelectRows);
    tableView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    tableView->setEditTriggers(
        QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed
        | QAbstractItemView::SelectedClicked);
    tableView->horizontalHeader()->setStretchLastSection(true);
    tableView->verticalHeader()->setVisible(false);
    tableView->setItemDelegateForColumn(
        BusSignalModeModel::ModeColumn,
        new ComboBoxDelegate({"master", "slave", "system"}, true, tableView));
    tableView->setItemDelegateForColumn(
        BusSignalModeModel::DirectionColumn,
        new ComboBoxDelegate({"in", "out", "inout"}, false, tableView));
    tableView->setItemDelegateForColumn(
        BusSignalModeModel::QualifierColumn,
        new ComboBoxDelegate(
            {"", "clock", "reset", "power", "ground", "data", "control"}, true, tableView));
    tableLayout->addWidget(tableView);
    splitter->addWidget(tablePanel);

    inspectorTabs = new QTabWidget(splitter);
    inspectorTabs->setObjectName(QStringLiteral("busInspectorTabs"));

    summaryLabel = new QLabel(inspectorTabs);
    summaryLabel->setObjectName(QStringLiteral("busSummaryLabel"));
    summaryLabel->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    summaryLabel->setWordWrap(true);
    inspectorTabs->addTab(summaryLabel, tr("Summary"));

    usageTable = new QTableWidget(inspectorTabs);
    usageTable->setObjectName(QStringLiteral("busUsageTable"));
    usageTable->setColumnCount(7);
    usageTable->setHorizontalHeaderLabels(
        {tr("Library"),
         tr("Module"),
         tr("Interface"),
         tr("Bus"),
         tr("Mode"),
         tr("Mappings"),
         tr("Problems")});
    usageTable->horizontalHeader()->setStretchLastSection(true);
    usageTable->verticalHeader()->setVisible(false);
    usageTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    inspectorTabs->addTab(usageTable, tr("Usages"));

    problemList = new QListWidget(inspectorTabs);
    problemList->setObjectName(QStringLiteral("busProblemList"));
    inspectorTabs->addTab(problemList, tr("Problems"));

    yamlPreview = new QPlainTextEdit(inspectorTabs);
    yamlPreview->setObjectName(QStringLiteral("busYamlPreview"));
    yamlPreview->setReadOnly(true);
    inspectorTabs->addTab(yamlPreview, tr("YAML"));
    splitter->addWidget(inspectorTabs);

    splitter->setSizes({260, 610, 310});

    statusLabel = new QLabel(this);
    statusBar()->addPermanentWidget(statusLabel, 1);
}

void BusEditorWindow::setupActions()
{
    auto *toolBar = addToolBar(tr("Bus Editor"));
    toolBar->setMovable(false);

    newLibraryAction   = toolBar->addAction(QIcon::fromTheme("folder-new"), tr("New Library"));
    newBusAction       = toolBar->addAction(QIcon::fromTheme("document-new"), tr("New Bus"));
    duplicateBusAction = toolBar->addAction(QIcon::fromTheme("edit-copy"), tr("Duplicate Bus"));
    renameBusAction    = toolBar->addAction(QIcon::fromTheme("edit-rename"), tr("Rename Bus"));
    importCsvAction    = toolBar->addAction(QIcon::fromTheme("document-open"), tr("Import CSV"));
    toolBar->addSeparator();
    addRowAction        = toolBar->addAction(QIcon::fromTheme("list-add"), tr("Add Row"));
    duplicateRowsAction = toolBar->addAction(QIcon::fromTheme("edit-copy"), tr("Duplicate Row"));
    deleteRowsAction    = toolBar->addAction(QIcon::fromTheme("edit-delete"), tr("Delete Row"));
    toolBar->addSeparator();
    saveAction     = toolBar->addAction(QIcon::fromTheme("document-save"), tr("Save"));
    revertAction   = toolBar->addAction(QIcon::fromTheme("document-revert"), tr("Revert"));
    validateAction = toolBar->addAction(QIcon::fromTheme("emblem-ok"), tr("Validate"));
    refreshAction  = toolBar->addAction(QIcon::fromTheme("view-refresh"), tr("Refresh"));
    toolBar->addSeparator();
    deleteBusAction     = toolBar->addAction(QIcon::fromTheme("edit-delete"), tr("Delete Bus"));
    deleteLibraryAction = toolBar->addAction(QIcon::fromTheme("edit-delete"), tr("Delete Library"));

    addAction(addRowAction);
    addAction(duplicateRowsAction);
    addAction(deleteRowsAction);
    addAction(saveAction);
    addAction(revertAction);

    addRowAction->setShortcut(QKeySequence(Qt::Key_Insert));
    duplicateRowsAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_D));
    deleteRowsAction->setShortcut(QKeySequence::Delete);
    saveAction->setShortcut(QKeySequence::Save);
    revertAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_R));
    searchEdit->addAction(QIcon::fromTheme("edit-find"), QLineEdit::LeadingPosition);
    auto *focusSearchAction = new QAction(this);
    focusSearchAction->setShortcut(QKeySequence::Find);
    addAction(focusSearchAction);

    connect(newLibraryAction, &QAction::triggered, this, &BusEditorWindow::handleNewLibrary);
    connect(newBusAction, &QAction::triggered, this, &BusEditorWindow::handleNewBus);
    connect(duplicateBusAction, &QAction::triggered, this, &BusEditorWindow::handleDuplicateBus);
    connect(renameBusAction, &QAction::triggered, this, &BusEditorWindow::handleRenameBus);
    connect(deleteLibraryAction, &QAction::triggered, this, &BusEditorWindow::handleDeleteLibrary);
    connect(importCsvAction, &QAction::triggered, this, &BusEditorWindow::handleImportCsv);
    connect(addRowAction, &QAction::triggered, this, &BusEditorWindow::handleAddRow);
    connect(duplicateRowsAction, &QAction::triggered, this, &BusEditorWindow::handleDuplicateRows);
    connect(deleteRowsAction, &QAction::triggered, this, &BusEditorWindow::handleDeleteRows);
    connect(deleteBusAction, &QAction::triggered, this, &BusEditorWindow::handleDeleteBus);
    connect(saveAction, &QAction::triggered, this, &BusEditorWindow::handleSave);
    connect(revertAction, &QAction::triggered, this, &BusEditorWindow::handleRevert);
    connect(validateAction, &QAction::triggered, this, &BusEditorWindow::handleValidate);
    connect(refreshAction, &QAction::triggered, this, &BusEditorWindow::handleRefresh);
    connect(searchEdit, &QLineEdit::textChanged, this, &BusEditorWindow::handleSearchChanged);
    connect(focusSearchAction, &QAction::triggered, this, [this]() {
        searchEdit->setFocus();
        searchEdit->selectAll();
    });
    connect(problemList, &QListWidget::itemClicked, this, &BusEditorWindow::handleProblemActivated);
    connect(problemList, &QListWidget::itemActivated, this, &BusEditorWindow::handleProblemActivated);
    connect(
        signalModeModel,
        &BusSignalModeModel::dirtyChanged,
        this,
        &BusEditorWindow::handleDirtyChanged);
    connect(
        signalModeModel, &QAbstractItemModel::dataChanged, this, &BusEditorWindow::updateInspector);
    connect(
        signalModeModel, &QAbstractItemModel::rowsInserted, this, &BusEditorWindow::updateInspector);
    connect(
        signalModeModel, &QAbstractItemModel::rowsRemoved, this, &BusEditorWindow::updateInspector);
    connect(
        libraryView->selectionModel(),
        &QItemSelectionModel::currentChanged,
        this,
        &BusEditorWindow::handleLibrarySelectionChanged);

    updateActions();
}

BusLibraryModel *BusEditorWindow::libraryModelForTest() const
{
    return libraryModel;
}

BusSignalModeModel *BusEditorWindow::signalModeModelForTest() const
{
    return signalModeModel;
}

void BusEditorWindow::closeEvent(QCloseEvent *event)
{
    if (!checkSaveBeforeDiscard()) {
        event->ignore();
        return;
    }
    event->accept();
}

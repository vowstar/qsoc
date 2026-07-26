// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef BUSEDITORWINDOW_H
#define BUSEDITORWINDOW_H

#include "buscsvimportdialog.h"
#include "buslibrarymodel.h"
#include "bussignalmodemodel.h"
#include "common/qsocbusmanager.h"
#include "common/qsocprojectmanager.h"
#include "gui/undo/snapshotcommand.h"

#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QPointer>
#include <QSortFilterProxyModel>
#include <QTabWidget>
#include <QTableView>
#include <QTableWidget>
#include <QTreeView>

class QListWidget;
class QListWidgetItem;
class QAction;

class BusEditorWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit BusEditorWindow(QWidget *parent = nullptr, QSocProjectManager *projectManager = nullptr);

    void setProjectManager(QSocProjectManager *projectManager);
    void openFile(const QString &filePath);
    bool importCsvFiles(
        const QStringList &filePaths,
        const QString     &libraryName,
        const QString     &busName,
        BusCsvMergeMode    mergeMode,
        QStringList       *warnings = nullptr);
    bool createLibrary(const QString &libraryName);
    bool duplicateCurrentBus(const QString &newBusName);
    bool renameCurrentBus(const QString &newBusName, bool updateReferences);
    bool deleteLibrary(const QString &libraryName);

    BusLibraryModel    *libraryModelForTest() const;
    BusSignalModeModel *signalModeModelForTest() const;

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    /* Manual Signal Handlers */
    void handleLibrarySelectionChanged(const QModelIndex &current, const QModelIndex &previous);
    void handleDirtyChanged(bool dirty);
    void handleNewLibrary();
    void handleNewBus();
    void handleDuplicateBus();
    void handleRenameBus();
    void handleDeleteLibrary();
    void handleImportCsv();
    void handleAddRow();
    void handleDuplicateRows();
    void handleDeleteRows();
    void handleDeleteBus();
    void handleSave();
    void handleRevert();
    void handleValidate();
    void handleRefresh();
    void handleSearchChanged(const QString &text);
    void handleProblemActivated(QListWidgetItem *item);

private:
    /* Window Setup */
    void setupUi();
    void setupActions();

    /* Project Loading */
    bool hasWritableProject() const;
    void reloadProject(
        const QString &preferredLibrary = QString(), const QString &preferredBus = QString());
    void selectBus(const QString &libraryName, const QString &busName);
    void clearCurrentBus();

    /* Editing */
    bool checkSaveBeforeDiscard();
    bool saveCurrentBus();
    bool importCsvRows(
        const QList<QSocBusSignalMode> &rows,
        const QString                  &libraryName,
        const QString                  &busName,
        BusCsvMergeMode                 mergeMode);
    QList<int>        selectedSourceRows() const;
    QSocBusDefinition currentDefinitionFromModel() const;

    /**
     * @brief Capture the bus currently loaded in the model.
     * @details The definition owns a YAML node, which copies by reference, so
     *          the node is deep copied or a later edit would rewrite the
     *          snapshot too.
     * @return Snapshot that can be restored later.
     */
    QSocBusDefinition captureDefinition() const;

    /**
     * @brief Load a captured definition back into the model.
     * @param[in] definition Snapshot to restore.
     */
    void restoreDefinition(const QSocBusDefinition &definition);

    using DefinitionScope = SnapshotScope<QSocBusDefinition>;

    /* Inspector */
    QList<QSocBusProblem> currentProblems(QStringList *scanErrors = nullptr) const;
    void                  updateInspector();
    void updateProblems(const QList<QSocBusProblem> &problems, const QStringList &scanErrors);
    void updateUsages(const QList<QSocBusUsage> &usages);
    void updateSummary();
    void updateYamlPreview();
    void updateActions();
    void setStatusText(const QString &text);
    void selectSourceRow(int row);

    /* Member Variables */
    QPointer<QSocProjectManager> projectManager;
    QSocBusManager               busManager;
    /* Bulk edits such as a CSV import rewrite the whole signal table; one
       snapshot per edit gives them a single step of undo. */
    QUndoStack undoStack;
    QAction   *undoAction = nullptr;
    QAction   *redoAction = nullptr;

    QString currentLibraryName;
    QString currentBusName;
    bool    changingSelection = false;

    BusLibraryModel       *libraryModel     = nullptr;
    BusSignalModeModel    *signalModeModel  = nullptr;
    QSortFilterProxyModel *signalProxyModel = nullptr;

    QTreeView      *libraryView   = nullptr;
    QTableView     *tableView     = nullptr;
    QLineEdit      *searchEdit    = nullptr;
    QTabWidget     *inspectorTabs = nullptr;
    QLabel         *summaryLabel  = nullptr;
    QTableWidget   *usageTable    = nullptr;
    QListWidget    *problemList   = nullptr;
    QPlainTextEdit *yamlPreview   = nullptr;
    QLabel         *statusLabel   = nullptr;

    QAction *newLibraryAction    = nullptr;
    QAction *newBusAction        = nullptr;
    QAction *duplicateBusAction  = nullptr;
    QAction *renameBusAction     = nullptr;
    QAction *deleteLibraryAction = nullptr;
    QAction *importCsvAction     = nullptr;
    QAction *addRowAction        = nullptr;
    QAction *duplicateRowsAction = nullptr;
    QAction *deleteRowsAction    = nullptr;
    QAction *deleteBusAction     = nullptr;
    QAction *saveAction          = nullptr;
    QAction *revertAction        = nullptr;
    QAction *validateAction      = nullptr;
    QAction *refreshAction       = nullptr;
};

#endif // BUSEDITORWINDOW_H

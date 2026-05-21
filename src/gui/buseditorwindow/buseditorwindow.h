// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef BUSEDITORWINDOW_H
#define BUSEDITORWINDOW_H

#include "buscsvimportdialog.h"
#include "buslibrarymodel.h"
#include "bussignalmodemodel.h"
#include "common/qsocbusmanager.h"
#include "common/qsocprojectmanager.h"

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
    QString                      currentLibraryName;
    QString                      currentBusName;
    bool                         changingSelection = false;

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

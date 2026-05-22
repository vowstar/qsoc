// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef MODULEEDITORWINDOW_H
#define MODULEEDITORWINDOW_H

#include "common/qsocbusmanager.h"
#include "common/qsocmodulemanager.h"
#include "common/qsocprojectmanager.h"
#include "modulebusinterfacemodel.h"
#include "modulebusmappingmodel.h"
#include "modulelibrarymodel.h"
#include "modulemappingproxymodel.h"
#include "moduleparametermodel.h"
#include "moduleportmodel.h"

#include <QLabel>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QPointer>
#include <QSortFilterProxyModel>
#include <QTabWidget>
#include <QTableView>
#include <QTreeView>

#include <memory>

class QAction;
class BusEditorWindow;
class QCloseEvent;
class QGraphicsView;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class SchematicModule;
class QTableWidget;

namespace QSchematic {
class Scene;
}

class ModuleEditorWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit ModuleEditorWindow(
        QWidget *parent = nullptr, QSocProjectManager *projectManager = nullptr);
    ~ModuleEditorWindow() override;

    void setProjectManager(QSocProjectManager *projectManager);
    void openFile(const QString &filePath);
    bool createLibrary(const QString &libraryName);
    bool createModule(const QString &libraryName, const QString &moduleName);
    bool duplicateCurrentModule(const QString &newModuleName);
    bool renameCurrentModule(const QString &newModuleName);
    bool deleteCurrentModule();
    bool deleteLibrary(const QString &libraryName);
    bool importVerilogFiles(
        const QStringList &filePaths,
        const QString     &libraryName = QString(),
        const QString     &moduleRegex = QString());

    ModuleLibraryModel      *libraryModelForTest() const;
    ModulePortModel         *portModelForTest() const;
    ModuleParameterModel    *parameterModelForTest() const;
    ModuleBusInterfaceModel *busInterfaceModelForTest() const;
    ModuleBusMappingModel   *busMappingModelForTest() const;

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    /* Manual Signal Handlers */
    void handleLibrarySelectionChanged(const QModelIndex &current, const QModelIndex &previous);
    void handleInterfaceSelectionChanged(const QModelIndex &current, const QModelIndex &previous);
    void handleInterfaceDataChanged(
        const QModelIndex &topLeft, const QModelIndex &bottomRight, const QList<int> &roles);
    void handleDirtyChanged(bool dirty);
    void handleNewLibrary();
    void handleNewModule();
    void handleDuplicateModule();
    void handleRenameModule();
    void handleDeleteModule();
    void handleDeleteLibrary();
    void handleAddRow();
    void handleDuplicateRow();
    void handleDeleteRow();
    void handleImportVerilog();
    void handleAddInterface();
    void handleDuplicateInterface();
    void handleRenameInterface();
    void handleDeleteInterface();
    void handleAutoMatchByName();
    void handleAutoMatchByPrefix();
    void handleClearMissingMappings();
    void handleCreateMissingPorts();
    void handleShowOnlyProblems(bool enabled);
    void handleShowEmptyMappings(bool enabled);
    void handleAutoMatch();
    void handleOpenBusEditor();
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
        const QString &preferredLibrary = QString(), const QString &preferredModule = QString());
    void selectModule(const QString &libraryName, const QString &moduleName);
    void clearCurrentModule();

    /* Editing */
    bool                   checkSaveBeforeDiscard();
    bool                   saveCurrentModule();
    bool                   isDirty() const;
    void                   setModelsDirty(bool dirty);
    void                   syncMappingToInterfaceModel();
    void                   rebuildInterfaceMapping(int row, bool markDirty);
    void                   refreshMappingContext(bool markDirty);
    QList<int>             selectedInterfaceRows() const;
    QSocModuleDefinition   currentDefinitionFromModels() const;
    QSocBusDefinition      busDefinitionForName(const QString &busName) const;
    QStringList            loadedBusNames() const;
    QStringList            modesForBus(const QString &busName) const;
    QString                busLibraryPathForName(const QString &busName) const;
    QSocModuleBusInterface selectedInterface() const;

    /* Inspector */
    QList<QSocModuleProblem> currentProblems() const;
    void                     updateInspector();
    void                     updateProblems(const QList<QSocModuleProblem> &problems);
    void updateUsages(const QList<QSocModuleUsage> &usages, const QStringList &scanErrors);
    void updateSummary();
    void updateSymbolPreview();
    void updateYamlPreview();
    void updateActions();
    void setStatusText(const QString &text);
    void selectInterfaceRow(int row);

    /* Member Variables */
    QPointer<QSocProjectManager> projectManager;
    QSocBusManager               busManager;
    QSocModuleManager            moduleManager;
    QString                      currentLibraryName;
    QString                      currentModuleName;
    QSocModuleDefinition         currentDefinitionBase;
    bool                         changingSelection   = false;
    bool                         syncingInterfaceRow = false;
    int                          currentInterfaceRow = -1;
    BusEditorWindow             *busEditorWindow     = nullptr;

    ModuleLibraryModel      *libraryModel        = nullptr;
    ModulePortModel         *portModel           = nullptr;
    ModuleParameterModel    *parameterModel      = nullptr;
    ModuleBusInterfaceModel *busInterfaceModel   = nullptr;
    ModuleBusMappingModel   *busMappingModel     = nullptr;
    QSortFilterProxyModel   *portProxyModel      = nullptr;
    QSortFilterProxyModel   *parameterProxyModel = nullptr;
    QSortFilterProxyModel   *interfaceProxyModel = nullptr;
    ModuleMappingProxyModel *mappingProxyModel   = nullptr;

    QTreeView                       *libraryView        = nullptr;
    QLineEdit                       *searchEdit         = nullptr;
    QTabWidget                      *editorTabs         = nullptr;
    QTableView                      *portView           = nullptr;
    QTableView                      *parameterView      = nullptr;
    QTableView                      *busInterfaceView   = nullptr;
    QTableView                      *busMappingView     = nullptr;
    QTabWidget                      *inspectorTabs      = nullptr;
    QLabel                          *summaryLabel       = nullptr;
    QListWidget                     *problemList        = nullptr;
    QTableWidget                    *usageTable         = nullptr;
    QPlainTextEdit                  *yamlPreview        = nullptr;
    QGraphicsView                   *symbolPreviewView  = nullptr;
    QSchematic::Scene               *symbolPreviewScene = nullptr;
    std::shared_ptr<SchematicModule> symbolPreviewModule;
    QLabel                          *statusLabel = nullptr;

    QAction *newLibraryAction         = nullptr;
    QAction *newModuleAction          = nullptr;
    QAction *duplicateModuleAction    = nullptr;
    QAction *renameModuleAction       = nullptr;
    QAction *deleteModuleAction       = nullptr;
    QAction *deleteLibraryAction      = nullptr;
    QAction *addRowAction             = nullptr;
    QAction *duplicateRowAction       = nullptr;
    QAction *deleteRowAction          = nullptr;
    QAction *importVerilogAction      = nullptr;
    QAction *addInterfaceAction       = nullptr;
    QAction *duplicateInterfaceAction = nullptr;
    QAction *renameInterfaceAction    = nullptr;
    QAction *deleteInterfaceAction    = nullptr;
    QAction *autoMatchNameAction      = nullptr;
    QAction *autoMatchAction          = nullptr;
    QAction *clearMissingAction       = nullptr;
    QAction *createMissingPortsAction = nullptr;
    QAction *showOnlyProblemsAction   = nullptr;
    QAction *showEmptyMappingsAction  = nullptr;
    QAction *openBusEditorAction      = nullptr;
    QAction *saveAction               = nullptr;
    QAction *revertAction             = nullptr;
    QAction *validateAction           = nullptr;
    QAction *refreshAction            = nullptr;
};

#endif // MODULEEDITORWINDOW_H

// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "moduleeditorwindow.h"

#include <QDir>
#include <QFileInfo>
#include <QItemSelectionModel>

namespace {

bool findFirstInvalidModule(QSocModuleManager *manager, QString *libraryName, QString *moduleName)
{
    if (!manager)
        return false;

    for (const QString &candidateLibrary : manager->listLoadedLibraries()) {
        for (const QString &candidateModule : manager->listModulesInLibrary(candidateLibrary)) {
            const QSocModuleDefinition definition
                = manager->getModuleDefinition(candidateLibrary, candidateModule);
            const QList<QSocModuleProblem> problems = manager->validateModuleDefinition(definition);
            for (const QSocModuleProblem &problem : problems) {
                if (problem.severity != QSocModuleProblemSeverity::Error)
                    continue;
                if (libraryName)
                    *libraryName = candidateLibrary;
                if (moduleName)
                    *moduleName = candidateModule;
                return true;
            }
        }
    }
    return false;
}

} // namespace

void ModuleEditorWindow::setProjectManager(QSocProjectManager *projectManager)
{
    if (this->projectManager == projectManager)
        return;

    if (!checkSaveBeforeDiscard())
        return;

    this->projectManager = projectManager;
    busManager.setProjectManager(projectManager);
    moduleManager.setProjectManager(projectManager);
    reloadProject();
}

void ModuleEditorWindow::openFile(const QString &filePath)
{
    const QFileInfo fileInfo(filePath);
    const QString   libraryName = fileInfo.suffix() == QStringLiteral("soc_mod")
                                      ? fileInfo.completeBaseName()
                                      : QString();
    reloadProject(libraryName);
}

bool ModuleEditorWindow::hasWritableProject() const
{
    return projectManager && projectManager->isValid(true);
}

void ModuleEditorWindow::reloadProject(
    const QString &preferredLibrary, const QString &preferredModule)
{
    if (!checkSaveBeforeDiscard())
        return;

    const QString lastLibraryName = currentLibraryName;
    const QString lastModuleName  = currentModuleName;
    currentLibraryName.clear();
    currentModuleName.clear();
    currentDefinitionBase = {};
    currentInterfaceRow   = -1;
    portModel->clear();
    parameterModel->clear();
    busInterfaceModel->clear();
    busMappingModel->clear();
    busManager.resetBusData();
    moduleManager.resetModuleData();

    if (!projectManager || !projectManager->isValid()) {
        libraryModel->setModuleManager(nullptr);
        updateInspector();
        updateActions();
        setStatusText(tr("No project loaded"));
        return;
    }

    busManager.setProjectManager(projectManager);
    moduleManager.setProjectManager(projectManager);
    for (const QString &libraryName : busManager.listLibrary())
        busManager.load(libraryName);
    for (const QString &libraryName : moduleManager.listLibrary())
        moduleManager.load(libraryName);

    libraryModel->setModuleManager(&moduleManager);
    libraryView->expandAll();

    QString libraryToOpen = preferredLibrary;
    QString moduleToOpen  = preferredModule;
    if (libraryToOpen.isEmpty() && moduleToOpen.isEmpty()) {
        QString invalidLibrary;
        QString invalidModule;
        if (findFirstInvalidModule(&moduleManager, &invalidLibrary, &invalidModule)) {
            libraryToOpen = invalidLibrary;
            moduleToOpen  = invalidModule;
        } else {
            libraryToOpen = lastLibraryName;
            moduleToOpen  = lastModuleName;
        }
    }

    if (libraryToOpen.isEmpty() || moduleManager.listModulesInLibrary(libraryToOpen).isEmpty()) {
        const QStringList loadedLibraries = moduleManager.listLoadedLibraries();
        if (!loadedLibraries.isEmpty())
            libraryToOpen = loadedLibraries.first();
    }

    if (!libraryToOpen.isEmpty() && moduleToOpen.isEmpty()) {
        const QStringList modules = moduleManager.listModulesInLibrary(libraryToOpen);
        if (!modules.isEmpty())
            moduleToOpen = modules.first();
    }

    if (!libraryToOpen.isEmpty() && !moduleToOpen.isEmpty()) {
        selectModule(libraryToOpen, moduleToOpen);
    } else {
        updateInspector();
        updateActions();
        setStatusText(tr("No modules in project"));
    }
}

void ModuleEditorWindow::selectModule(const QString &libraryName, const QString &moduleName)
{
    if (!checkSaveBeforeDiscard())
        return;

    const QModelIndex index = libraryModel->indexForModule(libraryName, moduleName);
    changingSelection       = true;
    libraryView->setCurrentIndex(index);
    changingSelection = false;

    currentLibraryName    = libraryName;
    currentModuleName     = moduleName;
    currentDefinitionBase = moduleManager.getModuleDefinition(libraryName, moduleName);
    portModel->setPorts(currentDefinitionBase.ports);
    parameterModel->setParameters(currentDefinitionBase.parameters);
    busInterfaceModel->setBusInterfaces(currentDefinitionBase.busInterfaces);

    currentInterfaceRow = -1;
    if (busInterfaceModel->rowCount() > 0) {
        const QModelIndex source = busInterfaceModel->index(0, 0);
        const QModelIndex proxy  = interfaceProxyModel->mapFromSource(source);
        changingSelection        = true;
        busInterfaceView->setCurrentIndex(proxy);
        changingSelection                      = false;
        currentInterfaceRow                    = 0;
        const QSocModuleBusInterface interface = busInterfaceModel->interfaceAt(0);
        busMappingModel
            ->setContext(interface, busDefinitionForName(interface.busName), portModel->ports());
        busMappingModel->rebuildRowsFromBusDefinition(false);
    } else {
        busMappingModel->clear();
    }

    updateInspector();
    updateActions();
    setStatusText(tr("Loaded %1/%2").arg(libraryName, moduleName));
}

void ModuleEditorWindow::clearCurrentModule()
{
    currentLibraryName.clear();
    currentModuleName.clear();
    currentDefinitionBase = {};
    currentInterfaceRow   = -1;
    portModel->clear();
    parameterModel->clear();
    busInterfaceModel->clear();
    busMappingModel->clear();
    updateInspector();
    updateActions();
}

QSocModuleDefinition ModuleEditorWindow::currentDefinitionFromModels() const
{
    QSocModuleDefinition definition = currentDefinitionBase;
    definition.libraryName          = currentLibraryName;
    definition.moduleName           = currentModuleName;
    definition.ports                = portModel->ports();
    definition.parameters           = parameterModel->parameters();
    definition.busInterfaces        = busInterfaceModel->busInterfaces();
    if (currentInterfaceRow >= 0 && currentInterfaceRow < definition.busInterfaces.size())
        definition.busInterfaces[currentInterfaceRow] = busMappingModel->interfaceDefinition();
    definition.isNullDefinition = currentDefinitionBase.isNullDefinition
                                  && definition.ports.isEmpty() && definition.parameters.isEmpty()
                                  && definition.busInterfaces.isEmpty();
    return definition;
}

QSocBusDefinition ModuleEditorWindow::busDefinitionForName(const QString &busName) const
{
    if (busName.isEmpty())
        return {};
    for (const QString &libraryName : busManager.listLoadedLibraries()) {
        if (busManager.listBusesInLibrary(libraryName).contains(busName))
            return busManager.getBusDefinition(libraryName, busName);
    }
    return {};
}

QStringList ModuleEditorWindow::loadedBusNames() const
{
    QStringList buses;
    for (const QString &libraryName : busManager.listLoadedLibraries()) {
        for (const QString &busName : busManager.listBusesInLibrary(libraryName)) {
            if (!buses.contains(busName))
                buses.append(busName);
        }
    }
    buses.sort(Qt::CaseInsensitive);
    return buses;
}

QStringList ModuleEditorWindow::modesForBus(const QString &busName) const
{
    QStringList             modes;
    const QSocBusDefinition definition = busDefinitionForName(busName);
    for (const QSocBusSignalMode &row : definition.rows) {
        if (!row.mode.isEmpty() && !modes.contains(row.mode))
            modes.append(row.mode);
    }
    modes.sort(Qt::CaseInsensitive);
    return modes;
}

QString ModuleEditorWindow::busLibraryPathForName(const QString &busName) const
{
    if (!projectManager || !projectManager->isValid())
        return {};
    if (busName.isEmpty())
        return {};
    for (const QString &libraryName : busManager.listLoadedLibraries()) {
        if (busManager.listBusesInLibrary(libraryName).contains(busName)) {
            return QDir(projectManager->getBusPath())
                .filePath(libraryName + QStringLiteral(".soc_bus"));
        }
    }
    return {};
}

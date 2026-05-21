// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "buseditorwindow.h"

#include <QFileInfo>

namespace {

bool hasErrorProblem(const QList<QSocBusProblem> &problems)
{
    for (const QSocBusProblem &problem : problems) {
        if (problem.severity == QSocBusProblemSeverity::Error)
            return true;
    }
    return false;
}

bool findFirstInvalidBus(QSocBusManager *manager, QString *libraryName, QString *busName)
{
    if (!manager)
        return false;

    for (const QString &candidateLibrary : manager->listLoadedLibraries()) {
        for (const QString &candidateBus : manager->listBusesInLibrary(candidateLibrary)) {
            const QSocBusDefinition definition
                = manager->getBusDefinition(candidateLibrary, candidateBus);
            if (!hasErrorProblem(manager->validateBusDefinition(definition)))
                continue;
            if (libraryName)
                *libraryName = candidateLibrary;
            if (busName)
                *busName = candidateBus;
            return true;
        }
    }
    return false;
}

} // namespace

void BusEditorWindow::setProjectManager(QSocProjectManager *projectManager)
{
    if (this->projectManager == projectManager)
        return;

    if (!checkSaveBeforeDiscard())
        return;

    this->projectManager = projectManager;
    busManager.setProjectManager(projectManager);
    reloadProject();
}

void BusEditorWindow::openFile(const QString &filePath)
{
    const QFileInfo fileInfo(filePath);
    const QString   libraryName = fileInfo.suffix() == QStringLiteral("soc_bus")
                                      ? fileInfo.completeBaseName()
                                      : QString();
    reloadProject(libraryName);
}

bool BusEditorWindow::hasWritableProject() const
{
    return projectManager && projectManager->isValid(true);
}

void BusEditorWindow::reloadProject(const QString &preferredLibrary, const QString &preferredBus)
{
    if (!checkSaveBeforeDiscard())
        return;

    const QString lastLibraryName = currentLibraryName;
    const QString lastBusName     = currentBusName;
    currentLibraryName.clear();
    currentBusName.clear();
    signalModeModel->clear();
    busManager.resetBusData();

    if (!projectManager || !projectManager->isValid()) {
        libraryModel->setBusManager(nullptr);
        updateInspector();
        updateActions();
        setStatusText(tr("No project loaded"));
        return;
    }

    busManager.setProjectManager(projectManager);
    const QStringList libraries = busManager.listLibrary();
    for (const QString &libraryName : libraries)
        busManager.load(libraryName);

    libraryModel->setBusManager(&busManager);
    libraryView->expandAll();

    QString libraryToOpen = preferredLibrary;
    QString busToOpen     = preferredBus;

    if (libraryToOpen.isEmpty() && busToOpen.isEmpty()) {
        QString invalidLibrary;
        QString invalidBus;
        if (findFirstInvalidBus(&busManager, &invalidLibrary, &invalidBus)) {
            libraryToOpen = invalidLibrary;
            busToOpen     = invalidBus;
        } else {
            libraryToOpen = lastLibraryName;
            busToOpen     = lastBusName;
        }
    }

    if (libraryToOpen.isEmpty() || busManager.listBusesInLibrary(libraryToOpen).isEmpty()) {
        const QStringList loadedLibraries = busManager.listLoadedLibraries();
        if (!loadedLibraries.isEmpty())
            libraryToOpen = loadedLibraries.first();
    }

    if (!libraryToOpen.isEmpty() && busToOpen.isEmpty()) {
        const QStringList buses = busManager.listBusesInLibrary(libraryToOpen);
        if (!buses.isEmpty())
            busToOpen = buses.first();
    }

    if (!libraryToOpen.isEmpty() && !busToOpen.isEmpty()) {
        selectBus(libraryToOpen, busToOpen);
    } else {
        updateInspector();
        updateActions();
        setStatusText(tr("No buses in project"));
    }
}

void BusEditorWindow::selectBus(const QString &libraryName, const QString &busName)
{
    if (!checkSaveBeforeDiscard())
        return;

    const QModelIndex index = libraryModel->indexForBus(libraryName, busName);
    changingSelection       = true;
    libraryView->setCurrentIndex(index);
    changingSelection = false;

    currentLibraryName = libraryName;
    currentBusName     = busName;
    signalModeModel->setDefinition(busManager.getBusDefinition(libraryName, busName));
    updateInspector();
    updateActions();
    setStatusText(tr("Loaded %1/%2").arg(libraryName, busName));
}

void BusEditorWindow::clearCurrentBus()
{
    currentLibraryName.clear();
    currentBusName.clear();
    signalModeModel->clear();
    updateInspector();
    updateActions();
}

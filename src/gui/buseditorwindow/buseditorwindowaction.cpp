// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "buseditorwindow.h"

#include <algorithm>
#include <QAbstractButton>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QMessageBox>
#include <QSet>

namespace {

enum class ReferenceRenameKind { Signal, Mode };

enum class ReferenceSaveChoice { Cancel, UpdateReferences, SaveBusOnly };

struct ReferenceRename
{
    ReferenceRenameKind kind = ReferenceRenameKind::Signal;
    QString             oldName;
    QString             newName;
};

bool hasErrorProblem(const QList<QSocBusProblem> &problems)
{
    for (const QSocBusProblem &problem : problems) {
        if (problem.severity == QSocBusProblemSeverity::Error)
            return true;
    }
    return false;
}

bool hasErrorProblem(const QList<QSocBusProblem> &problems, QStringList *messages)
{
    bool hasError = false;
    for (const QSocBusProblem &problem : problems) {
        if (problem.severity != QSocBusProblemSeverity::Error)
            continue;
        hasError = true;
        if (messages)
            messages->append(problem.message);
    }
    return hasError;
}

QString usagePath(const QSocBusUsage &usage)
{
    return QString("%1/%2/%3").arg(usage.moduleLibrary, usage.moduleName, usage.interfaceName);
}

QStringList uniqueSorted(QStringList values)
{
    values.removeDuplicates();
    values.sort(Qt::CaseInsensitive);
    return values;
}

QStringList usagePaths(const QList<QSocBusUsage> &usages)
{
    QStringList paths;
    for (const QSocBusUsage &usage : usages)
        paths.append(usagePath(usage));
    return uniqueSorted(paths);
}

bool busExistsInLoadedLibraries(const QSocBusManager &manager, const QString &busName)
{
    for (const QString &libraryName : manager.listLoadedLibraries()) {
        if (manager.listBusesInLibrary(libraryName).contains(busName))
            return true;
    }
    return false;
}

QString referenceRenameLabel(const ReferenceRename &rename)
{
    const QString kind = rename.kind == ReferenceRenameKind::Signal ? BusEditorWindow::tr("Signal")
                                                                    : BusEditorWindow::tr("Mode");
    return QString("%1: %2 -> %3").arg(kind, rename.oldName, rename.newName);
}

QSet<QString> definitionNames(
    const QSocBusDefinition &definition, const ReferenceRenameKind renameKind)
{
    QSet<QString> names;
    for (const QSocBusSignalMode &row : definition.rows) {
        const QString name = renameKind == ReferenceRenameKind::Signal ? row.signal : row.mode;
        if (!name.isEmpty())
            names.insert(name);
    }
    return names;
}

void appendSingleSetRename(
    QList<ReferenceRename>   *renames,
    const ReferenceRenameKind renameKind,
    const QSet<QString>      &oldNames,
    const QSet<QString>      &newNames)
{
    QSet<QString> removed = oldNames;
    removed.subtract(newNames);
    QSet<QString> added = newNames;
    added.subtract(oldNames);
    if (removed.size() != 1 || added.size() != 1)
        return;

    renames->append({renameKind, *removed.constBegin(), *added.constBegin()});
}

QList<ReferenceRename> detectReferenceRenames(
    const QSocBusDefinition &oldDefinition, const QSocBusDefinition &newDefinition)
{
    QList<ReferenceRename> renames;
    appendSingleSetRename(
        &renames,
        ReferenceRenameKind::Signal,
        definitionNames(oldDefinition, ReferenceRenameKind::Signal),
        definitionNames(newDefinition, ReferenceRenameKind::Signal));
    appendSingleSetRename(
        &renames,
        ReferenceRenameKind::Mode,
        definitionNames(oldDefinition, ReferenceRenameKind::Mode),
        definitionNames(newDefinition, ReferenceRenameKind::Mode));
    return renames;
}

bool referenceShapeChanged(
    const QSocBusDefinition &oldDefinition, const QSocBusDefinition &newDefinition)
{
    return definitionNames(oldDefinition, ReferenceRenameKind::Signal)
               != definitionNames(newDefinition, ReferenceRenameKind::Signal)
           || definitionNames(oldDefinition, ReferenceRenameKind::Mode)
                  != definitionNames(newDefinition, ReferenceRenameKind::Mode);
}

QStringList affectedReferencePaths(
    const QList<ReferenceRename> &renames, const QList<QSocBusUsage> &usages, QStringList *errors)
{
    QStringList paths;
    for (const ReferenceRename &rename : renames) {
        for (const QSocBusUsage &usage : usages) {
            if (rename.kind == ReferenceRenameKind::Signal) {
                if (!usage.mappingSignals.contains(rename.oldName))
                    continue;
                if (usage.mappingSignals.contains(rename.newName) && errors) {
                    errors->append(
                        BusEditorWindow::tr("Mapping already has signal %1: %2")
                            .arg(rename.newName, usagePath(usage)));
                }
                paths.append(usagePath(usage));
                continue;
            }
            if (usage.mode == rename.oldName)
                paths.append(usagePath(usage));
        }
    }
    return uniqueSorted(paths);
}

ReferenceSaveChoice askReferenceSaveChoice(
    QWidget *parent, const QList<ReferenceRename> &renames, const QStringList &affectedPaths)
{
    if (affectedPaths.isEmpty())
        return ReferenceSaveChoice::UpdateReferences;

    QStringList renameLines;
    for (const ReferenceRename &rename : renames)
        renameLines.append(referenceRenameLabel(rename));

    QMessageBox box(parent);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(BusEditorWindow::tr("Update References"));
    box.setText(BusEditorWindow::tr("Update module references for this bus edit?"));
    box.setInformativeText(BusEditorWindow::tr("Affected interfaces: %1").arg(affectedPaths.size()));
    box.setDetailedText(
        BusEditorWindow::tr("Renames:\n%1\n\nInterfaces:\n%2")
            .arg(renameLines.join('\n'), affectedPaths.join('\n')));
    box.setStandardButtons(QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
    box.setDefaultButton(QMessageBox::Yes);
    box.button(QMessageBox::Yes)->setText(BusEditorWindow::tr("Update References"));
    box.button(QMessageBox::No)->setText(BusEditorWindow::tr("Save Bus Only"));
    switch (box.exec()) {
    case QMessageBox::Yes:
        return ReferenceSaveChoice::UpdateReferences;
    case QMessageBox::No:
        return ReferenceSaveChoice::SaveBusOnly;
    default:
        return ReferenceSaveChoice::Cancel;
    }
}

bool applyReferenceRenames(
    QSocBusManager               *manager,
    const QString                &busName,
    const QList<ReferenceRename> &renames,
    QStringList                  *changedModules,
    QStringList                  *errors)
{
    QList<ReferenceRename> applied;
    for (const ReferenceRename &rename : renames) {
        bool ok = false;
        if (rename.kind == ReferenceRenameKind::Signal) {
            ok = manager->renameSignalReferences(
                busName, rename.oldName, rename.newName, changedModules, errors);
        } else {
            ok = manager->renameModeReferences(
                busName, rename.oldName, rename.newName, changedModules, errors);
        }
        if (ok) {
            applied.append(rename);
            continue;
        }

        for (auto it = applied.crbegin(); it != applied.crend(); ++it) {
            QStringList rollbackErrors;
            if (it->kind == ReferenceRenameKind::Signal) {
                manager->renameSignalReferences(
                    busName, it->newName, it->oldName, nullptr, &rollbackErrors);
            } else {
                manager->renameModeReferences(
                    busName, it->newName, it->oldName, nullptr, &rollbackErrors);
            }
            if (errors)
                errors->append(rollbackErrors);
        }
        return false;
    }
    if (changedModules)
        *changedModules = uniqueSorted(*changedModules);
    return true;
}

void showErrorList(QWidget *parent, const QString &title, const QStringList &errors)
{
    QMessageBox box(parent);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(title);
    box.setText(errors.isEmpty() ? title : errors.first());
    if (errors.size() > 1)
        box.setDetailedText(errors.join('\n'));
    box.exec();
}

} // namespace

void BusEditorWindow::handleLibrarySelectionChanged(
    const QModelIndex &current, const QModelIndex &previous)
{
    Q_UNUSED(previous)
    if (changingSelection)
        return;

    if (BusLibraryModel::nodeType(current) != BusLibraryModel::BusNode)
        return;

    const QString libraryName = BusLibraryModel::libraryName(current);
    const QString busName     = BusLibraryModel::busName(current);
    if (libraryName == currentLibraryName && busName == currentBusName)
        return;

    if (!checkSaveBeforeDiscard()) {
        changingSelection = true;
        libraryView->setCurrentIndex(libraryModel->indexForBus(currentLibraryName, currentBusName));
        changingSelection = false;
        return;
    }

    selectBus(libraryName, busName);
}

void BusEditorWindow::handleDirtyChanged(bool dirty)
{
    Q_UNUSED(dirty)
    updateActions();
    updateSummary();
}

void BusEditorWindow::handleNewLibrary()
{
    if (!hasWritableProject()) {
        QMessageBox::information(this, tr("No Project"), tr("Open a writable project first."));
        return;
    }

    bool          ok = false;
    const QString libraryName
        = QInputDialog::getText(
              this, tr("New Bus Library"), tr("Library name:"), QLineEdit::Normal, {}, &ok)
              .trimmed();
    if (!ok || libraryName.isEmpty())
        return;

    if (!createLibrary(libraryName)) {
        QMessageBox::warning(this, tr("Create Failed"), tr("Cannot create bus library."));
    }
}

bool BusEditorWindow::createLibrary(const QString &libraryName)
{
    if (!hasWritableProject())
        return false;

    const QString trimmedName = libraryName.trimmed();
    if (trimmedName.isEmpty())
        return false;

    if (!busManager.createLibrary(trimmedName))
        return false;

    libraryModel->setBusManager(&busManager);
    libraryView->expandAll();
    setStatusText(tr("Created library %1").arg(trimmedName));
    return true;
}

void BusEditorWindow::handleNewBus()
{
    if (!hasWritableProject()) {
        QMessageBox::information(this, tr("No Project"), tr("Open a writable project first."));
        return;
    }

    const QModelIndex current     = libraryView->currentIndex();
    QString           libraryName = BusLibraryModel::libraryName(current);
    if (BusLibraryModel::nodeType(current) == BusLibraryModel::RootNode)
        libraryName.clear();

    if (libraryName.isEmpty()) {
        const QStringList libraries = busManager.listLoadedLibraries();
        if (!libraries.isEmpty())
            libraryName = libraries.first();
    }

    if (libraryName.isEmpty()) {
        QMessageBox::information(this, tr("No Library"), tr("Create a bus library first."));
        return;
    }

    if (!checkSaveBeforeDiscard())
        return;

    bool          ok = false;
    const QString busName
        = QInputDialog::getText(this, tr("New Bus"), tr("Bus name:"), QLineEdit::Normal, {}, &ok)
              .trimmed();
    if (!ok || busName.isEmpty())
        return;
    if (busExistsInLoadedLibraries(busManager, busName)) {
        QMessageBox::warning(this, tr("Create Failed"), tr("Bus name already exists."));
        return;
    }

    QSocBusDefinition definition;
    definition.libraryName = libraryName;
    definition.busName     = busName;
    currentLibraryName     = libraryName;
    currentBusName         = busName;
    signalModeModel->setDefinition(definition);
    signalModeModel->setDirty(true);
    updateInspector();
    updateActions();
    setStatusText(tr("New unsaved bus %1/%2").arg(libraryName, busName));
}

void BusEditorWindow::handleDuplicateBus()
{
    if (currentLibraryName.isEmpty() || currentBusName.isEmpty() || !hasWritableProject())
        return;
    if (!checkSaveBeforeDiscard())
        return;

    bool          ok          = false;
    const QString defaultName = currentBusName + QStringLiteral("_copy");
    const QString newBusName
        = QInputDialog::getText(
              this, tr("Duplicate Bus"), tr("New bus name:"), QLineEdit::Normal, defaultName, &ok)
              .trimmed();
    if (!ok || newBusName.isEmpty())
        return;

    if (!duplicateCurrentBus(newBusName)) {
        QMessageBox::warning(this, tr("Duplicate Failed"), tr("Cannot duplicate bus."));
    }
}

bool BusEditorWindow::duplicateCurrentBus(const QString &newBusName)
{
    if (currentLibraryName.isEmpty() || currentBusName.isEmpty() || !hasWritableProject())
        return false;
    if (signalModeModel->isDirty())
        return false;

    const QString trimmedNew = newBusName.trimmed();
    if (trimmedNew.isEmpty() || trimmedNew == currentBusName)
        return false;
    if (busExistsInLoadedLibraries(busManager, trimmedNew))
        return false;
    if (!busManager.listBusesInLibrary(currentLibraryName).contains(currentBusName))
        return false;

    const QString     oldBusName = currentBusName;
    QSocBusDefinition definition = currentDefinitionFromModel();
    definition.busName           = trimmedNew;
    if (!busManager.replaceBusDefinition(definition))
        return false;

    reloadProject(currentLibraryName, trimmedNew);
    setStatusText(tr("Duplicated %1 as %2").arg(oldBusName, trimmedNew));
    return true;
}

void BusEditorWindow::handleRenameBus()
{
    if (currentLibraryName.isEmpty() || currentBusName.isEmpty() || !hasWritableProject())
        return;
    if (!checkSaveBeforeDiscard())
        return;

    bool          ok = false;
    const QString newBusName
        = QInputDialog::getText(
              this, tr("Rename Bus"), tr("Bus name:"), QLineEdit::Normal, currentBusName, &ok)
              .trimmed();
    if (!ok || newBusName.isEmpty() || newBusName == currentBusName)
        return;

    QStringList               scanErrors;
    const QList<QSocBusUsage> usages = busManager.scanBusUsages(currentBusName, &scanErrors);
    if (!scanErrors.isEmpty()) {
        showErrorList(this, tr("Reference Scan Failed"), scanErrors);
        return;
    }

    bool updateReferences = false;
    if (!usages.isEmpty()) {
        QMessageBox box(this);
        box.setIcon(QMessageBox::Question);
        box.setWindowTitle(tr("Update References"));
        box.setText(tr("Update module references from %1 to %2?").arg(currentBusName, newBusName));
        box.setInformativeText(tr("Affected interfaces: %1").arg(usages.size()));
        box.setDetailedText(usagePaths(usages).join('\n'));
        box.setStandardButtons(QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
        box.setDefaultButton(QMessageBox::Yes);
        box.button(QMessageBox::Yes)->setText(tr("Update References"));
        box.button(QMessageBox::No)->setText(tr("Rename Bus Only"));
        const int choice = box.exec();
        if (choice == QMessageBox::Cancel)
            return;
        updateReferences = choice == QMessageBox::Yes;
    }

    renameCurrentBus(newBusName, updateReferences);
}

bool BusEditorWindow::renameCurrentBus(const QString &newBusName, bool updateReferences)
{
    if (currentLibraryName.isEmpty() || currentBusName.isEmpty() || !hasWritableProject())
        return false;

    const QString oldLibrary = currentLibraryName;
    const QString oldBusName = currentBusName;
    const QString trimmedNew = newBusName.trimmed();
    if (trimmedNew.isEmpty())
        return false;
    if (trimmedNew == oldBusName)
        return true;

    if (!busManager.renameBusInLibrary(oldLibrary, oldBusName, trimmedNew)) {
        QMessageBox::warning(this, tr("Rename Failed"), tr("Cannot rename bus."));
        return false;
    }

    QString finalStatus;
    if (updateReferences) {
        QStringList changedModules;
        QStringList errors;
        if (!busManager.renameBusReferences(oldBusName, trimmedNew, &changedModules, &errors)) {
            QStringList rollbackErrors;
            busManager.renameBusReferences(trimmedNew, oldBusName, nullptr, &rollbackErrors);
            busManager.renameBusInLibrary(oldLibrary, trimmedNew, oldBusName);
            errors.append(rollbackErrors);
            showErrorList(this, tr("Reference Update Failed"), errors);
            reloadProject(oldLibrary, oldBusName);
            return false;
        }
        finalStatus = tr("Renamed %1 to %2 and updated %3 references")
                          .arg(oldBusName, trimmedNew, QString::number(changedModules.size()));
    } else {
        finalStatus = tr("Renamed %1 to %2").arg(oldBusName, trimmedNew);
    }

    reloadProject(oldLibrary, trimmedNew);
    setStatusText(finalStatus);
    return true;
}

void BusEditorWindow::handleAddRow()
{
    signalModeModel->addRow();
    const QModelIndex source = signalModeModel->index(signalModeModel->rowCount() - 1, 0);
    const QModelIndex proxy  = signalProxyModel->mapFromSource(source);
    if (proxy.isValid()) {
        tableView->setCurrentIndex(proxy);
        tableView->edit(proxy);
    }
}

void BusEditorWindow::handleDuplicateRows()
{
    signalModeModel->duplicateRows(selectedSourceRows());
}

void BusEditorWindow::handleDeleteRows()
{
    signalModeModel->removeRowIndices(selectedSourceRows());
}

void BusEditorWindow::handleDeleteBus()
{
    if (currentLibraryName.isEmpty() || currentBusName.isEmpty() || !hasWritableProject())
        return;
    if (!checkSaveBeforeDiscard())
        return;

    QStringList               scanErrors;
    const QList<QSocBusUsage> usages = busManager.scanBusUsages(currentBusName, &scanErrors);
    updateUsages(usages);
    if (!scanErrors.isEmpty() || !usages.isEmpty()) {
        QMessageBox::warning(
            this,
            tr("Bus In Use"),
            tr("This bus is referenced by modules. Remove those references before deleting it."));
        return;
    }

    const bool deletesLibraryFile = busManager.listBusesInLibrary(currentLibraryName).size() == 1;
    const QString questionText
        = deletesLibraryFile
              ? tr("Delete bus %1 and remove empty library %2?")
                    .arg(currentBusName, currentLibraryName)
              : tr("Delete bus %1 from library %2?").arg(currentBusName, currentLibraryName);
    if (QMessageBox::question(this, tr("Delete Bus"), questionText) != QMessageBox::Yes) {
        return;
    }

    const QString oldLibrary = currentLibraryName;
    if (!busManager.removeBusFromLibrary(currentLibraryName, currentBusName)) {
        QMessageBox::warning(this, tr("Delete Failed"), tr("Cannot delete bus."));
        return;
    }

    clearCurrentBus();
    reloadProject(oldLibrary);
}

void BusEditorWindow::handleDeleteLibrary()
{
    if (!hasWritableProject())
        return;

    QModelIndex current     = libraryView->currentIndex();
    QString     libraryName = BusLibraryModel::libraryName(current);
    if (libraryName.isEmpty())
        libraryName = currentLibraryName;
    if (libraryName.isEmpty())
        return;

    if (!busManager.listBusesInLibrary(libraryName).isEmpty()) {
        QMessageBox::warning(
            this, tr("Library Not Empty"), tr("Delete all buses before deleting this library."));
        return;
    }

    if (QMessageBox::question(
            this, tr("Delete Library"), tr("Delete empty library %1?").arg(libraryName))
        != QMessageBox::Yes) {
        return;
    }

    if (!deleteLibrary(libraryName)) {
        QMessageBox::warning(this, tr("Delete Failed"), tr("Cannot delete bus library."));
    }
}

bool BusEditorWindow::deleteLibrary(const QString &libraryName)
{
    if (!hasWritableProject())
        return false;

    const QString trimmedName = libraryName.trimmed();
    if (trimmedName.isEmpty())
        return false;
    if (!busManager.listLoadedLibraries().contains(trimmedName))
        return false;
    if (!busManager.listBusesInLibrary(trimmedName).isEmpty())
        return false;
    if (!busManager.removeLibraryIfEmpty(trimmedName))
        return false;

    if (currentLibraryName == trimmedName)
        clearCurrentBus();
    libraryModel->setBusManager(&busManager);
    libraryView->expandAll();
    setStatusText(tr("Deleted library %1").arg(trimmedName));
    return true;
}

void BusEditorWindow::handleSave()
{
    saveCurrentBus();
}

void BusEditorWindow::handleRevert()
{
    if (currentLibraryName.isEmpty() || currentBusName.isEmpty())
        return;

    const QString libraryName = currentLibraryName;
    const QString busName     = currentBusName;
    signalModeModel->setDirty(false);
    selectBus(libraryName, busName);
}

void BusEditorWindow::handleValidate()
{
    updateInspector();
    const QList<QSocBusProblem> problems = currentProblems();
    if (hasErrorProblem(problems)) {
        setStatusText(tr("Validation failed"));
    } else if (!problems.isEmpty()) {
        setStatusText(tr("Validation passed with warnings"));
    } else {
        setStatusText(tr("Validation passed"));
    }
}

void BusEditorWindow::handleRefresh()
{
    reloadProject(currentLibraryName, currentBusName);
}

void BusEditorWindow::handleSearchChanged(const QString &text)
{
    signalProxyModel->setFilterFixedString(text);
}

bool BusEditorWindow::checkSaveBeforeDiscard()
{
    if (!signalModeModel || !signalModeModel->isDirty())
        return true;

    const QMessageBox::StandardButton button = QMessageBox::question(
        this,
        tr("Unsaved Bus"),
        tr("Save changes to %1/%2?").arg(currentLibraryName, currentBusName),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
        QMessageBox::Save);

    if (button == QMessageBox::Save)
        return saveCurrentBus();
    if (button == QMessageBox::Discard) {
        signalModeModel->setDirty(false);
        return true;
    }
    return false;
}

bool BusEditorWindow::saveCurrentBus()
{
    if (currentLibraryName.isEmpty() || currentBusName.isEmpty() || !hasWritableProject())
        return false;

    const QSocBusDefinition definition = currentDefinitionFromModel();
    const QSocBusDefinition oldDefinition
        = busManager.getBusDefinition(currentLibraryName, currentBusName);

    QStringList                 problemMessages;
    const QList<QSocBusProblem> definitionProblems = busManager.validateBusDefinition(definition);
    updateProblems(definitionProblems, {});
    if (hasErrorProblem(definitionProblems, &problemMessages)) {
        showErrorList(this, tr("Save Blocked"), problemMessages);
        return false;
    }

    const QList<ReferenceRename> referenceRenames
        = detectReferenceRenames(oldDefinition, definition);
    QStringList               scanErrors;
    QStringList               referenceErrors;
    const QList<QSocBusUsage> usages = busManager.scanBusUsages(currentBusName, &scanErrors);
    if (!scanErrors.isEmpty()) {
        updateProblems(definitionProblems, scanErrors);
        if (referenceShapeChanged(oldDefinition, definition)) {
            showErrorList(this, tr("Reference Scan Failed"), scanErrors);
            return false;
        }
    }

    QStringList affectedReferences;
    if (scanErrors.isEmpty())
        affectedReferences = affectedReferencePaths(referenceRenames, usages, &referenceErrors);
    if (!referenceErrors.isEmpty()) {
        showErrorList(this, tr("Reference Update Blocked"), referenceErrors);
        return false;
    }

    ReferenceSaveChoice referenceChoice = ReferenceSaveChoice::UpdateReferences;
    if (!referenceRenames.isEmpty() && !affectedReferences.isEmpty()) {
        referenceChoice = askReferenceSaveChoice(this, referenceRenames, affectedReferences);
        if (referenceChoice == ReferenceSaveChoice::Cancel)
            return false;
    }

    const bool willUpdateReferences = referenceChoice == ReferenceSaveChoice::UpdateReferences
                                      && !referenceRenames.isEmpty()
                                      && !affectedReferences.isEmpty();
    if (!willUpdateReferences) {
        problemMessages.clear();
        QStringList           validationScanErrors;
        QList<QSocBusProblem> referenceProblems;
        if (scanErrors.isEmpty())
            referenceProblems = busManager.validateBusReferences(definition, &validationScanErrors);
        updateProblems(
            definitionProblems + referenceProblems,
            scanErrors.isEmpty() ? validationScanErrors : scanErrors);
        const bool allowBrokenReferences = referenceChoice == ReferenceSaveChoice::SaveBusOnly
                                           && !referenceRenames.isEmpty();
        if (!allowBrokenReferences
            && (hasErrorProblem(referenceProblems, &problemMessages)
                || !validationScanErrors.isEmpty())) {
            problemMessages.append(validationScanErrors);
            showErrorList(this, tr("Save Blocked"), problemMessages);
            return false;
        }
    }

    if (!busManager.replaceBusDefinition(definition)) {
        QMessageBox::warning(this, tr("Save Failed"), tr("Cannot write bus library."));
        return false;
    }

    QString finalStatus;
    if (willUpdateReferences) {
        QStringList changedModules;
        QStringList errors;
        if (!applyReferenceRenames(
                &busManager, definition.busName, referenceRenames, &changedModules, &errors)) {
            busManager.replaceBusDefinition(oldDefinition);
            showErrorList(this, tr("Reference Update Failed"), errors);
            reloadProject(oldDefinition.libraryName, oldDefinition.busName);
            return false;
        }
        finalStatus = tr("Saved %1/%2 and updated %3 references")
                          .arg(
                              definition.libraryName,
                              definition.busName,
                              QString::number(changedModules.size()));
    } else {
        finalStatus = tr("Saved %1/%2").arg(definition.libraryName, definition.busName);
    }

    signalModeModel->setDirty(false);
    reloadProject(definition.libraryName, definition.busName);
    setStatusText(finalStatus);
    return true;
}

QList<int> BusEditorWindow::selectedSourceRows() const
{
    QList<int> rows;
    if (!tableView || !tableView->selectionModel())
        return rows;

    QSet<int>             seen;
    const QModelIndexList selected = tableView->selectionModel()->selectedRows();
    for (const QModelIndex &proxyIndex : selected) {
        const QModelIndex sourceIndex = signalProxyModel->mapToSource(proxyIndex);
        if (!sourceIndex.isValid() || seen.contains(sourceIndex.row()))
            continue;
        seen.insert(sourceIndex.row());
        rows.append(sourceIndex.row());
    }
    std::sort(rows.begin(), rows.end());
    return rows;
}

QSocBusDefinition BusEditorWindow::currentDefinitionFromModel() const
{
    QSocBusDefinition definition = signalModeModel->definition();
    definition.libraryName       = currentLibraryName;
    definition.busName           = currentBusName;
    return definition;
}

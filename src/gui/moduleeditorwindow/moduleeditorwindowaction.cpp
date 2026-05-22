// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "moduleeditorwindow.h"

#include "common/qsocverilogutils.h"
#include "gui/buseditorwindow/buseditorwindow.h"

#include <algorithm>
#include <functional>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QSet>

namespace {

bool hasErrorProblem(const QList<QSocModuleProblem> &problems)
{
    for (const QSocModuleProblem &problem : problems) {
        if (problem.severity == QSocModuleProblemSeverity::Error)
            return true;
    }
    return false;
}

QString nextInterfaceName(const QList<QSocModuleBusInterface> &interfaces)
{
    for (int i = 0;; ++i) {
        const QString name = i == 0 ? QStringLiteral("if0") : QStringLiteral("if%1").arg(i);
        bool          used = false;
        for (const QSocModuleBusInterface &interface : interfaces) {
            if (interface.name == name) {
                used = true;
                break;
            }
        }
        if (!used)
            return name;
    }
}

bool interfaceNameExists(const QList<QSocModuleBusInterface> &interfaces, const QString &name)
{
    for (const QSocModuleBusInterface &interface : interfaces) {
        if (interface.name == name)
            return true;
    }
    return false;
}

bool moduleExistsInLibrary(
    const QSocModuleManager &manager, const QString &libraryName, const QString &moduleName)
{
    return manager.listModulesInLibrary(libraryName).contains(moduleName);
}

QStringList usagePaths(const QList<QSocModuleUsage> &usages)
{
    QStringList paths;
    for (const QSocModuleUsage &usage : usages) {
        paths.append(QString("%1/%2/%3").arg(usage.sourceType, usage.filePath, usage.instanceName));
    }
    paths.removeDuplicates();
    paths.sort(Qt::CaseInsensitive);
    return paths;
}

QStringList usagePathsForInterfaces(
    const QList<QSocModuleUsage> &usages, const QStringList &interfaceNames)
{
    QStringList paths;
    for (const QSocModuleUsage &usage : usages) {
        bool affected = false;
        for (const QString &interfaceName : interfaceNames) {
            if (usage.busInterfaces.contains(interfaceName)) {
                affected = true;
                break;
            }
        }
        if (affected)
            paths.append(
                QString("%1/%2/%3").arg(usage.sourceType, usage.filePath, usage.instanceName));
    }
    paths.removeDuplicates();
    paths.sort(Qt::CaseInsensitive);
    return paths;
}

QList<int> selectedSourceRows(const QTableView *view, const QSortFilterProxyModel *proxy)
{
    QList<int> rows;
    if (!view || !proxy)
        return rows;

    if (view->selectionModel()) {
        for (const QModelIndex &proxyIndex : view->selectionModel()->selectedRows()) {
            const QModelIndex source = proxy->mapToSource(proxyIndex);
            if (source.isValid())
                rows.append(source.row());
        }
    }

    if (rows.isEmpty() && view->currentIndex().isValid()) {
        const QModelIndex source = proxy->mapToSource(view->currentIndex());
        if (source.isValid())
            rows.append(source.row());
    }

    std::sort(rows.begin(), rows.end());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
    return rows;
}

bool hasPortNamed(const QList<QSocModulePort> &ports, const QString &name)
{
    for (const QSocModulePort &port : ports) {
        if (port.name == name)
            return true;
    }
    return false;
}

QStringList referencedPortNames(
    const QList<QSocModuleBusInterface> &interfaces, const QStringList &portNames)
{
    QSet<QString> referenced;
    for (const QSocModuleBusInterface &interface : interfaces) {
        for (const QSocModuleBusMapping &mapping : interface.mapping) {
            if (portNames.contains(mapping.modulePort))
                referenced.insert(mapping.modulePort);
        }
    }

    QStringList names = referenced.values();
    names.sort(Qt::CaseInsensitive);
    return names;
}

QString sanitizeIdentifier(QString name)
{
    name = name.trimmed();
    for (QChar &ch : name) {
        if (!ch.isLetterOrNumber() && ch != QLatin1Char('_'))
            ch = QLatin1Char('_');
    }
    if (name.isEmpty() || name.front().isDigit())
        name.prepend(QStringLiteral("p_"));
    return name;
}

QString generatedPortName(const QString &interfaceName, const QString &busSignal)
{
    const QString base = interfaceName.trimmed().isEmpty()
                             ? busSignal.trimmed()
                             : interfaceName.trimmed() + QStringLiteral("_") + busSignal.trimmed();
    return sanitizeIdentifier(base);
}

QString existingPortForSignal(
    const QList<QSocModulePort> &ports, const QString &interfaceName, const QString &busSignal)
{
    const QString exactName    = busSignal.trimmed();
    const QString prefixedName = generatedPortName(interfaceName, busSignal);
    for (const QSocModulePort &port : ports) {
        if (port.name == exactName || port.name == prefixedName)
            return port.name;
    }
    return {};
}

QString logicTypeForWidth(const QString &width)
{
    bool      ok    = false;
    const int value = width.trimmed().toInt(&ok);
    if (ok && value > 1)
        return QStringLiteral("logic[%1:0]").arg(value - 1);
    return QStringLiteral("logic");
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

QString uniqueInterfaceName(const QList<QSocModuleBusInterface> &interfaces, const QString &baseName)
{
    QString base = baseName.trimmed();
    if (base.isEmpty())
        base = QStringLiteral("if0");
    if (!interfaceNameExists(interfaces, base))
        return base;

    for (int suffix = 1;; ++suffix) {
        const QString candidate = QStringLiteral("%1_%2").arg(base, QString::number(suffix));
        if (!interfaceNameExists(interfaces, candidate))
            return candidate;
    }
}

struct InterfaceDialogResult
{
    QString name;
    QString busName;
    QString mode;
};

bool getInterfaceDialogResult(
    QWidget                                           *parent,
    const QString                                     &title,
    const InterfaceDialogResult                       &defaults,
    const QStringList                                 &busNames,
    const std::function<QStringList(const QString &)> &modeProvider,
    InterfaceDialogResult                             *result)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(title);

    auto *layout    = new QFormLayout(&dialog);
    auto *nameEdit  = new QLineEdit(defaults.name, &dialog);
    auto *busCombo  = new QComboBox(&dialog);
    auto *modeCombo = new QComboBox(&dialog);
    auto *buttons   = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, Qt::Horizontal, &dialog);

    busCombo->setEditable(true);
    busCombo->addItems(busNames);
    busCombo->setCurrentText(defaults.busName);
    modeCombo->setEditable(true);

    const auto updateModes = [&modeCombo, &modeProvider](const QString &busName) {
        const QString currentMode = modeCombo->currentText().trimmed();
        modeCombo->clear();
        modeCombo->addItems(modeProvider ? modeProvider(busName) : QStringList());
        if (!currentMode.isEmpty()) {
            modeCombo->setCurrentText(currentMode);
        } else if (modeCombo->count() == 0) {
            modeCombo->setCurrentText(QStringLiteral("master"));
        }
    };
    updateModes(defaults.busName);
    modeCombo->setCurrentText(defaults.mode);

    layout->addRow(ModuleEditorWindow::tr("Interface:"), nameEdit);
    layout->addRow(ModuleEditorWindow::tr("Bus:"), busCombo);
    layout->addRow(ModuleEditorWindow::tr("Mode:"), modeCombo);
    layout->addRow(buttons);

    QObject::connect(busCombo, &QComboBox::currentTextChanged, &dialog, updateModes);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() != QDialog::Accepted)
        return false;

    result->name    = nameEdit->text().trimmed();
    result->busName = busCombo->currentText().trimmed();
    result->mode    = modeCombo->currentText().trimmed();
    return true;
}

} // namespace

void ModuleEditorWindow::handleLibrarySelectionChanged(
    const QModelIndex &current, const QModelIndex &previous)
{
    Q_UNUSED(previous)
    if (changingSelection)
        return;

    if (ModuleLibraryModel::nodeType(current) != ModuleLibraryModel::ModuleNode)
        return;

    const QString libraryName = ModuleLibraryModel::libraryName(current);
    const QString moduleName  = ModuleLibraryModel::moduleName(current);
    if (libraryName == currentLibraryName && moduleName == currentModuleName)
        return;

    if (!checkSaveBeforeDiscard()) {
        changingSelection = true;
        libraryView->setCurrentIndex(
            libraryModel->indexForModule(currentLibraryName, currentModuleName));
        changingSelection = false;
        return;
    }

    selectModule(libraryName, moduleName);
}

void ModuleEditorWindow::handleInterfaceSelectionChanged(
    const QModelIndex &current, const QModelIndex &previous)
{
    if (changingSelection)
        return;

    const QModelIndex previousSource = interfaceProxyModel->mapToSource(previous);
    if (previousSource.isValid() && previousSource.row() == currentInterfaceRow)
        syncMappingToInterfaceModel();

    const QModelIndex source = interfaceProxyModel->mapToSource(current);
    currentInterfaceRow      = source.isValid() ? source.row() : -1;
    if (currentInterfaceRow < 0) {
        busMappingModel->clear();
        updateActions();
        return;
    }

    refreshMappingContext(false);
    updateInspector();
    updateActions();
}

void ModuleEditorWindow::handleInterfaceDataChanged(
    const QModelIndex &topLeft, const QModelIndex &bottomRight, const QList<int> &roles)
{
    Q_UNUSED(roles)
    if (syncingInterfaceRow || !topLeft.isValid() || !bottomRight.isValid())
        return;

    const bool touchedBusOrMode = topLeft.column() <= ModuleBusInterfaceModel::ModeColumn
                                  && bottomRight.column() >= ModuleBusInterfaceModel::BusColumn;
    for (int row = topLeft.row(); touchedBusOrMode && row <= bottomRight.row(); ++row)
        rebuildInterfaceMapping(row, true);

    if (currentInterfaceRow >= topLeft.row() && currentInterfaceRow <= bottomRight.row())
        refreshMappingContext(false);

    updateInspector();
    updateActions();
}

void ModuleEditorWindow::handleDirtyChanged(bool dirty)
{
    Q_UNUSED(dirty)
    updateActions();
    updateSummary();
}

void ModuleEditorWindow::handleNewLibrary()
{
    if (!hasWritableProject()) {
        QMessageBox::information(this, tr("No Project"), tr("Open a writable project first."));
        return;
    }

    bool          ok = false;
    const QString libraryName
        = QInputDialog::getText(
              this, tr("New Module Library"), tr("Library name:"), QLineEdit::Normal, {}, &ok)
              .trimmed();
    if (!ok || libraryName.isEmpty())
        return;

    if (!createLibrary(libraryName))
        QMessageBox::warning(this, tr("Create Failed"), tr("Cannot create module library."));
}

bool ModuleEditorWindow::createLibrary(const QString &libraryName)
{
    if (!hasWritableProject())
        return false;

    const QString trimmedName = libraryName.trimmed();
    if (trimmedName.isEmpty())
        return false;
    if (!moduleManager.createLibrary(trimmedName))
        return false;

    libraryModel->setModuleManager(&moduleManager);
    libraryView->expandAll();
    setStatusText(tr("Created library %1").arg(trimmedName));
    updateActions();
    return true;
}

void ModuleEditorWindow::handleNewModule()
{
    if (!hasWritableProject()) {
        QMessageBox::information(this, tr("No Project"), tr("Open a writable project first."));
        return;
    }

    QModelIndex current     = libraryView->currentIndex();
    QString     libraryName = ModuleLibraryModel::libraryName(current);
    if (ModuleLibraryModel::nodeType(current) == ModuleLibraryModel::RootNode)
        libraryName.clear();
    if (libraryName.isEmpty())
        libraryName = currentLibraryName;
    if (libraryName.isEmpty()) {
        const QStringList libraries = moduleManager.listLoadedLibraries();
        if (!libraries.isEmpty())
            libraryName = libraries.first();
    }
    if (libraryName.isEmpty()) {
        QMessageBox::information(this, tr("No Library"), tr("Create a module library first."));
        return;
    }

    if (!checkSaveBeforeDiscard())
        return;

    bool          ok = false;
    const QString moduleName
        = QInputDialog::getText(this, tr("New Module"), tr("Module name:"), QLineEdit::Normal, {}, &ok)
              .trimmed();
    if (!ok || moduleName.isEmpty())
        return;

    if (!createModule(libraryName, moduleName))
        QMessageBox::warning(this, tr("Create Failed"), tr("Cannot create module."));
}

bool ModuleEditorWindow::createModule(const QString &libraryName, const QString &moduleName)
{
    if (!hasWritableProject() || isDirty())
        return false;

    const QString trimmedLibrary = libraryName.trimmed();
    const QString trimmedModule  = moduleName.trimmed();
    if (trimmedLibrary.isEmpty() || trimmedModule.isEmpty())
        return false;
    if (!moduleManager.listLoadedLibraries().contains(trimmedLibrary))
        return false;
    if (!QSocVerilogUtils::isValidVerilogIdentifier(trimmedModule))
        return false;
    if (moduleExistsInLibrary(moduleManager, trimmedLibrary, trimmedModule))
        return false;

    changingSelection = true;
    libraryView->setCurrentIndex(libraryModel->indexForModule(trimmedLibrary, {}));
    changingSelection = false;

    currentLibraryName                     = trimmedLibrary;
    currentModuleName                      = trimmedModule;
    currentDefinitionBase                  = {};
    currentDefinitionBase.libraryName      = trimmedLibrary;
    currentDefinitionBase.moduleName       = trimmedModule;
    currentDefinitionBase.isNullDefinition = true;
    currentInterfaceRow                    = -1;
    portModel->clear();
    parameterModel->clear();
    busInterfaceModel->clear();
    busMappingModel->clear();
    setModelsDirty(true);
    updateInspector();
    updateActions();
    setStatusText(tr("New unsaved module %1/%2").arg(trimmedLibrary, trimmedModule));
    return true;
}

void ModuleEditorWindow::handleDuplicateModule()
{
    if (currentLibraryName.isEmpty() || currentModuleName.isEmpty() || !hasWritableProject())
        return;
    if (!checkSaveBeforeDiscard())
        return;

    bool          ok            = false;
    const QString defaultName   = currentModuleName + QStringLiteral("_copy");
    const QString newModuleName = QInputDialog::getText(
                                      this,
                                      tr("Duplicate Module"),
                                      tr("New module name:"),
                                      QLineEdit::Normal,
                                      defaultName,
                                      &ok)
                                      .trimmed();
    if (!ok || newModuleName.isEmpty())
        return;

    if (!duplicateCurrentModule(newModuleName))
        QMessageBox::warning(this, tr("Duplicate Failed"), tr("Cannot duplicate module."));
}

bool ModuleEditorWindow::duplicateCurrentModule(const QString &newModuleName)
{
    if (currentLibraryName.isEmpty() || currentModuleName.isEmpty() || !hasWritableProject())
        return false;
    if (isDirty())
        return false;

    const QString trimmedNew = newModuleName.trimmed();
    if (trimmedNew.isEmpty() || trimmedNew == currentModuleName)
        return false;
    if (!QSocVerilogUtils::isValidVerilogIdentifier(trimmedNew))
        return false;
    if (moduleExistsInLibrary(moduleManager, currentLibraryName, trimmedNew))
        return false;

    const QString        oldModuleName = currentModuleName;
    QSocModuleDefinition definition    = currentDefinitionFromModels();
    definition.moduleName              = trimmedNew;
    if (!moduleManager.replaceModuleDefinition(definition))
        return false;

    reloadProject(currentLibraryName, trimmedNew);
    setStatusText(tr("Duplicated %1 as %2").arg(oldModuleName, trimmedNew));
    return true;
}

void ModuleEditorWindow::handleRenameModule()
{
    if (currentLibraryName.isEmpty() || currentModuleName.isEmpty() || !hasWritableProject())
        return;
    if (!checkSaveBeforeDiscard())
        return;

    bool          ok            = false;
    const QString newModuleName = QInputDialog::getText(
                                      this,
                                      tr("Rename Module"),
                                      tr("Module name:"),
                                      QLineEdit::Normal,
                                      currentModuleName,
                                      &ok)
                                      .trimmed();
    if (!ok || newModuleName.isEmpty() || newModuleName == currentModuleName)
        return;

    QStringList                  scanErrors;
    const QList<QSocModuleUsage> usages
        = moduleManager.scanModuleUsages(currentModuleName, &scanErrors);
    updateUsages(usages, scanErrors);
    if (!scanErrors.isEmpty()) {
        showErrorList(this, tr("Reference Scan Failed"), scanErrors);
        return;
    }
    if (!usages.isEmpty()) {
        QMessageBox box(this);
        box.setIcon(QMessageBox::Question);
        box.setWindowTitle(tr("Rename Module"));
        box.setText(tr("Rename the library module definition only?"));
        box.setInformativeText(
            tr("Generated netlists and placed schematic copies are not updated automatically."));
        box.setDetailedText(usagePaths(usages).join('\n'));
        box.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
        box.setDefaultButton(QMessageBox::No);
        if (box.exec() != QMessageBox::Yes)
            return;
    }

    if (!renameCurrentModule(newModuleName))
        QMessageBox::warning(this, tr("Rename Failed"), tr("Cannot rename module."));
}

bool ModuleEditorWindow::renameCurrentModule(const QString &newModuleName)
{
    if (currentLibraryName.isEmpty() || currentModuleName.isEmpty() || !hasWritableProject())
        return false;
    if (isDirty())
        return false;

    const QString oldLibrary    = currentLibraryName;
    const QString oldModuleName = currentModuleName;
    const QString trimmedNew    = newModuleName.trimmed();
    if (trimmedNew.isEmpty())
        return false;
    if (trimmedNew == oldModuleName)
        return true;
    if (!QSocVerilogUtils::isValidVerilogIdentifier(trimmedNew))
        return false;
    if (moduleExistsInLibrary(moduleManager, oldLibrary, trimmedNew))
        return false;

    if (!moduleManager.renameModuleInLibrary(oldLibrary, oldModuleName, trimmedNew))
        return false;

    reloadProject(oldLibrary, trimmedNew);
    setStatusText(tr("Renamed %1 to %2").arg(oldModuleName, trimmedNew));
    return true;
}

void ModuleEditorWindow::handleDeleteModule()
{
    if (currentLibraryName.isEmpty() || currentModuleName.isEmpty() || !hasWritableProject())
        return;
    if (!checkSaveBeforeDiscard())
        return;

    QStringList                  scanErrors;
    const QList<QSocModuleUsage> usages
        = moduleManager.scanModuleUsages(currentModuleName, &scanErrors);
    updateUsages(usages, scanErrors);
    if (!scanErrors.isEmpty() || !usages.isEmpty()) {
        QMessageBox::warning(
            this,
            tr("Module In Use"),
            tr("This module is referenced by project outputs or schematics. Remove those "
               "references before deleting it."));
        return;
    }

    const bool    deletesLibraryFile = moduleManager.listModulesInLibrary(currentLibraryName).size()
                                       == 1;
    const QString questionText
        = deletesLibraryFile
              ? tr("Delete module %1 and remove empty library %2?")
                    .arg(currentModuleName, currentLibraryName)
              : tr("Delete module %1 from library %2?").arg(currentModuleName, currentLibraryName);
    if (QMessageBox::question(this, tr("Delete Module"), questionText) != QMessageBox::Yes)
        return;

    if (!deleteCurrentModule())
        QMessageBox::warning(this, tr("Delete Failed"), tr("Cannot delete module."));
}

bool ModuleEditorWindow::deleteCurrentModule()
{
    if (currentLibraryName.isEmpty() || currentModuleName.isEmpty() || !hasWritableProject())
        return false;
    if (isDirty())
        return false;

    QStringList                  scanErrors;
    const QList<QSocModuleUsage> usages
        = moduleManager.scanModuleUsages(currentModuleName, &scanErrors);
    if (!scanErrors.isEmpty() || !usages.isEmpty())
        return false;

    const QString oldLibrary    = currentLibraryName;
    const QString oldModuleName = currentModuleName;
    if (!moduleManager.removeModuleFromLibrary(oldLibrary, oldModuleName))
        return false;

    clearCurrentModule();
    reloadProject(oldLibrary);
    setStatusText(tr("Deleted module %1").arg(oldModuleName));
    return true;
}

void ModuleEditorWindow::handleDeleteLibrary()
{
    if (!hasWritableProject())
        return;

    QModelIndex current     = libraryView->currentIndex();
    QString     libraryName = ModuleLibraryModel::libraryName(current);
    if (libraryName.isEmpty())
        libraryName = currentLibraryName;
    if (libraryName.isEmpty())
        return;

    if (!moduleManager.listModulesInLibrary(libraryName).isEmpty()) {
        QMessageBox::warning(
            this, tr("Library Not Empty"), tr("Delete all modules before deleting this library."));
        return;
    }

    if (QMessageBox::question(
            this, tr("Delete Library"), tr("Delete empty library %1?").arg(libraryName))
        != QMessageBox::Yes) {
        return;
    }

    if (!deleteLibrary(libraryName))
        QMessageBox::warning(this, tr("Delete Failed"), tr("Cannot delete module library."));
}

bool ModuleEditorWindow::deleteLibrary(const QString &libraryName)
{
    if (!hasWritableProject())
        return false;

    const QString trimmedName = libraryName.trimmed();
    if (trimmedName.isEmpty())
        return false;
    if (!moduleManager.listLoadedLibraries().contains(trimmedName))
        return false;
    if (!moduleManager.listModulesInLibrary(trimmedName).isEmpty())
        return false;
    if (!moduleManager.removeLibraryIfEmpty(trimmedName))
        return false;

    if (currentLibraryName == trimmedName)
        clearCurrentModule();
    libraryModel->setModuleManager(&moduleManager);
    libraryView->expandAll();
    setStatusText(tr("Deleted library %1").arg(trimmedName));
    updateActions();
    return true;
}

void ModuleEditorWindow::handleImportVerilog()
{
    if (!hasWritableProject() || isDirty())
        return;

    const QStringList filePaths = QFileDialog::getOpenFileNames(
        this,
        tr("Import Verilog"),
        QString(),
        tr("Verilog/SystemVerilog (*.v *.sv *.vh *.svh);;All Files (*)"));
    if (filePaths.isEmpty())
        return;

    if (!importVerilogFiles(filePaths, currentLibraryName))
        QMessageBox::warning(this, tr("Import Failed"), tr("Cannot import Verilog module."));
}

bool ModuleEditorWindow::importVerilogFiles(
    const QStringList &filePaths, const QString &libraryName, const QString &moduleRegex)
{
    if (!hasWritableProject() || isDirty())
        return false;

    QStringList files;
    for (const QString &filePath : filePaths) {
        const QString trimmed = filePath.trimmed();
        if (!trimmed.isEmpty())
            files.append(trimmed);
    }
    if (files.isEmpty())
        return false;

    const QRegularExpression regex(moduleRegex);
    if (!moduleManager.importFromFileList(libraryName.trimmed(), regex, QString(), files))
        return false;

    reloadProject(libraryName.trimmed());
    setStatusText(tr("Imported %1 Verilog file(s)").arg(files.size()));
    return true;
}

void ModuleEditorWindow::handleAddRow()
{
    if (currentLibraryName.isEmpty() || currentModuleName.isEmpty())
        return;

    QWidget *current = editorTabs->currentWidget();
    if (current == portView) {
        portModel->addRow();
        const QModelIndex proxy = portProxyModel->mapFromSource(
            portModel->index(portModel->rowCount() - 1, ModulePortModel::NameColumn));
        portView->setCurrentIndex(proxy);
        portView->edit(proxy);
    } else if (current == parameterView) {
        parameterModel->addRow();
        const QModelIndex proxy = parameterProxyModel->mapFromSource(
            parameterModel->index(parameterModel->rowCount() - 1, ModuleParameterModel::NameColumn));
        parameterView->setCurrentIndex(proxy);
        parameterView->edit(proxy);
    } else if (current == busInterfaceView) {
        handleAddInterface();
        return;
    } else if (current == busMappingView) {
        if (currentInterfaceRow < 0)
            return;
        busMappingModel->addRow();
        syncMappingToInterfaceModel();
        const QModelIndex proxy = mappingProxyModel->mapFromSource(
            busMappingModel
                ->index(busMappingModel->rowCount() - 1, ModuleBusMappingModel::BusSignalColumn));
        busMappingView->setCurrentIndex(proxy);
        busMappingView->edit(proxy);
    }
    updateInspector();
}

void ModuleEditorWindow::handleDuplicateRow()
{
    if (currentLibraryName.isEmpty() || currentModuleName.isEmpty())
        return;

    QWidget *current = editorTabs->currentWidget();
    if (current == portView) {
        const QList<int> rows = selectedSourceRows(portView, portProxyModel);
        syncMappingToInterfaceModel();
        portModel->duplicateRows(rows);
        refreshMappingContext(false);
    } else if (current == parameterView) {
        parameterModel->duplicateRows(selectedSourceRows(parameterView, parameterProxyModel));
    } else if (current == busInterfaceView) {
        handleDuplicateInterface();
        return;
    } else if (current == busMappingView) {
        if (currentInterfaceRow < 0)
            return;
        busMappingModel->duplicateRows(selectedSourceRows(busMappingView, mappingProxyModel));
        syncMappingToInterfaceModel();
    }
    updateInspector();
}

void ModuleEditorWindow::handleDeleteRow()
{
    if (currentLibraryName.isEmpty() || currentModuleName.isEmpty())
        return;

    QWidget *current = editorTabs->currentWidget();
    if (current == portView) {
        const QList<int> rows = selectedSourceRows(portView, portProxyModel);
        if (rows.isEmpty())
            return;

        QStringList portNames;
        for (int row : rows) {
            const QString name
                = portModel->index(row, ModulePortModel::NameColumn).data().toString();
            if (!name.isEmpty())
                portNames.append(name);
        }

        const QStringList referenced
            = referencedPortNames(busInterfaceModel->busInterfaces(), portNames);
        if (!referenced.isEmpty()) {
            QMessageBox box(this);
            box.setIcon(QMessageBox::Warning);
            box.setWindowTitle(tr("Delete Ports"));
            box.setText(tr("Delete ports that are still referenced by bus mappings?"));
            box.setDetailedText(referenced.join('\n'));
            box.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
            box.setDefaultButton(QMessageBox::No);
            if (box.exec() != QMessageBox::Yes)
                return;
        }

        syncMappingToInterfaceModel();
        portModel->removeRowIndices(rows);
        refreshMappingContext(false);
    } else if (current == parameterView) {
        parameterModel->removeRowIndices(selectedSourceRows(parameterView, parameterProxyModel));
    } else if (current == busInterfaceView) {
        handleDeleteInterface();
        return;
    } else if (current == busMappingView) {
        if (currentInterfaceRow < 0)
            return;
        busMappingModel->removeRowIndices(selectedSourceRows(busMappingView, mappingProxyModel));
        syncMappingToInterfaceModel();
    }
    updateInspector();
}

void ModuleEditorWindow::handleAddInterface()
{
    if (currentLibraryName.isEmpty() || currentModuleName.isEmpty())
        return;

    InterfaceDialogResult defaults;
    defaults.name    = nextInterfaceName(busInterfaceModel->busInterfaces());
    defaults.busName = loadedBusNames().value(0);
    defaults.mode    = modesForBus(defaults.busName).value(0, QStringLiteral("master"));

    InterfaceDialogResult result;
    if (!getInterfaceDialogResult(
            this,
            tr("Add Bus Interface"),
            defaults,
            loadedBusNames(),
            [this](const QString &busName) { return modesForBus(busName); },
            &result)) {
        return;
    }
    if (result.name.isEmpty())
        return;
    if (interfaceNameExists(busInterfaceModel->busInterfaces(), result.name)) {
        QMessageBox::warning(
            this, tr("Duplicate Interface"), tr("A bus interface with that name already exists."));
        return;
    }

    QSocModuleBusInterface interface;
    interface.name    = result.name;
    interface.busName = result.busName;
    interface.mode    = result.mode;

    ModuleBusMappingModel mappingBuilder;
    mappingBuilder.setContext(interface, busDefinitionForName(interface.busName), portModel->ports());
    mappingBuilder.rebuildRowsFromBusDefinition(false);
    mappingBuilder.autoMatchByInterfacePrefix();
    interface = mappingBuilder.interfaceDefinition();

    busInterfaceModel->addInterface(interface);
    const QModelIndex source = busInterfaceModel->index(busInterfaceModel->rowCount() - 1, 0);
    const QModelIndex proxy  = interfaceProxyModel->mapFromSource(source);
    if (proxy.isValid()) {
        busInterfaceView->setCurrentIndex(proxy);
    }
    updateInspector();
}

void ModuleEditorWindow::handleDuplicateInterface()
{
    if (currentInterfaceRow < 0)
        return;

    QSocModuleBusInterface duplicate = busInterfaceModel->interfaceAt(currentInterfaceRow);
    duplicate.name                   = uniqueInterfaceName(
        busInterfaceModel->busInterfaces(), duplicate.name + QStringLiteral("_copy"));
    busInterfaceModel->addInterface(duplicate);
    selectInterfaceRow(busInterfaceModel->rowCount() - 1);
}

void ModuleEditorWindow::handleRenameInterface()
{
    if (currentInterfaceRow < 0)
        return;

    const QSocModuleBusInterface interface = busInterfaceModel->interfaceAt(currentInterfaceRow);
    bool                         ok        = false;
    const QString                newName   = QInputDialog::getText(
                                                 this,
                                                 tr("Rename Bus Interface"),
                                                 tr("Interface name:"),
                                                 QLineEdit::Normal,
                                                 interface.name,
                                                 &ok)
                                                 .trimmed();
    if (!ok || newName.isEmpty() || newName == interface.name)
        return;
    if (interfaceNameExists(busInterfaceModel->busInterfaces(), newName)) {
        QMessageBox::warning(
            this, tr("Duplicate Interface"), tr("A bus interface with that name already exists."));
        return;
    }

    QStringList                  scanErrors;
    const QList<QSocModuleUsage> usages
        = moduleManager.scanModuleUsages(currentModuleName, &scanErrors);
    updateUsages(usages, scanErrors);
    if (!scanErrors.isEmpty()) {
        showErrorList(this, tr("Reference Scan Failed"), scanErrors);
        return;
    }
    const QStringList affected = usagePathsForInterfaces(usages, {interface.name});
    if (!affected.isEmpty()) {
        QMessageBox box(this);
        box.setIcon(QMessageBox::Question);
        box.setWindowTitle(tr("Rename Bus Interface"));
        box.setText(tr("Rename the library bus interface definition only?"));
        box.setInformativeText(
            tr("Generated netlists and placed schematic copies are not updated automatically."));
        box.setDetailedText(affected.join('\n'));
        box.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
        box.setDefaultButton(QMessageBox::No);
        if (box.exec() != QMessageBox::Yes)
            return;
    }

    busInterfaceModel->setData(
        busInterfaceModel->index(currentInterfaceRow, ModuleBusInterfaceModel::InterfaceColumn),
        newName);
}

void ModuleEditorWindow::handleDeleteInterface()
{
    const QList<int> rows = selectedInterfaceRows();
    if (rows.isEmpty())
        return;

    QStringList interfaceNames;
    for (int row : rows) {
        const QString name = busInterfaceModel->interfaceAt(row).name;
        if (!name.isEmpty())
            interfaceNames.append(name);
    }

    QStringList                  scanErrors;
    const QList<QSocModuleUsage> usages
        = moduleManager.scanModuleUsages(currentModuleName, &scanErrors);
    updateUsages(usages, scanErrors);
    if (!scanErrors.isEmpty()) {
        showErrorList(this, tr("Reference Scan Failed"), scanErrors);
        return;
    }

    QMessageBox box(this);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(tr("Delete Bus Interface"));
    box.setText(tr("Delete %1 bus interface(s) from this module?").arg(rows.size()));
    const QStringList affected = usagePathsForInterfaces(usages, interfaceNames);
    if (!affected.isEmpty()) {
        box.setInformativeText(
            tr("Generated netlists and placed schematic copies are not updated automatically."));
        box.setDetailedText(affected.join('\n'));
    }
    box.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
    box.setDefaultButton(QMessageBox::No);
    if (box.exec() != QMessageBox::Yes)
        return;

    const int nextRow = rows.first() > 0 ? rows.first() - 1 : 0;
    busInterfaceModel->removeRowIndices(rows);
    if (busInterfaceModel->rowCount() > 0) {
        selectInterfaceRow(qMin(nextRow, busInterfaceModel->rowCount() - 1));
    } else {
        currentInterfaceRow = -1;
        busMappingModel->clear();
    }
    updateInspector();
}

void ModuleEditorWindow::handleAutoMatch()
{
    handleAutoMatchByPrefix();
}

void ModuleEditorWindow::handleAutoMatchByName()
{
    if (currentInterfaceRow < 0)
        return;
    busMappingModel->autoMatchByName();
    syncMappingToInterfaceModel();
    updateInspector();
}

void ModuleEditorWindow::handleAutoMatchByPrefix()
{
    if (currentInterfaceRow < 0)
        return;
    busMappingModel->autoMatchByInterfacePrefix();
    syncMappingToInterfaceModel();
    updateInspector();
}

void ModuleEditorWindow::handleClearMissingMappings()
{
    if (currentInterfaceRow < 0)
        return;
    const int oldCount = busMappingModel->rowCount();
    busMappingModel->removeUnknownBusSignalRows();
    syncMappingToInterfaceModel();
    updateInspector();
    setStatusText(tr("Removed %1 stale mapping row(s)").arg(oldCount - busMappingModel->rowCount()));
}

void ModuleEditorWindow::handleCreateMissingPorts()
{
    if (currentInterfaceRow < 0)
        return;

    int                   created   = 0;
    int                   assigned  = 0;
    QList<QSocModulePort> ports     = portModel->ports();
    const auto            interface = busMappingModel->interfaceDefinition();
    for (int row = 0; row < busMappingModel->rowCount(); ++row) {
        if (busMappingModel->rowHasUnknownBusSignal(row))
            continue;

        const QString busSignal = busMappingModel->index(row, ModuleBusMappingModel::BusSignalColumn)
                                      .data()
                                      .toString()
                                      .trimmed();
        if (busSignal.isEmpty())
            continue;

        QString    portName = busMappingModel->index(row, ModuleBusMappingModel::ModulePortColumn)
                                  .data()
                                  .toString()
                                  .trimmed();
        const bool hadPortName = !portName.isEmpty();
        if (!hadPortName) {
            portName = existingPortForSignal(ports, interface.name, busSignal);
            if (portName.isEmpty())
                portName = generatedPortName(interface.name, busSignal);
        }
        if (!QSocVerilogUtils::isValidVerilogIdentifier(portName))
            continue;

        if (!hasPortNamed(ports, portName)) {
            QSocModulePort port;
            port.name      = portName;
            port.direction = busMappingModel
                                 ->index(row, ModuleBusMappingModel::ExpectedDirectionColumn)
                                 .data()
                                 .toString();
            port.type      = logicTypeForWidth(
                busMappingModel->index(row, ModuleBusMappingModel::ExpectedWidthColumn)
                    .data()
                    .toString());

            const int portRow = portModel->rowCount();
            portModel->addRow();
            portModel->setData(portModel->index(portRow, ModulePortModel::NameColumn), port.name);
            portModel->setData(
                portModel->index(portRow, ModulePortModel::DirectionColumn), port.direction);
            portModel->setData(portModel->index(portRow, ModulePortModel::TypeColumn), port.type);
            ports.append(port);
            ++created;
        }

        if (!hadPortName
            && busMappingModel->setData(
                busMappingModel->index(row, ModuleBusMappingModel::ModulePortColumn), portName)) {
            ++assigned;
        }
    }

    syncMappingToInterfaceModel();
    refreshMappingContext(false);
    updateInspector();
    setStatusText(tr("Created %1 port(s), assigned %2 mapping(s)").arg(created).arg(assigned));
}

void ModuleEditorWindow::handleShowOnlyProblems(bool enabled)
{
    mappingProxyModel->setShowOnlyProblems(enabled);
}

void ModuleEditorWindow::handleShowEmptyMappings(bool enabled)
{
    mappingProxyModel->setShowEmptyMappings(enabled);
}

void ModuleEditorWindow::handleOpenBusEditor()
{
    if (!projectManager || !projectManager->isValid())
        return;

    const QString busName  = selectedInterface().busName;
    const QString filePath = busLibraryPathForName(busName);
    if (!busEditorWindow)
        busEditorWindow = new BusEditorWindow(this, projectManager);
    busEditorWindow->setProjectManager(projectManager);
    if (!filePath.isEmpty())
        busEditorWindow->openFile(filePath);
    busEditorWindow->show();
    busEditorWindow->raise();
    busEditorWindow->activateWindow();
}

void ModuleEditorWindow::handleSave()
{
    saveCurrentModule();
}

void ModuleEditorWindow::handleRevert()
{
    if (currentLibraryName.isEmpty() || currentModuleName.isEmpty())
        return;

    const QString libraryName = currentLibraryName;
    const QString moduleName  = currentModuleName;
    setModelsDirty(false);
    selectModule(libraryName, moduleName);
}

void ModuleEditorWindow::handleValidate()
{
    updateInspector();
    const QList<QSocModuleProblem> problems = currentProblems();
    if (hasErrorProblem(problems)) {
        setStatusText(tr("Validation failed"));
    } else if (!problems.isEmpty()) {
        setStatusText(tr("Validation passed with warnings"));
    } else {
        setStatusText(tr("Validation passed"));
    }
}

void ModuleEditorWindow::handleRefresh()
{
    reloadProject(currentLibraryName, currentModuleName);
}

void ModuleEditorWindow::handleSearchChanged(const QString &text)
{
    portProxyModel->setFilterFixedString(text);
    parameterProxyModel->setFilterFixedString(text);
    interfaceProxyModel->setFilterFixedString(text);
    mappingProxyModel->setFilterFixedString(text);
}

bool ModuleEditorWindow::checkSaveBeforeDiscard()
{
    if (!isDirty())
        return true;

    const QMessageBox::StandardButton button = QMessageBox::question(
        this,
        tr("Unsaved Module"),
        tr("Save changes to %1/%2?").arg(currentLibraryName, currentModuleName),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
        QMessageBox::Save);

    if (button == QMessageBox::Save)
        return saveCurrentModule();
    if (button == QMessageBox::Discard) {
        setModelsDirty(false);
        return true;
    }
    return false;
}

bool ModuleEditorWindow::saveCurrentModule()
{
    if (currentLibraryName.isEmpty() || currentModuleName.isEmpty() || !hasWritableProject())
        return false;

    syncMappingToInterfaceModel();
    const QSocModuleDefinition     definition = currentDefinitionFromModels();
    const QList<QSocModuleProblem> problems   = moduleManager.validateModuleDefinition(definition);
    updateProblems(problems);
    if (hasErrorProblem(problems)) {
        QMessageBox::warning(this, tr("Save Blocked"), tr("Fix module errors before saving."));
        return false;
    }

    if (!moduleManager.replaceModuleDefinition(definition)) {
        QMessageBox::warning(this, tr("Save Failed"), tr("Cannot write module library."));
        return false;
    }

    setModelsDirty(false);
    reloadProject(definition.libraryName, definition.moduleName);
    setStatusText(tr("Saved %1/%2").arg(definition.libraryName, definition.moduleName));
    return true;
}

bool ModuleEditorWindow::isDirty() const
{
    return portModel->isDirty() || parameterModel->isDirty() || busInterfaceModel->isDirty()
           || busMappingModel->isDirty();
}

void ModuleEditorWindow::setModelsDirty(bool dirty)
{
    portModel->setDirty(dirty);
    parameterModel->setDirty(dirty);
    busInterfaceModel->setDirty(dirty);
    busMappingModel->setDirty(dirty);
}

void ModuleEditorWindow::syncMappingToInterfaceModel()
{
    if (currentInterfaceRow < 0 || currentInterfaceRow >= busInterfaceModel->rowCount())
        return;
    if (!busMappingModel->isDirty())
        return;
    syncingInterfaceRow = true;
    busInterfaceModel->setInterfaceAt(currentInterfaceRow, busMappingModel->interfaceDefinition());
    syncingInterfaceRow = false;
    busMappingModel->setDirty(false);
}

void ModuleEditorWindow::rebuildInterfaceMapping(int row, bool markDirty)
{
    if (row < 0 || row >= busInterfaceModel->rowCount())
        return;

    QSocModuleBusInterface interface = busInterfaceModel->interfaceAt(row);
    ModuleBusMappingModel  mappingBuilder;
    mappingBuilder.setContext(interface, busDefinitionForName(interface.busName), portModel->ports());
    mappingBuilder.rebuildRowsFromBusDefinition(markDirty);
    interface = mappingBuilder.interfaceDefinition();

    syncingInterfaceRow = true;
    busInterfaceModel->setInterfaceAt(row, interface);
    syncingInterfaceRow = false;
}

void ModuleEditorWindow::refreshMappingContext(bool markDirty)
{
    if (currentInterfaceRow < 0 || currentInterfaceRow >= busInterfaceModel->rowCount()) {
        busMappingModel->clear();
        return;
    }

    const QSocModuleBusInterface interface = busInterfaceModel->interfaceAt(currentInterfaceRow);
    busMappingModel
        ->setContext(interface, busDefinitionForName(interface.busName), portModel->ports());
    busMappingModel->rebuildRowsFromBusDefinition(markDirty);
}

QList<int> ModuleEditorWindow::selectedInterfaceRows() const
{
    QList<int> rows;
    if (!busInterfaceView->selectionModel())
        return rows;

    for (const QModelIndex &proxyIndex : busInterfaceView->selectionModel()->selectedRows()) {
        const QModelIndex source = interfaceProxyModel->mapToSource(proxyIndex);
        if (source.isValid())
            rows.append(source.row());
    }
    std::sort(rows.begin(), rows.end());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
    return rows;
}

QSocModuleBusInterface ModuleEditorWindow::selectedInterface() const
{
    if (currentInterfaceRow < 0)
        return {};
    return busInterfaceModel->interfaceAt(currentInterfaceRow);
}

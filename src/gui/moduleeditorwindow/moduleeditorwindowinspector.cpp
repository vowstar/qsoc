// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "moduleeditorwindow.h"

#include "gui/schematicwindow/schematicmodule.h"

#include <yaml-cpp/yaml.h>
#include <qschematic/scene.hpp>

#include <sstream>
#include <QDir>
#include <QGraphicsView>
#include <QItemSelectionModel>
#include <QLineEdit>
#include <QListWidget>
#include <QStatusBar>
#include <QTableWidget>

namespace {

QString problemPrefix(const QSocModuleProblem &problem)
{
    return problem.severity == QSocModuleProblemSeverity::Error ? ModuleEditorWindow::tr("Error")
                                                                : ModuleEditorWindow::tr("Warning");
}

QString problemText(const QSocModuleProblem &problem)
{
    QStringList parts;
    parts << problemPrefix(problem);
    if (!problem.code.isEmpty())
        parts << problem.code;
    if (!problem.section.isEmpty())
        parts << problem.section;
    if (problem.row >= 0)
        parts << ModuleEditorWindow::tr("row %1").arg(problem.row + 1);
    if (!problem.itemName.isEmpty())
        parts << problem.itemName;
    if (!problem.message.isEmpty())
        parts << problem.message;
    return parts.join(QStringLiteral(": "));
}

} // namespace

QList<QSocModuleProblem> ModuleEditorWindow::currentProblems() const
{
    if (currentLibraryName.isEmpty() || currentModuleName.isEmpty())
        return {};
    return moduleManager.validateModuleDefinition(currentDefinitionFromModels());
}

void ModuleEditorWindow::updateInspector()
{
    updateProblems(currentProblems());
    QStringList                  scanErrors;
    const QList<QSocModuleUsage> usages
        = currentModuleName.isEmpty()
              ? QList<QSocModuleUsage>()
              : moduleManager.scanModuleUsages(currentModuleName, &scanErrors);
    updateUsages(usages, scanErrors);
    updateSummary();
    updateSymbolPreview();
    updateYamlPreview();
}

void ModuleEditorWindow::updateProblems(const QList<QSocModuleProblem> &problems)
{
    problemList->clear();
    for (const QSocModuleProblem &problem : problems) {
        auto *item = new QListWidgetItem(problemText(problem), problemList);
        item->setData(Qt::UserRole, problem.section);
        item->setData(Qt::UserRole + 1, problem.row);
        if (problem.row >= 0)
            item->setToolTip(tr("Select row %1").arg(problem.row + 1));
    }
}

void ModuleEditorWindow::updateUsages(
    const QList<QSocModuleUsage> &usages, const QStringList &scanErrors)
{
    usageTable->setRowCount(usages.size() + scanErrors.size());
    int row = 0;
    for (const QSocModuleUsage &usage : usages) {
        usageTable->setItem(row, 0, new QTableWidgetItem(usage.sourceType));
        usageTable->setItem(row, 1, new QTableWidgetItem(usage.filePath));
        usageTable->setItem(row, 2, new QTableWidgetItem(usage.instanceName));
        usageTable->setItem(row, 3, new QTableWidgetItem(usage.moduleName));
        usageTable->setItem(row, 4, new QTableWidgetItem(QString::number(usage.portNames.size())));
        usageTable
            ->setItem(row, 5, new QTableWidgetItem(QString::number(usage.busInterfaces.size())));
        usageTable->setItem(row, 6, new QTableWidgetItem(usage.status));
        ++row;
    }
    for (const QString &scanError : scanErrors) {
        usageTable->setItem(row, 0, new QTableWidgetItem(tr("Error")));
        usageTable->setItem(row, 6, new QTableWidgetItem(scanError));
        ++row;
    }
}

void ModuleEditorWindow::updateSummary()
{
    if (currentLibraryName.isEmpty() || currentModuleName.isEmpty()) {
        summaryLabel->setText(tr("No module selected."));
        return;
    }

    const QString filePath = projectManager && projectManager->isValid()
                                 ? QDir(projectManager->getProjectPath())
                                       .relativeFilePath(
                                           QDir(projectManager->getModulePath())
                                               .filePath(
                                                   currentLibraryName + QStringLiteral(".soc_mod")))
                                 : currentLibraryName + QStringLiteral(".soc_mod");
    const QSocModuleDefinition   definition = currentDefinitionFromModels();
    QStringList                  scanErrors;
    const QList<QSocModuleUsage> usages
        = moduleManager.scanModuleUsages(currentModuleName, &scanErrors);
    summaryLabel->setText(
        tr("Library: %1\nFile: %2\nModule: %3\nPorts: %4\nParameters: %5\nBus interfaces: "
           "%6\nUsages: %7\nState: %8")
            .arg(
                currentLibraryName,
                filePath,
                currentModuleName,
                QString::number(definition.ports.size()),
                QString::number(definition.parameters.size()),
                QString::number(definition.busInterfaces.size()),
                QString::number(usages.size()),
                isDirty() ? tr("modified") : tr("clean")));
}

void ModuleEditorWindow::updateSymbolPreview()
{
    if (!symbolPreviewScene || !symbolPreviewView)
        return;

    if (symbolPreviewModule) {
        symbolPreviewScene->removeItem(symbolPreviewModule);
        symbolPreviewModule.reset();
    }

    const QRectF emptyRect(0, 0, 240, 180);
    if (currentLibraryName.isEmpty() || currentModuleName.isEmpty()) {
        symbolPreviewScene->setSceneRect(emptyRect);
        symbolPreviewView->fitInView(emptyRect, Qt::KeepAspectRatio);
        return;
    }

    const QSocModuleDefinition definition = currentDefinitionFromModels();
    const YAML::Node           yaml       = moduleManager.moduleDefinitionToYaml(definition);
    if (!yaml || yaml.IsNull()) {
        symbolPreviewScene->setSceneRect(emptyRect);
        symbolPreviewView->fitInView(emptyRect, Qt::KeepAspectRatio);
        return;
    }

    symbolPreviewModule = std::make_shared<SchematicModule>(currentModuleName, yaml);
    symbolPreviewModule->setInstanceName(currentModuleName);
    symbolPreviewScene->addItem(symbolPreviewModule);

    const QRectF bounds = symbolPreviewModule->sceneBoundingRect().adjusted(-40, -40, 40, 40);
    symbolPreviewScene->setSceneRect(bounds);
    symbolPreviewView->fitInView(bounds, Qt::KeepAspectRatio);
}

void ModuleEditorWindow::updateYamlPreview()
{
    if (currentLibraryName.isEmpty() || currentModuleName.isEmpty()) {
        yamlPreview->clear();
        return;
    }

    const QSocModuleDefinition definition = currentDefinitionFromModels();
    const YAML::Node           yaml       = moduleManager.moduleDefinitionToYaml(definition);
    std::stringstream          stream;
    stream << yaml;
    yamlPreview->setPlainText(QString::fromStdString(stream.str()));
}

void ModuleEditorWindow::updateActions()
{
    const bool hasProject           = hasWritableProject();
    const bool hasLibraries         = hasProject && !moduleManager.listLoadedLibraries().isEmpty();
    const bool hasModule            = !currentLibraryName.isEmpty() && !currentModuleName.isEmpty();
    const bool hasCleanModule       = hasModule && !isDirty();
    const bool hasSelectedInterface = currentInterfaceRow >= 0;
    const bool mappingTabActive     = editorTabs->currentWidget() == busMappingView;
    newLibraryAction->setEnabled(hasProject);
    newModuleAction->setEnabled(hasLibraries && !isDirty());
    duplicateModuleAction->setEnabled(hasProject && hasCleanModule);
    renameModuleAction->setEnabled(hasProject && hasCleanModule);
    deleteModuleAction->setEnabled(hasProject && hasCleanModule);
    deleteLibraryAction->setEnabled(hasLibraries && !isDirty());
    importVerilogAction->setEnabled(hasProject && !isDirty());
    addRowAction->setEnabled(hasProject && hasModule);
    duplicateRowAction->setEnabled(hasProject && hasModule);
    deleteRowAction->setEnabled(hasProject && hasModule);
    addInterfaceAction->setEnabled(hasProject && hasModule);
    duplicateInterfaceAction->setEnabled(hasProject && hasModule && hasSelectedInterface);
    renameInterfaceAction->setEnabled(hasProject && hasModule && hasSelectedInterface);
    deleteInterfaceAction->setEnabled(hasProject && hasModule && hasSelectedInterface);
    autoMatchNameAction->setEnabled(hasProject && hasModule && hasSelectedInterface);
    autoMatchAction->setEnabled(hasProject && hasModule && hasSelectedInterface);
    clearMissingAction->setEnabled(hasProject && hasModule && hasSelectedInterface);
    createMissingPortsAction->setEnabled(hasProject && hasModule && hasSelectedInterface);
    showOnlyProblemsAction->setEnabled(hasModule && mappingTabActive);
    showEmptyMappingsAction->setEnabled(hasModule && mappingTabActive);
    openBusEditorAction->setEnabled(hasProject && hasModule && hasSelectedInterface);
    saveAction->setEnabled(hasProject && hasModule && isDirty());
    revertAction->setEnabled(hasModule && isDirty());
    validateAction->setEnabled(hasModule);
    refreshAction->setEnabled(projectManager && projectManager->isValid());
}

void ModuleEditorWindow::setStatusText(const QString &text)
{
    statusLabel->setText(text);
    statusBar()->showMessage(text, 3000);
}

void ModuleEditorWindow::handleProblemActivated(QListWidgetItem *item)
{
    if (!item)
        return;

    const QString section = item->data(Qt::UserRole).toString();
    bool          ok      = false;
    const int     row     = item->data(Qt::UserRole + 1).toInt(&ok);
    if (!ok || row < 0)
        return;

    if (section == QStringLiteral("bus"))
        selectInterfaceRow(row);
}

void ModuleEditorWindow::selectInterfaceRow(int row)
{
    if (row < 0 || row >= busInterfaceModel->rowCount())
        return;

    QModelIndex proxyIndex = interfaceProxyModel->mapFromSource(busInterfaceModel->index(row, 0));
    if (!proxyIndex.isValid()) {
        searchEdit->clear();
        proxyIndex = interfaceProxyModel->mapFromSource(busInterfaceModel->index(row, 0));
    }

    if (!proxyIndex.isValid() || !busInterfaceView->selectionModel())
        return;

    busInterfaceView->selectionModel()
        ->select(proxyIndex, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    busInterfaceView->setCurrentIndex(proxyIndex);
    busInterfaceView->scrollTo(proxyIndex);
    editorTabs->setCurrentWidget(busInterfaceView);
    busInterfaceView->setFocus();
    setStatusText(tr("Selected interface row %1").arg(row + 1));
}

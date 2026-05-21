// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "buseditorwindow.h"

#include <sstream>
#include <QDir>
#include <QItemSelectionModel>
#include <QListWidget>
#include <QSet>
#include <QStatusBar>
#include <QTableWidgetItem>

namespace {

QString problemPrefix(const QSocBusProblem &problem)
{
    return problem.severity == QSocBusProblemSeverity::Error ? BusEditorWindow::tr("Error")
                                                             : BusEditorWindow::tr("Warning");
}

QString problemText(const QSocBusProblem &problem)
{
    QStringList parts;
    parts << problemPrefix(problem);
    if (!problem.code.isEmpty())
        parts << problem.code;
    if (problem.row >= 0)
        parts << BusEditorWindow::tr("row %1").arg(problem.row + 1);
    if (!problem.signal.isEmpty())
        parts << problem.signal;
    if (!problem.mode.isEmpty())
        parts << problem.mode;
    if (!problem.moduleName.isEmpty()) {
        QStringList usageParts;
        if (!problem.moduleLibrary.isEmpty())
            usageParts << problem.moduleLibrary;
        usageParts << problem.moduleName;
        if (!problem.interfaceName.isEmpty())
            usageParts << problem.interfaceName;
        parts << usageParts.join(QStringLiteral("/"));
    }
    if (!problem.message.isEmpty())
        parts << problem.message;
    return parts.join(QStringLiteral(": "));
}

QString usageProblemState(const QSocBusUsage &usage, const QSocBusDefinition &definition)
{
    QSet<QString> signalNames;
    QSet<QString> modes;
    for (const QSocBusSignalMode &row : definition.rows) {
        if (!row.signal.isEmpty())
            signalNames.insert(row.signal);
        if (!row.mode.isEmpty())
            modes.insert(row.mode);
    }

    QStringList problems;
    if (!usage.mode.isEmpty() && !modes.contains(usage.mode))
        problems << BusEditorWindow::tr("Mode");
    for (const QString &signal : usage.mappingSignals) {
        if (!signalNames.contains(signal)) {
            problems << BusEditorWindow::tr("Signal");
            break;
        }
    }
    if (!usage.emptyMappingSignals.isEmpty())
        problems << BusEditorWindow::tr("Empty");
    return problems.isEmpty() ? BusEditorWindow::tr("OK") : problems.join(QStringLiteral(", "));
}

} // namespace

QList<QSocBusProblem> BusEditorWindow::currentProblems(QStringList *scanErrors) const
{
    QList<QSocBusProblem> problems;
    if (currentLibraryName.isEmpty() || currentBusName.isEmpty())
        return problems;

    const QSocBusDefinition definition = currentDefinitionFromModel();
    problems.append(busManager.validateBusDefinition(definition));
    if (projectManager && projectManager->isValid())
        problems.append(busManager.validateBusReferences(definition, scanErrors));
    return problems;
}

void BusEditorWindow::updateInspector()
{
    QStringList                 scanErrors;
    const QList<QSocBusProblem> problems = currentProblems(&scanErrors);
    updateProblems(problems, scanErrors);

    QStringList               usageErrors;
    const QList<QSocBusUsage> usages = currentBusName.isEmpty()
                                           ? QList<QSocBusUsage>()
                                           : busManager.scanBusUsages(currentBusName, &usageErrors);
    updateUsages(usages);
    updateSummary();
    updateYamlPreview();
}

void BusEditorWindow::updateProblems(
    const QList<QSocBusProblem> &problems, const QStringList &scanErrors)
{
    problemList->clear();
    for (const QString &error : scanErrors)
        problemList->addItem(tr("Error: scan: %1").arg(error));
    for (const QSocBusProblem &problem : problems) {
        auto *item = new QListWidgetItem(problemText(problem), problemList);
        item->setData(Qt::UserRole, problem.row);
        item->setData(Qt::UserRole + 1, problem.code);
        if (problem.row >= 0)
            item->setToolTip(tr("Select row %1").arg(problem.row + 1));
    }
}

void BusEditorWindow::updateUsages(const QList<QSocBusUsage> &usages)
{
    const QSocBusDefinition definition = currentDefinitionFromModel();
    usageTable->setRowCount(usages.size());
    for (int row = 0; row < usages.size(); ++row) {
        const QSocBusUsage &usage = usages.at(row);
        usageTable->setItem(row, 0, new QTableWidgetItem(usage.moduleLibrary));
        usageTable->setItem(row, 1, new QTableWidgetItem(usage.moduleName));
        usageTable->setItem(row, 2, new QTableWidgetItem(usage.interfaceName));
        usageTable->setItem(row, 3, new QTableWidgetItem(usage.busName));
        usageTable->setItem(row, 4, new QTableWidgetItem(usage.mode));
        usageTable
            ->setItem(row, 5, new QTableWidgetItem(QString::number(usage.mappingSignals.size())));
        usageTable->setItem(row, 6, new QTableWidgetItem(usageProblemState(usage, definition)));
    }
}

void BusEditorWindow::updateSummary()
{
    if (currentLibraryName.isEmpty() || currentBusName.isEmpty()) {
        summaryLabel->setText(tr("No bus selected."));
        return;
    }

    const QSocBusDefinition definition = currentDefinitionFromModel();
    QSet<QString>           signalNames;
    QSet<QString>           modes;
    for (const QSocBusSignalMode &row : definition.rows) {
        if (!row.signal.isEmpty())
            signalNames.insert(row.signal);
        if (!row.mode.isEmpty())
            modes.insert(row.mode);
    }

    const QString filePath = projectManager && projectManager->isValid()
                                 ? QDir(projectManager->getProjectPath())
                                       .relativeFilePath(
                                           QDir(projectManager->getBusPath())
                                               .filePath(
                                                   currentLibraryName + QStringLiteral(".soc_bus")))
                                 : currentLibraryName + QStringLiteral(".soc_bus");
    QStringList   usageErrors;
    const QList<QSocBusUsage> usages = busManager.scanBusUsages(currentBusName, &usageErrors);

    summaryLabel->setText(tr("Library: %1\nFile: %2\nBus: %3\nSignals: %4\nModes: %5\nRows: "
                             "%6\nUsages: %7\nState: %8")
                              .arg(
                                  currentLibraryName,
                                  filePath,
                                  currentBusName,
                                  QString::number(signalNames.size()),
                                  QString::number(modes.size()),
                                  QString::number(signalModeModel->rowCount()),
                                  QString::number(usages.size()),
                                  signalModeModel->isDirty() ? tr("modified") : tr("clean")));
}

void BusEditorWindow::updateYamlPreview()
{
    if (currentLibraryName.isEmpty() || currentBusName.isEmpty()) {
        yamlPreview->clear();
        return;
    }

    const QSocBusDefinition definition = currentDefinitionFromModel();
    const YAML::Node        yaml       = busManager.busDefinitionToYaml(definition);
    std::stringstream       stream;
    stream << yaml;
    yamlPreview->setPlainText(QString::fromStdString(stream.str()));
}

void BusEditorWindow::updateActions()
{
    const bool hasProject = hasWritableProject();
    const bool hasBus     = !currentLibraryName.isEmpty() && !currentBusName.isEmpty();
    const bool hasLibrary = libraryView
                            && !BusLibraryModel::libraryName(libraryView->currentIndex()).isEmpty();
    newLibraryAction->setEnabled(hasProject);
    newBusAction->setEnabled(hasProject);
    duplicateBusAction->setEnabled(hasProject && hasBus);
    renameBusAction->setEnabled(hasProject && hasBus);
    deleteLibraryAction->setEnabled(hasProject && (hasLibrary || !currentLibraryName.isEmpty()));
    importCsvAction->setEnabled(hasProject);
    addRowAction->setEnabled(hasBus);
    duplicateRowsAction->setEnabled(hasBus);
    deleteRowsAction->setEnabled(hasBus);
    deleteBusAction->setEnabled(hasProject && hasBus);
    saveAction->setEnabled(hasProject && hasBus && signalModeModel->isDirty());
    revertAction->setEnabled(hasBus && signalModeModel->isDirty());
    validateAction->setEnabled(hasBus);
    refreshAction->setEnabled(projectManager && projectManager->isValid());
}

void BusEditorWindow::setStatusText(const QString &text)
{
    statusLabel->setText(text);
    statusBar()->showMessage(text, 3000);
}

void BusEditorWindow::handleProblemActivated(QListWidgetItem *item)
{
    if (!item)
        return;

    bool      ok  = false;
    const int row = item->data(Qt::UserRole).toInt(&ok);
    if (!ok || row < 0)
        return;

    selectSourceRow(row);
}

void BusEditorWindow::selectSourceRow(int row)
{
    if (row < 0 || row >= signalModeModel->rowCount())
        return;

    QModelIndex proxyIndex = signalProxyModel->mapFromSource(signalModeModel->index(row, 0));
    if (!proxyIndex.isValid()) {
        searchEdit->clear();
        proxyIndex = signalProxyModel->mapFromSource(signalModeModel->index(row, 0));
    }

    if (!proxyIndex.isValid() || !tableView->selectionModel())
        return;

    tableView->selectionModel()
        ->select(proxyIndex, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    tableView->setCurrentIndex(proxyIndex);
    tableView->scrollTo(proxyIndex);
    tableView->setFocus();
    setStatusText(tr("Selected row %1").arg(row + 1));
}

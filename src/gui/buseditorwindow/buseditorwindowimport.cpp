// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "buseditorwindow.h"

#include "buscsvimportdialog.h"

#include <QFileDialog>
#include <QHash>
#include <QMessageBox>

namespace {

QString rowKey(const QSocBusSignalMode &row)
{
    return row.signal + QChar(0x1f) + row.mode;
}

void preserveRowExtras(QSocBusSignalMode &target, const QSocBusSignalMode &source)
{
    target.signalExtraAttributes = source.signalExtraAttributes;
    target.modeExtraAttributes   = source.modeExtraAttributes;
}

QSocBusDefinition mergeCsvRows(
    QSocBusDefinition definition, const QList<QSocBusSignalMode> &rows, BusCsvMergeMode mergeMode)
{
    if (mergeMode == BusCsvMergeMode::Replace) {
        definition.rows = rows;
        return definition;
    }

    if (mergeMode == BusCsvMergeMode::Append) {
        definition.rows.append(rows);
        return definition;
    }

    QHash<QString, int> existingRows;
    for (int i = 0; i < definition.rows.size(); ++i)
        existingRows.insert(rowKey(definition.rows.at(i)), i);

    for (QSocBusSignalMode row : rows) {
        const QString key = rowKey(row);
        const auto    it  = existingRows.constFind(key);
        if (it == existingRows.constEnd()) {
            existingRows.insert(key, definition.rows.size());
            definition.rows.append(row);
            continue;
        }

        preserveRowExtras(row, definition.rows.at(*it));
        definition.rows[*it] = row;
    }
    return definition;
}

} // namespace

void BusEditorWindow::handleImportCsv()
{
    if (!hasWritableProject()) {
        QMessageBox::information(this, tr("No Project"), tr("Open a writable project first."));
        return;
    }

    if (!checkSaveBeforeDiscard())
        return;

    const QStringList filePaths = QFileDialog::getOpenFileNames(
        this, tr("Import Bus CSV"), QString(), tr("CSV Files (*.csv);;All Files (*)"));
    if (filePaths.isEmpty())
        return;

    QStringList                    warnings;
    const QList<QSocBusSignalMode> rows = busManager.parseBusCsvFiles(filePaths, &warnings);
    BusCsvImportDialog             dialog(rows, warnings, &busManager, this);
    dialog.setTarget(currentLibraryName, currentBusName);
    if (dialog.exec() != QDialog::Accepted)
        return;

    if (!importCsvRows(rows, dialog.libraryName(), dialog.busName(), dialog.mergeMode()))
        QMessageBox::warning(this, tr("Import Failed"), tr("Cannot import the selected CSV files."));
}

bool BusEditorWindow::importCsvFiles(
    const QStringList &filePaths,
    const QString     &libraryName,
    const QString     &busName,
    BusCsvMergeMode    mergeMode,
    QStringList       *warnings)
{
    if (!hasWritableProject() || filePaths.isEmpty() || !checkSaveBeforeDiscard())
        return false;

    QStringList                    localWarnings;
    QStringList                   *targetWarnings = warnings ? warnings : &localWarnings;
    const QList<QSocBusSignalMode> rows = busManager.parseBusCsvFiles(filePaths, targetWarnings);
    return importCsvRows(rows, libraryName, busName, mergeMode);
}

bool BusEditorWindow::importCsvRows(
    const QList<QSocBusSignalMode> &rows,
    const QString                  &libraryName,
    const QString                  &busName,
    BusCsvMergeMode                 mergeMode)
{
    const QString targetLibrary = libraryName.trimmed();
    const QString targetBus     = busName.trimmed();
    if (!hasWritableProject() || rows.isEmpty() || targetLibrary.isEmpty() || targetBus.isEmpty())
        return false;

    if (!busManager.listLoadedLibraries().contains(targetLibrary)) {
        if (!busManager.createLibrary(targetLibrary))
            return false;
    }

    QSocBusDefinition definition = busManager.getBusDefinition(targetLibrary, targetBus);
    definition.libraryName       = targetLibrary;
    definition.busName           = targetBus;
    definition                   = mergeCsvRows(definition, rows, mergeMode);

    currentLibraryName = targetLibrary;
    currentBusName     = targetBus;
    libraryModel->setBusManager(&busManager);
    libraryView->expandAll();
    libraryView->setCurrentIndex(libraryModel->indexForBus(targetLibrary, targetBus));
    signalModeModel->setDefinition(definition);
    signalModeModel->setDirty(true);
    updateInspector();
    updateActions();
    setStatusText(
        tr("Imported %1 CSV rows into %2/%3").arg(rows.size()).arg(targetLibrary, targetBus));
    return true;
}

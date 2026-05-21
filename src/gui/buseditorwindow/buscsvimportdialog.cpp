// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "buscsvimportdialog.h"

#include "bussignalmodemodel.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QSplitter>
#include <QVBoxLayout>

BusCsvImportDialog::BusCsvImportDialog(
    const QList<QSocBusSignalMode> &rows,
    const QStringList              &warnings,
    QSocBusManager                 *manager,
    QWidget                        *parent)
    : QDialog(parent)
    , rows(rows)
    , warnings(warnings)
    , manager(manager)
{
    setupUi();
    populatePreview();
    updateBusNames();
}

QString BusCsvImportDialog::libraryName() const
{
    return libraryCombo->currentText().trimmed();
}

QString BusCsvImportDialog::busName() const
{
    return busCombo->currentText().trimmed();
}

BusCsvMergeMode BusCsvImportDialog::mergeMode() const
{
    return static_cast<BusCsvMergeMode>(mergeCombo->currentData().toInt());
}

void BusCsvImportDialog::setTarget(const QString &libraryName, const QString &busName)
{
    if (!libraryName.isEmpty())
        libraryCombo->setCurrentText(libraryName);
    updateBusNames();
    if (!busName.isEmpty())
        busCombo->setCurrentText(busName);
}

void BusCsvImportDialog::accept()
{
    if (libraryName().isEmpty() || busName().isEmpty()) {
        QMessageBox::warning(this, tr("Missing Target"), tr("Select a target library and bus."));
        return;
    }
    if (rows.isEmpty()) {
        QMessageBox::warning(this, tr("Empty Import"), tr("The selected CSV files contain no rows."));
        return;
    }
    QDialog::accept();
}

void BusCsvImportDialog::setupUi()
{
    setWindowTitle(tr("Import Bus CSV"));
    resize(820, 560);

    auto *layout = new QVBoxLayout(this);

    auto *form   = new QFormLayout();
    libraryCombo = new QComboBox(this);
    libraryCombo->setObjectName(QStringLiteral("busCsvLibraryCombo"));
    libraryCombo->setEditable(true);
    if (manager)
        libraryCombo->addItems(manager->listLoadedLibraries());
    form->addRow(tr("Library"), libraryCombo);

    busCombo = new QComboBox(this);
    busCombo->setObjectName(QStringLiteral("busCsvBusCombo"));
    busCombo->setEditable(true);
    form->addRow(tr("Bus"), busCombo);

    mergeCombo = new QComboBox(this);
    mergeCombo->setObjectName(QStringLiteral("busCsvMergeCombo"));
    mergeCombo->addItem(tr("Replace bus"), static_cast<int>(BusCsvMergeMode::Replace));
    mergeCombo->addItem(tr("Append rows"), static_cast<int>(BusCsvMergeMode::Append));
    mergeCombo->addItem(tr("Merge by signal and mode"), static_cast<int>(BusCsvMergeMode::Merge));
    form->addRow(tr("Mode"), mergeCombo);
    layout->addLayout(form);

    auto *splitter = new QSplitter(Qt::Vertical, this);
    previewTable   = new QTableWidget(splitter);
    previewTable->setObjectName(QStringLiteral("busCsvPreviewTable"));
    previewTable->setColumnCount(BusSignalModeModel::ColumnCount);
    previewTable->setHorizontalHeaderLabels(
        {tr("Signal"), tr("Mode"), tr("Direction"), tr("Width"), tr("Qualifier"), tr("Description")});
    previewTable->horizontalHeader()->setStretchLastSection(true);
    previewTable->verticalHeader()->setVisible(false);
    previewTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    splitter->addWidget(previewTable);

    warningList = new QListWidget(splitter);
    warningList->setObjectName(QStringLiteral("busCsvWarningList"));
    splitter->addWidget(warningList);
    splitter->setSizes({420, 120});
    layout->addWidget(splitter);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &BusCsvImportDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &BusCsvImportDialog::reject);
    layout->addWidget(buttons);

    connect(libraryCombo, &QComboBox::currentTextChanged, this, &BusCsvImportDialog::updateBusNames);
}

void BusCsvImportDialog::populatePreview()
{
    previewTable->setRowCount(rows.size());
    for (int row = 0; row < rows.size(); ++row) {
        const QSocBusSignalMode &item = rows.at(row);
        previewTable
            ->setItem(row, BusSignalModeModel::SignalColumn, new QTableWidgetItem(item.signal));
        previewTable->setItem(row, BusSignalModeModel::ModeColumn, new QTableWidgetItem(item.mode));
        previewTable
            ->setItem(row, BusSignalModeModel::DirectionColumn, new QTableWidgetItem(item.direction));
        previewTable->setItem(row, BusSignalModeModel::WidthColumn, new QTableWidgetItem(item.width));
        previewTable
            ->setItem(row, BusSignalModeModel::QualifierColumn, new QTableWidgetItem(item.qualifier));
        previewTable->setItem(
            row, BusSignalModeModel::DescriptionColumn, new QTableWidgetItem(item.description));
    }

    warningList->clear();
    for (const QString &warning : warnings)
        warningList->addItem(warning);
}

void BusCsvImportDialog::updateBusNames()
{
    const QString currentBus = busCombo->currentText();
    busCombo->clear();

    if (manager && !libraryName().isEmpty())
        busCombo->addItems(manager->listBusesInLibrary(libraryName()));

    if (!currentBus.isEmpty())
        busCombo->setCurrentText(currentBus);
}

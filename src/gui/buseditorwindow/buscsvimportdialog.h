// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef BUSCSVIMPORTDIALOG_H
#define BUSCSVIMPORTDIALOG_H

#include "common/qsocbusmanager.h"

#include <QComboBox>
#include <QDialog>
#include <QList>
#include <QListWidget>
#include <QTableWidget>

enum class BusCsvMergeMode { Replace, Append, Merge };

class BusCsvImportDialog : public QDialog
{
    Q_OBJECT

public:
    explicit BusCsvImportDialog(
        const QList<QSocBusSignalMode> &rows,
        const QStringList              &warnings,
        QSocBusManager                 *manager,
        QWidget                        *parent = nullptr);

    QString         libraryName() const;
    QString         busName() const;
    BusCsvMergeMode mergeMode() const;
    void            setTarget(const QString &libraryName, const QString &busName);

protected:
    void accept() override;

private:
    void setupUi();
    void populatePreview();
    void updateBusNames();

    QList<QSocBusSignalMode> rows;
    QStringList              warnings;
    QSocBusManager          *manager = nullptr;

    QComboBox    *libraryCombo = nullptr;
    QComboBox    *busCombo     = nullptr;
    QComboBox    *mergeCombo   = nullptr;
    QTableWidget *previewTable = nullptr;
    QListWidget  *warningList  = nullptr;
};

#endif // BUSCSVIMPORTDIALOG_H

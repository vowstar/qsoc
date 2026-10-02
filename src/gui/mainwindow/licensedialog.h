// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef LICENSEDIALOG_H
#define LICENSEDIALOG_H

#include <QDialog>
#include <QListWidget>
#include <QPlainTextEdit>

/**
 * @brief Read-only viewer for the licenses of third-party components.
 */
class LicenseDialog : public QDialog
{
    Q_OBJECT

public:
    explicit LicenseDialog(QWidget *parent = nullptr);

private slots:
    /* Manual Signal Handlers */
    void handleCurrentRowChanged(int row);

private:
    QListWidget    *componentList = nullptr;
    QPlainTextEdit *licenseText   = nullptr;
};

#endif // LICENSEDIALOG_H

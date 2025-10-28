// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#include "gui/mainwindow/mainwindow.h"

#include "./ui_mainwindow.h"

#include <QDebug>
#include <QFileInfo>
#include <QStandardItem>
#include <QStandardItemModel>

void MainWindow::handleTreeDoubleClick(const QModelIndex &index)
{
    auto *model = qobject_cast<QStandardItemModel *>(ui->treeViewProjectFile->model());
    if (!model) {
        return;
    }

    QStandardItem *item = model->itemFromIndex(index);
    if (!item) {
        return;
    }

    QString   filePath = item->data(Qt::UserRole).toString();
    QFileInfo fileInfo(filePath);

    /* Only handle files, not directories */
    if (!fileInfo.isFile()) {
        return;
    }

    /* Open the appropriate editor based on file extension */
    if (fileInfo.suffix() == "soc_sch") {
        /* Open Schematic Editor using unified method */
        openSchematicEditor(filePath);
    }
    /* Future extension points:
     * else if (fileInfo.suffix() == "soc_mod") { ... }
     * else if (fileInfo.suffix() == "soc_bus") { ... }
     */
}

void MainWindow::openSchematicEditor(const QString &filePath)
{
    qDebug() << "MainWindow: Opening schematic editor"
             << (filePath.isEmpty() ? "(untitled)" : filePath);

    // If window is already visible with unsaved changes, close triggers save prompt
    if (schematicWindow.isVisible()) {
        if (!schematicWindow.close()) {
            qDebug() << "MainWindow: User cancelled close";
            return; // User cancelled
        }
    }

    // Set parent and window flag
    schematicWindow.setParent(this);
    schematicWindow.setWindowFlag(Qt::Window, true);

    // Set project manager (ensures module list is loaded)
    if (projectManager && projectManager->isValid()) {
        qDebug() << "MainWindow: Setting project manager to schematic window";
        schematicWindow.setProjectManager(projectManager);
    } else {
        qDebug() << "MainWindow: No valid project manager, schematic will use empty model";
    }

    // Open file if specified, otherwise it's a new "untitled" file
    if (!filePath.isEmpty()) {
        schematicWindow.openFile(filePath);
    }

    // Show and activate window
    schematicWindow.show();
    schematicWindow.raise();
    schematicWindow.activateWindow();
    qDebug() << "MainWindow: Schematic window opened";
}

// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#include "gui/mainwindow/mainwindow.h"

#include "./ui_mainwindow.h"

#include <QApplication>

void MainWindow::on_actionQuit_triggered()
{
    close();
}

void MainWindow::on_actionSchematicEditor_triggered()
{
    qDebug() << "MainWindow: Opening schematic editor";
    qDebug() << "MainWindow: projectManager is" << (projectManager ? "valid" : "null");

    if (projectManager) {
        qDebug() << "MainWindow: projectManager->isValid() =" << projectManager->isValid();
        if (projectManager->isValid()) {
            qDebug() << "MainWindow: Project path:" << projectManager->getProjectPath();
            qDebug() << "MainWindow: Module path:" << projectManager->getModulePath();
        }
    }

    schematicWindow.setParent(this);
    schematicWindow.setWindowFlag(Qt::Window, true);

    // Set the project manager if available
    if (projectManager && projectManager->isValid()) {
        qDebug() << "MainWindow: Setting project manager to schematic window";
        schematicWindow.setProjectManager(projectManager);
    } else {
        qDebug() << "MainWindow: No valid project manager, schematic will use empty model";
    }

    qDebug() << "MainWindow: Showing schematic window";
    schematicWindow.show();
    qDebug() << "MainWindow: Schematic window shown";
}

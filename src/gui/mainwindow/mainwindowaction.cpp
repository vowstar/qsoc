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
    schematicWindow.setParent(this);
    schematicWindow.setWindowFlag(Qt::Window, true);
    schematicWindow.show();
}

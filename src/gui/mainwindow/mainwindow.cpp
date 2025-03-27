// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#include "gui/mainwindow/mainwindow.h"

#include "./ui_mainwindow.h"

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    /* Initialize QObject hierarchy with parent-child relationships for automatic memory management */
    projectManager = new QSocProjectManager(this);

    /* Configure UI elements */
    ui->toolButtonSchematicEditor->setDefaultAction(ui->actionSchematicEditor);
    ui->toolButtonModuleEditor->setDefaultAction(ui->actionModuleEditor);

    /* Configure project tree view */
    ui->treeViewProjectFile->setHeaderHidden(true);
    ui->treeViewProjectFile->setStyleSheet(
        "QTreeView::item {"
        "    height: 25px;" /* Fixed item height */
        "    padding: 2px;" /* Visual padding */
        "}");
    ui->treeViewProjectFile->setIconSize(QSize(24, 24));
}

MainWindow::~MainWindow()
{
    delete ui;
}

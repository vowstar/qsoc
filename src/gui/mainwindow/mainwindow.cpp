// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#include "gui/mainwindow/mainwindow.h"

#include "./ui_mainwindow.h"

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , projectManager(new QSocProjectManager(this))
{
    ui->setupUi(this);

    /* Configure UI elements */
    ui->toolButtonBusEditor->setDefaultAction(ui->actionBusEditor);
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
    ui->treeViewProjectFile->setEditTriggers(QAbstractItemView::NoEditTriggers);

    /* Connect double-click signal */
    connect(
        ui->treeViewProjectFile,
        &QTreeView::doubleClicked,
        this,
        &MainWindow::onTreeItemDoubleClicked);

    /* Auto-open project if exactly one exists in current directory */
    autoOpenSingleProject();
}

MainWindow::~MainWindow()
{
    delete ui;
}

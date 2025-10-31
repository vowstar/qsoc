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
        &MainWindow::handleTreeDoubleClick);

    /* Setup permanent status bar label (not affected by QMenuBar clearMessage) */
    statusBarPermanentLabel = new QLabel(this);
    statusBarPermanentLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    statusBar()->addPermanentWidget(statusBarPermanentLabel, 1);

    /* Auto-open project if exactly one exists in current directory */
    autoOpenSingleProject();
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::updateWindowTitle()
{
    if (!projectManager || projectManager->getProjectName().isEmpty()) {
        setWindowTitle("QSoC");
        return;
    }

    /* Build full project file path */
    const QString projectPath = projectManager->getProjectPath() + "/"
                                + projectManager->getProjectName() + ".soc_pro";

    /* Truncate if too long */
    const QString displayPath = truncateMiddle(projectPath, 60);

    setWindowTitle(QString("QSoC - Project: %1").arg(displayPath));
}

QString MainWindow::truncateMiddle(const QString &str, int maxLen)
{
    if (str.length() <= maxLen) {
        return str;
    }

    /* Minimum 4 chars needed: "a..." */
    if (maxLen < 4) {
        return str.left(maxLen);
    }

    const int ellipsisLen  = 3;
    const int availableLen = maxLen - ellipsisLen;
    const int leftLen      = availableLen / 2;
    const int rightLen     = availableLen - leftLen;

    return str.left(leftLen) + "..." + str.right(rightLen);
}

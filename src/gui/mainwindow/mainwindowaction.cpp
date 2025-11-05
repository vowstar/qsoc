// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#include "gui/mainwindow/mainwindow.h"

#include "./ui_mainwindow.h"

#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QProcess>
#include <QStandardItemModel>
#include <QStatusBar>

void MainWindow::on_actionQuit_triggered()
{
    close();
}

void MainWindow::on_actionSchematicEditor_triggered()
{
    openSchematicEditor(); // No file path = new untitled file
}

void MainWindow::on_actionPRCEditor_triggered()
{
    openPrcEditor(); // No file path = new untitled file
}

void MainWindow::on_toolButtonPRCEditor_clicked()
{
    openPrcEditor(); // No file path = new untitled file
}

void MainWindow::on_actionNewProject_triggered()
{
    /* Close current project first (silent mode) */
    closeProject(true);

    /* Show save dialog to get project name and path */
    const QString filePath = QFileDialog::getSaveFileName(
        this,
        tr("Create New Project"),
        lastProjectDir,
        tr("QSoC Project (*.soc_pro);;All Files (*)"));

    /* Check if user canceled the dialog */
    if (filePath.isEmpty()) {
        return;
    }

    /* Extract project name and directory path */
    const QFileInfo fileInfo(filePath);
    const QString   projectName = fileInfo.baseName();
    const QString   projectDir  = QDir(fileInfo.absolutePath()).filePath(projectName);

    /* Configure project manager */
    projectManager->setProjectName(projectName);
    projectManager->setCurrentPath(projectDir);
    /* Create project structure */
    if (!projectManager->mkpath() || !projectManager->save(projectName)) {
        /* Error handling */
        qCritical() << "Failed to initialize project structure";
        QMessageBox::critical(
            this,
            tr("Project Creation Error"),
            tr("Failed to create project structure at: %1").arg(projectDir));
        return;
    }

    /* Remember the current project directory for next time */
    QDir dir(fileInfo.absolutePath());
    if (dir.cdUp()) {
        lastProjectDir = dir.absolutePath();
    } else {
        lastProjectDir = fileInfo.absolutePath();
    }

    /* Setup project tree view */
    setupProjectTreeView(projectName);
}

void MainWindow::on_actionOpenProject_triggered()
{
    /* Close current project first (silent mode) */
    closeProject(true);

    /* Show open dialog to get project file */
    const QString filePath = QFileDialog::getOpenFileName(
        this, tr("Open Project"), lastProjectDir, tr("QSoC Project (*.soc_pro);;All Files (*)"));

    /* Check if user canceled the dialog */
    if (filePath.isEmpty()) {
        return;
    }

    /* Extract project name from file path */
    const QFileInfo fileInfo(filePath);
    const QString   projectName = fileInfo.baseName();
    const QString   projectDir  = fileInfo.absolutePath();

    /* Configure and load project */
    projectManager->setProjectPath(projectDir);
    if (!projectManager->load(projectName)) {
        /* Error handling */
        qCritical() << "Failed to load project:" << projectName;
        QMessageBox::critical(
            this, tr("Project Loading Error"), tr("Failed to load project: %1").arg(projectName));
        return;
    }

    /* Remember the current project directory for next time */
    QDir dirParent(projectDir);
    if (dirParent.cdUp()) {
        lastProjectDir = dirParent.absolutePath();
    } else {
        lastProjectDir = projectDir;
    }

    /* Setup project tree view */
    setupProjectTreeView(projectName);
}

void MainWindow::on_actionCloseProject_triggered()
{
    closeProject(false);
}

void MainWindow::on_actionOpenProjectInFileExplorer_triggered()
{
    /* Check if there's an active project */
    if (!projectManager || projectManager->getProjectName().isEmpty()) {
        QMessageBox::information(this, tr("No Project Open"), tr("Please open a project first."));
        return;
    }

    const QString projectPath = projectManager->getProjectPath();

    /* Ensure the directory exists */
    const QDir dir(projectPath);
    if (!dir.exists()) {
        QMessageBox::warning(
            this,
            tr("Directory Not Found"),
            tr("The project directory does not exist: %1").arg(projectPath));
        return;
    }

    bool success = false;

#if defined(Q_OS_WIN)
    /* Windows: Use explorer.exe */
    success
        = QProcess::startDetached("explorer", QStringList() << QDir::toNativeSeparators(projectPath));
#elif defined(Q_OS_MAC)
    /* macOS: Use open command */
    success = QProcess::startDetached("open", QStringList() << projectPath);
#else
    /* Linux and other Unix-like systems */
    const QStringList fileManagers = {
        "xdg-open", /**< Should be available on most Linux distributions */
        "nautilus", /**< GNOME */
        "dolphin",  /**< KDE */
        "thunar",   /**< Xfce */
        "pcmanfm",  /**< LXDE/LXQt */
        "caja",     /**< MATE */
        "nemo"      /**< Cinnamon */
    };

    for (const QString &fileManager : fileManagers) {
        success = QProcess::startDetached(fileManager, QStringList() << projectPath);
        if (success) {
            break;
        }
    }
#endif

    if (!success) {
        QMessageBox::warning(
            this,
            tr("Failed to Open Directory"),
            tr("Could not open the project directory in file explorer."));
    }
}

void MainWindow::on_actionRefresh_triggered()
{
    /* Check if there's an active project */
    if (!projectManager || projectManager->getProjectName().isEmpty()) {
        QMessageBox::information(this, tr("No Project Open"), tr("Please open a project first."));
        return;
    }

    /* Get current project name */
    const QString projectName = projectManager->getProjectName();

    /* Clear existing tree view */
    auto *model = qobject_cast<QStandardItemModel *>(ui->treeViewProjectFile->model());
    if (model) {
        model->clear();
        model->setHorizontalHeaderLabels(QStringList() << "Project Files");
    }

    /* Reload project tree view */
    setupProjectTreeView(projectName);

    /* Show confirmation message */
    statusBar()->showMessage(tr("Project view refreshed"), 2000);
}

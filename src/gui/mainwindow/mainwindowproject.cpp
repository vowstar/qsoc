// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "gui/mainwindow/mainwindow.h"

#include "./ui_mainwindow.h"

#include <QDebug>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QIcon>
#include <QMessageBox>
#include <QProcess>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QStatusBar>

void MainWindow::closeProject(bool silent)
{
    /* Get the tree view model */
    auto *model = qobject_cast<QStandardItemModel *>(ui->treeViewProjectFile->model());

    /* Check if model exists */
    if (model) {
        /* Clear all root items from the model */
        model->clear();

        /* Restore header after clearing */
        model->setHorizontalHeaderLabels(QStringList() << "Project Files");
    }

    /* Reset project manager state if needed */
    if (projectManager) {
        projectManager->setProjectName("");
    }

    /* Inform user that project is closed only if not silent mode */
    if (!silent) {
        statusBar()->showMessage(tr("Project closed"), 2000);
    }
}

/* Private helper function to setup the project tree view */
void MainWindow::setupProjectTreeView(const QString &projectName)
{
    /* Create/update tree view model */
    if (!ui->treeViewProjectFile->model()) {
        auto *model = new QStandardItemModel(this);
        model->setHorizontalHeaderLabels(QStringList() << "Project Files");
        ui->treeViewProjectFile->setModel(model);
    }

    /* Add project to tree view */
    auto *model = qobject_cast<QStandardItemModel *>(ui->treeViewProjectFile->model());
    if (model) {
        auto *projectItem = new QStandardItem(QString("%1.soc_pro").arg(projectName));

        /* Set icon using theme system */
        projectItem->setIcon(QIcon::fromTheme("applications-soc"));
        /* Store full path in item data */
        projectItem->setData(projectManager->getProjectPath(), Qt::UserRole);

        /* Add project directories as child nodes */
        auto *busDirItem = new QStandardItem(tr("Bus"));
        busDirItem->setIcon(QIcon::fromTheme("document-open"));
        busDirItem->setData(projectManager->getBusPath(), Qt::UserRole);
        projectItem->appendRow(busDirItem);

        auto *moduleDirItem = new QStandardItem(tr("Module"));
        moduleDirItem->setIcon(QIcon::fromTheme("document-open"));
        moduleDirItem->setData(projectManager->getModulePath(), Qt::UserRole);
        projectItem->appendRow(moduleDirItem);

        auto *schematicDirItem = new QStandardItem(tr("Schematic"));
        schematicDirItem->setIcon(QIcon::fromTheme("document-open"));
        schematicDirItem->setData(projectManager->getSchematicPath(), Qt::UserRole);
        projectItem->appendRow(schematicDirItem);

        auto *outputDirItem = new QStandardItem(tr("Output"));
        outputDirItem->setIcon(QIcon::fromTheme("document-open"));
        outputDirItem->setData(projectManager->getOutputPath(), Qt::UserRole);
        projectItem->appendRow(outputDirItem);

        /* Add Bus files (*.soc_bus) to Bus node */
        QDir        busDir(projectManager->getBusPath());
        QStringList busFilters;
        busFilters << "*.soc_bus";
        busDir.setNameFilters(busFilters);
        foreach (const QString busFileName, busDir.entryList(QDir::Files)) {
            auto *busFileItem = new QStandardItem(busFileName);
            busFileItem->setIcon(QIcon::fromTheme("applications-bus"));
            busFileItem->setData(busDir.filePath(busFileName), Qt::UserRole);
            busDirItem->appendRow(busFileItem);
        }
        /* Expand Bus node if it has children */
        if (busDirItem->hasChildren()) {
            ui->treeViewProjectFile->setExpanded(model->indexFromItem(busDirItem), true);
        }

        /* Add Module files (*.soc_mod) to Module node */
        QDir        moduleDir(projectManager->getModulePath());
        QStringList moduleFilters;
        moduleFilters << "*.soc_mod";
        moduleDir.setNameFilters(moduleFilters);
        foreach (const QString moduleFileName, moduleDir.entryList(QDir::Files)) {
            auto *moduleFileItem = new QStandardItem(moduleFileName);
            moduleFileItem->setIcon(QIcon::fromTheme("applications-module"));
            moduleFileItem->setData(moduleDir.filePath(moduleFileName), Qt::UserRole);
            moduleDirItem->appendRow(moduleFileItem);
        }
        /* Expand Module node if it has children */
        if (moduleDirItem->hasChildren()) {
            ui->treeViewProjectFile->setExpanded(model->indexFromItem(moduleDirItem), true);
        }

        /* Add Schematic files (*.soc_sch) to Schematic node */
        QDir        schematicDir(projectManager->getSchematicPath());
        QStringList schematicFilters;
        schematicFilters << "*.soc_sch";
        schematicDir.setNameFilters(schematicFilters);
        foreach (const QString schematicFileName, schematicDir.entryList(QDir::Files)) {
            auto *schematicFileItem = new QStandardItem(schematicFileName);
            schematicFileItem->setIcon(QIcon::fromTheme("applications-schematic"));
            schematicFileItem->setData(schematicDir.filePath(schematicFileName), Qt::UserRole);
            schematicDirItem->appendRow(schematicFileItem);
        }
        /* Expand Schematic node if it has children */
        if (schematicDirItem->hasChildren()) {
            ui->treeViewProjectFile->setExpanded(model->indexFromItem(schematicDirItem), true);
        }

        /* Add Output files to Output node - each file type separately */
        QDir outputDir(projectManager->getOutputPath());

        /* Add .soc_net files */
        outputDir.setNameFilters(QStringList() << "*.soc_net");
        foreach (const QString outputFileName, outputDir.entryList(QDir::Files)) {
            auto *outputFileItem = new QStandardItem(outputFileName);
            outputFileItem->setIcon(QIcon::fromTheme("applications-net"));
            outputFileItem->setData(outputDir.filePath(outputFileName), Qt::UserRole);
            outputDirItem->appendRow(outputFileItem);
        }

        /* Add .v (Verilog) files */
        outputDir.setNameFilters(QStringList() << "*.v");
        foreach (const QString outputFileName, outputDir.entryList(QDir::Files)) {
            auto *outputFileItem = new QStandardItem(outputFileName);
            outputFileItem->setIcon(QIcon::fromTheme("document-open"));
            outputFileItem->setData(outputDir.filePath(outputFileName), Qt::UserRole);
            outputDirItem->appendRow(outputFileItem);
        }

        /* Add .csv files */
        outputDir.setNameFilters(QStringList() << "*.csv");
        foreach (const QString outputFileName, outputDir.entryList(QDir::Files)) {
            auto *outputFileItem = new QStandardItem(outputFileName);
            outputFileItem->setIcon(QIcon::fromTheme("document-open"));
            outputFileItem->setData(outputDir.filePath(outputFileName), Qt::UserRole);
            outputDirItem->appendRow(outputFileItem);
        }
        /* Expand Output node if it has children */
        if (outputDirItem->hasChildren()) {
            ui->treeViewProjectFile->setExpanded(model->indexFromItem(outputDirItem), true);
        }

        model->appendRow(projectItem);
        /* Always expand the project item to show directories */
        ui->treeViewProjectFile->expand(model->indexFromItem(projectItem));
    }
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

void MainWindow::autoOpenSingleProject()
{
    /* Scan current directory for .soc_pro files */
    QDir        currentDir = QDir::current();
    QStringList projectFiles
        = currentDir.entryList(QStringList() << "*.soc_pro", QDir::Files, QDir::Name);

    /* Only auto-open if exactly one project file exists */
    if (projectFiles.count() != 1) {
        return;
    }

    /* Extract project information */
    const QString   projectFileName = projectFiles.first();
    const QString   projectFilePath = currentDir.filePath(projectFileName);
    const QFileInfo fileInfo(projectFilePath);
    const QString   projectName = fileInfo.baseName();
    const QString   projectDir  = fileInfo.absolutePath();

    /* Load project silently (no error dialogs on auto-open) */
    projectManager->setProjectPath(projectDir);
    if (projectManager->load(projectName)) {
        /* Setup project tree view on success */
        setupProjectTreeView(projectName);
    }
    /* Silently fail if load fails - user can manually open if needed */
}

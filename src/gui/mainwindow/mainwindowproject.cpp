// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "gui/mainwindow/mainwindow.h"

#include "./ui_mainwindow.h"
#include "common/qstringutils.h"

#include <QDir>
#include <QFileInfo>
#include <QIcon>
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

    /* Clear permanent status bar label */
    if (statusBarPermanentLabel) {
        statusBarPermanentLabel->clear();
    }

    /* Inform user that project is closed only if not silent mode */
    if (!silent) {
        statusBar()->showMessage(tr("Project closed"), 2000);
    }

    /* Update window title */
    updateWindowTitle();
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

    /* Update window title */
    updateWindowTitle();

    /* Display project path in permanent status bar label (60 char max) */
    if (statusBarPermanentLabel) {
        const QString projectPath = projectManager->getProjectPath() + "/"
                                    + projectManager->getProjectName() + ".soc_pro";
        const QString displayPath = QStringUtils::truncateMiddle(projectPath, 60);
        statusBarPermanentLabel->setText(QString("Project: %1").arg(displayPath));
    }
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

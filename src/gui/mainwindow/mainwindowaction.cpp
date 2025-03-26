#include "common/qsocprojectmanager.h"
#include "gui/mainwindow/mainwindow.h"

#include "./ui_mainwindow.h"

#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QIcon>
#include <QMessageBox>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QStatusBar>

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

void MainWindow::on_actionNewProject_triggered()
{
    /* Close current project first (silent mode) */
    closeProject(true);

    /* Show save dialog to get project name and path */
    QString filePath = QFileDialog::getSaveFileName(
        this,
        tr("Create New Project"),
        lastProjectDir,
        tr("QSoC Project (*.soc_pro);;All Files (*)"));

    /* Check if user canceled the dialog */
    if (filePath.isEmpty()) {
        return;
    }

    /* Extract project name and directory path */
    QFileInfo fileInfo(filePath);
    QString   projectName = fileInfo.baseName();
    QString   projectDir  = QDir(fileInfo.absolutePath()).filePath(projectName);

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

    /* Create/update tree view model */
    if (!ui->treeViewProjectFile->model()) {
        QStandardItemModel *model = new QStandardItemModel(this);
        model->setHorizontalHeaderLabels(QStringList() << "Project Files");
        ui->treeViewProjectFile->setModel(model);
    }

    /* Add new project to tree view */
    QStandardItemModel *model = qobject_cast<QStandardItemModel *>(ui->treeViewProjectFile->model());
    if (model) {
        QStandardItem *projectItem = new QStandardItem(QString("%1.soc_pro").arg(projectName));

        /* Set icon using theme system */
        projectItem->setIcon(QIcon::fromTheme("document-open"));
        /* Store full path in item data */
        projectItem->setData(projectManager->getProjectPath(), Qt::UserRole);

        /* Add project directories as child nodes */
        QStandardItem *busDirItem = new QStandardItem(tr("Bus"));
        busDirItem->setIcon(QIcon::fromTheme("document-open"));
        busDirItem->setData(projectManager->getBusPath(), Qt::UserRole);
        projectItem->appendRow(busDirItem);

        QStandardItem *moduleDirItem = new QStandardItem(tr("Module"));
        moduleDirItem->setIcon(QIcon::fromTheme("document-open"));
        moduleDirItem->setData(projectManager->getModulePath(), Qt::UserRole);
        projectItem->appendRow(moduleDirItem);

        QStandardItem *schematicDirItem = new QStandardItem(tr("Schematic"));
        schematicDirItem->setIcon(QIcon::fromTheme("document-open"));
        schematicDirItem->setData(projectManager->getSchematicPath(), Qt::UserRole);
        projectItem->appendRow(schematicDirItem);

        QStandardItem *outputDirItem = new QStandardItem(tr("Output"));
        outputDirItem->setIcon(QIcon::fromTheme("document-open"));
        outputDirItem->setData(projectManager->getOutputPath(), Qt::UserRole);
        projectItem->appendRow(outputDirItem);

        model->appendRow(projectItem);
        ui->treeViewProjectFile->expand(model->indexFromItem(projectItem));
    }
}

void MainWindow::on_actionOpenProject_triggered()
{
    /* Close current project first (silent mode) */
    closeProject(true);

    /* Show open dialog to get project file */
    QString filePath = QFileDialog::getOpenFileName(
        this, tr("Open Project"), lastProjectDir, tr("QSoC Project (*.soc_pro);;All Files (*)"));

    /* Check if user canceled the dialog */
    if (filePath.isEmpty()) {
        return;
    }

    /* Extract project name from file path */
    QFileInfo fileInfo(filePath);
    QString   projectName = fileInfo.baseName();
    QString   projectDir  = fileInfo.absolutePath();

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

    /* Create/update tree view model */
    if (!ui->treeViewProjectFile->model()) {
        QStandardItemModel *model = new QStandardItemModel(this);
        model->setHorizontalHeaderLabels(QStringList() << "Project Files");
        ui->treeViewProjectFile->setModel(model);
    }

    /* Add project to tree view */
    QStandardItemModel *model = qobject_cast<QStandardItemModel *>(ui->treeViewProjectFile->model());
    if (model) {
        QStandardItem *projectItem = new QStandardItem(QString("%1.soc_pro").arg(projectName));

        /* Set icon using theme system */
        projectItem->setIcon(QIcon::fromTheme("document-open"));
        /* Store full path in item data */
        projectItem->setData(projectManager->getProjectPath(), Qt::UserRole);

        /* Add project directories as child nodes */
        QStandardItem *busDirItem = new QStandardItem(tr("Bus"));
        busDirItem->setIcon(QIcon::fromTheme("document-open"));
        busDirItem->setData(projectManager->getBusPath(), Qt::UserRole);
        projectItem->appendRow(busDirItem);

        QStandardItem *moduleDirItem = new QStandardItem(tr("Module"));
        moduleDirItem->setIcon(QIcon::fromTheme("document-open"));
        moduleDirItem->setData(projectManager->getModulePath(), Qt::UserRole);
        projectItem->appendRow(moduleDirItem);

        QStandardItem *schematicDirItem = new QStandardItem(tr("Schematic"));
        schematicDirItem->setIcon(QIcon::fromTheme("document-open"));
        schematicDirItem->setData(projectManager->getSchematicPath(), Qt::UserRole);
        projectItem->appendRow(schematicDirItem);

        QStandardItem *outputDirItem = new QStandardItem(tr("Output"));
        outputDirItem->setIcon(QIcon::fromTheme("document-open"));
        outputDirItem->setData(projectManager->getOutputPath(), Qt::UserRole);
        projectItem->appendRow(outputDirItem);

        model->appendRow(projectItem);
        ui->treeViewProjectFile->expand(model->indexFromItem(projectItem));
    }
}

void MainWindow::on_actionCloseProject_triggered()
{
    closeProject(false);
}

void MainWindow::closeProject(bool silent)
{
    /* Get the tree view model */
    QStandardItemModel *model = qobject_cast<QStandardItemModel *>(ui->treeViewProjectFile->model());

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

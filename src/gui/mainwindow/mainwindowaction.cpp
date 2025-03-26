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
    /* Show save dialog to get project name and path */
    QString filePath = QFileDialog::getSaveFileName(
        this,
        tr("Create New Project"),
        QDir::homePath(),
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

    /* Create/update tree view model */
    if (!ui->treeViewProjectFile->model()) {
        QStandardItemModel *model = new QStandardItemModel(this);
        model->setHorizontalHeaderLabels(QStringList() << "Project Files");
        ui->treeViewProjectFile->setModel(model);
    }

    /* Add new project to tree view */
    QStandardItemModel *model = qobject_cast<QStandardItemModel *>(ui->treeViewProjectFile->model());
    if (model) {
        QStandardItem *projectItem = new QStandardItem(projectName);

        /* Set icon using theme system */
        projectItem->setIcon(QIcon::fromTheme("document-open"));
        /* Store full path in item data */
        projectItem->setData(projectDir, Qt::UserRole);

        /* List all subdirectories in projectDir */
        QDir          dir(projectDir);
        QFileInfoList dirList = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);

        /* Add each subdirectory to the tree */
        foreach (const QFileInfo &dirInfo, dirList) {
            QStandardItem *folderItem = new QStandardItem(dirInfo.fileName());
            folderItem->setIcon(QIcon::fromTheme("document-open"));
            folderItem->setData(dirInfo.absoluteFilePath(), Qt::UserRole);
            projectItem->appendRow(folderItem);
        }

        model->appendRow(projectItem);
        ui->treeViewProjectFile->expand(model->indexFromItem(projectItem));
    }
}

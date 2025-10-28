// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#include "gui/schematicwindow/schematicwindow.h"

#include "./ui_schematicwindow.h"

#include <QDebug>
#include <QFileDialog>
#include <QIcon>
#include <QMessageBox>
#include <QPrintDialog>
#include <QPrinter>
#include <QStandardPaths>

#include <gpds/archiver_yaml.hpp>
#include <gpds/serialize.hpp>
#include <qschematic/scene.hpp>

#include "common/qsocprojectmanager.h"

void SchematicWindow::on_actionQuit_triggered()
{
    close();
}

void SchematicWindow::on_actionShowGrid_triggered(bool checked)
{
    const QString iconName = checked ? "view-grid-on" : "view-grid-off";
    const QIcon   icon(QIcon::fromTheme(iconName));
    ui->actionShowGrid->setIcon(icon);
    settings.showGrid = checked;
    scene.setSettings(settings);
    ui->schematicView->setSettings(settings);
}

void SchematicWindow::on_actionSelectItem_triggered()
{
    qDebug() << "SchematicWindow: Switching to Normal Mode";
    ui->actionSelectItem->setChecked(true);
    ui->actionAddWire->setChecked(false);
    scene.setMode(QSchematic::Scene::NormalMode);
    qDebug() << "SchematicWindow: Current mode:" << scene.mode();
}

void SchematicWindow::on_actionAddWire_triggered()
{
    qDebug() << "SchematicWindow: Switching to Wire Mode";
    ui->actionAddWire->setChecked(true);
    ui->actionSelectItem->setChecked(false);
    scene.setMode(QSchematic::Scene::WireMode);
    qDebug() << "SchematicWindow: Current mode:" << scene.mode();
}

void SchematicWindow::on_actionUndo_triggered()
{
    if (scene.undoStack()->canUndo()) {
        scene.undoStack()->undo();
    }
}

void SchematicWindow::on_actionRedo_triggered()
{
    if (scene.undoStack()->canRedo()) {
        scene.undoStack()->redo();
    }
}

void SchematicWindow::on_actionPrint_triggered()
{
    QPrinter printer(QPrinter::HighResolution);
    if (QPrintDialog(&printer).exec() == QDialog::Accepted) {
        QPainter painter(&printer);
        painter.setRenderHint(QPainter::Antialiasing);
        scene.render(&painter);
    }
}

void SchematicWindow::on_actionSave_triggered()
{
    if (!projectManager) {
        QMessageBox::warning(this, tr("Save Error"), tr("No project manager available"));
        return;
    }

    QString defaultPath = projectManager->getSchematicPath();
    if (defaultPath.isEmpty()) {
        defaultPath = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    }

    QString fileName = QFileDialog::getSaveFileName(
        this, tr("Save Schematic"), defaultPath, tr("SOC Schematic Files (*.soc_sch)"));

    if (fileName.isEmpty()) {
        return;
    }

    if (!fileName.endsWith(".soc_sch")) {
        fileName += ".soc_sch";
    }

    // Use standard gpds API to serialize Scene directly
    const std::filesystem::path path = fileName.toStdString();
    const auto &[success, message]
        = gpds::to_file<gpds::archiver_yaml>(path, scene, QSchematic::Scene::gpds_name);

    if (!success) {
        QMessageBox::critical(
            this,
            tr("Save Error"),
            tr("Failed to save schematic: %1").arg(QString::fromStdString(message)));
        return;
    }

    QMessageBox::information(this, tr("Save Success"), tr("Schematic saved successfully"));
}

void SchematicWindow::on_actionOpen_triggered()
{
    if (!projectManager) {
        QMessageBox::warning(this, tr("Open Error"), tr("No project manager available"));
        return;
    }

    QString defaultPath = projectManager->getSchematicPath();
    if (defaultPath.isEmpty()) {
        defaultPath = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    }

    QString fileName = QFileDialog::getOpenFileName(
        this, tr("Open Schematic"), defaultPath, tr("SOC Schematic Files (*.soc_sch)"));

    if (fileName.isEmpty()) {
        return;
    }

    // Clear existing scene and undo stack
    scene.clear();
    scene.undoStack()->clear();

    // Use standard gpds API to deserialize Scene directly
    const std::filesystem::path path = fileName.toStdString();

    try {
        const auto &[success, message]
            = gpds::from_file<gpds::archiver_yaml>(path, scene, QSchematic::Scene::gpds_name);

        if (!success) {
            QMessageBox::critical(
                this,
                tr("Open Error"),
                tr("Failed to load schematic: %1").arg(QString::fromStdString(message)));
            return;
        }

        QMessageBox::information(this, tr("Open Success"), tr("Schematic loaded successfully"));
    } catch (const std::bad_optional_access &e) {
        QMessageBox::critical(
            this,
            tr("Open Error"),
            tr("Incompatible file format. This file was created with an older version.\n"
               "Please create a new schematic file."));
        return;
    } catch (const std::exception &e) {
        QMessageBox::critical(
            this, tr("Open Error"), tr("Failed to load schematic: %1").arg(e.what()));
        return;
    }
}

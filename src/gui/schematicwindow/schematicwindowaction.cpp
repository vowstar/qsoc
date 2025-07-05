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

#include <fstream>

#include <gpds/archiver_yaml.hpp>
#include <gpds/container.hpp>
#include <yaml-cpp/yaml.h>

#include "common/qsocprojectmanager.h"
#include "gui/schematicwindow/modulelibrary/socmoduleitem.h"

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

    try {
        gpds::container schematicData;

        // Save scene settings
        gpds::container settingsData;
        settingsData.add_value("showGrid", settings.showGrid);
        settingsData.add_value("gridSize", settings.gridSize);
        settingsData.add_value("debug", settings.debug);
        settingsData.add_value("routeStraightAngles", settings.routeStraightAngles);
        schematicData.add_value("settings", settingsData);

        // Save items
        gpds::container itemsData;
        const auto     &items = scene.items();

        for (const auto &item : items) {
            if (auto moduleItem = dynamic_cast<const ModuleLibrary::SocModuleItem *>(item.get())) {
                gpds::container itemData;
                itemData.add_value("name", moduleItem->moduleName().toStdString());
                itemData.add_value("x", moduleItem->pos().x());
                itemData.add_value("y", moduleItem->pos().y());
                itemData.add_value("width", moduleItem->size().width());
                itemData.add_value("height", moduleItem->size().height());
                itemData.add_value("rotation", moduleItem->rotation());

                // Save module YAML data
                YAML::Emitter emitter;
                emitter << moduleItem->moduleYaml();
                itemData.add_value("yaml", std::string(emitter.c_str()));

                itemsData.add_value("module", itemData);
            }
        }
        schematicData.add_value("items", itemsData);

        // TODO: Save wires (wire API needs investigation)
        gpds::container wiresData;
        schematicData.add_value("wires", wiresData);

        // Write to file
        gpds::archiver_yaml ar;
        std::ofstream       file(fileName.toStdString());
        ar.save(file, schematicData, "schematic");

        QMessageBox::information(this, tr("Save Success"), tr("Schematic saved successfully"));

    } catch (const std::exception &e) {
        QMessageBox::critical(
            this, tr("Save Error"), tr("Failed to save schematic: %1").arg(e.what()));
    }
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

    try {
        gpds::container     schematicData;
        gpds::archiver_yaml ar;

        std::ifstream file(fileName.toStdString());
        if (!ar.load(file, schematicData, "schematic")) {
            QMessageBox::critical(this, tr("Open Error"), tr("Failed to load schematic file"));
            return;
        }

        // Clear existing scene
        scene.clear();

        // Load settings
        if (auto settingsOpt = schematicData.get_value<gpds::container>("settings")) {
            const auto &settingsData = *settingsOpt;

            if (auto showGridOpt = settingsData.get_value<bool>("showGrid")) {
                settings.showGrid = *showGridOpt;
                ui->actionShowGrid->setChecked(settings.showGrid);
            }
            if (auto gridSizeOpt = settingsData.get_value<int>("gridSize")) {
                settings.gridSize = *gridSizeOpt;
            }
            if (auto debugOpt = settingsData.get_value<bool>("debug")) {
                settings.debug = *debugOpt;
            }
            if (auto routeAnglesOpt = settingsData.get_value<bool>("routeStraightAngles")) {
                settings.routeStraightAngles = *routeAnglesOpt;
            }
        }

        // Apply settings
        scene.setSettings(settings);
        ui->schematicView->setSettings(settings);

        // Load items
        if (auto itemsOpt = schematicData.get_value<gpds::container>("items")) {
            const auto &itemsData = *itemsOpt;

            // Try to get all module entries
            for (size_t i = 0;; ++i) {
                std::string key       = "module[" + std::to_string(i) + "]";
                auto        moduleOpt = itemsData.get_value<gpds::container>(key);
                if (!moduleOpt) {
                    // Try without index
                    moduleOpt = itemsData.get_value<gpds::container>("module");
                    if (!moduleOpt) {
                        break;
                    }
                }

                const auto &itemData = *moduleOpt;

                auto nameOpt     = itemData.get_value<std::string>("name");
                auto xOpt        = itemData.get_value<double>("x");
                auto yOpt        = itemData.get_value<double>("y");
                auto widthOpt    = itemData.get_value<double>("width");
                auto heightOpt   = itemData.get_value<double>("height");
                auto rotationOpt = itemData.get_value<double>("rotation");
                auto yamlOpt     = itemData.get_value<std::string>("yaml");

                if (nameOpt && xOpt && yOpt && widthOpt && heightOpt && rotationOpt && yamlOpt) {
                    QString moduleName = QString::fromStdString(*nameOpt);

                    // Load YAML data
                    YAML::Node moduleYaml = YAML::Load(*yamlOpt);

                    // Create module item
                    auto moduleItem = std::make_shared<ModuleLibrary::SocModuleItem>(
                        moduleName, moduleYaml, QSchematic::Items::Item::NodeType);

                    moduleItem->setPos(*xOpt, *yOpt);
                    moduleItem->setSize(*widthOpt, *heightOpt);
                    moduleItem->setRotation(*rotationOpt);
                    moduleItem->setSettings(settings);

                    // Add to scene
                    scene.addItem(moduleItem);
                }

                // If this was a single module entry, break
                if (!itemsData.get_value<gpds::container>("module[1]")) {
                    break;
                }
            }
        }

        QMessageBox::information(this, tr("Open Success"), tr("Schematic loaded successfully"));

    } catch (const std::exception &e) {
        QMessageBox::critical(
            this, tr("Open Error"), tr("Failed to open schematic: %1").arg(e.what()));
    }
}

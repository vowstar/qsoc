// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#include "gui/schematicwindow/schematicwindow.h"
#include "gui/schematicwindow/modulelibrary/modulewidget.h"

#include "./ui_schematicwindow.h"

#include <qschematic/commands/item_add.hpp>
#include <qschematic/items/item.hpp>

#include <QGridLayout>

SchematicWindow::SchematicWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::SchematicWindow)
    , moduleLibraryWidget(nullptr)
{
    ui->setupUi(this);

    settings.debug               = false;
    settings.showGrid            = true;
    settings.routeStraightAngles = true;

    connect(&scene, &QSchematic::Scene::modeChanged, [this](int mode) {
        switch (mode) {
        case QSchematic::Scene::NormalMode:
            on_actionSelectItem_triggered();
            break;

        case QSchematic::Scene::WireMode:
            on_actionAddWire_triggered();
            break;

        default:
            break;
        }
    });

    ui->actionUndo->setEnabled(scene.undoStack()->canUndo());
    ui->actionRedo->setEnabled(scene.undoStack()->canRedo());

    connect(scene.undoStack(), &QUndoStack::canUndoChanged, [this](bool canUndo) {
        ui->actionUndo->setEnabled(canUndo);
    });
    connect(scene.undoStack(), &QUndoStack::canRedoChanged, [this](bool canRedo) {
        ui->actionRedo->setEnabled(canRedo);
    });

    scene.setParent(ui->schematicView);
    scene.setSettings(settings);
    ui->schematicView->setSettings(settings);
    ui->schematicView->setScene(&scene);

    ui->undoViewCommandHistory->setStack(scene.undoStack());

    scene.clear();
    scene.setSceneRect(-500, -500, 3000, 3000);

    /* Initialize the module library */
    initializeModuleLibrary();
}

SchematicWindow::~SchematicWindow()
{
    delete ui;
}

void SchematicWindow::initializeModuleLibrary()
{
    /* Create the module library widget */
    moduleLibraryWidget = new ModuleLibrary::ModuleWidget(this);

    /* Connect signals/slots for module library */
    connect(
        moduleLibraryWidget,
        &ModuleLibrary::ModuleWidget::itemClicked,
        this,
        &SchematicWindow::addModuleToSchematic);
    connect(
        ui->schematicView,
        &QSchematic::View::zoomChanged,
        moduleLibraryWidget,
        &ModuleLibrary::ModuleWidget::setPixmapScale);

    /* Add the module library widget to the dock widget */
    QWidget     *dockContents = ui->dockWidgetModuleList->widget();
    QGridLayout *layout       = new QGridLayout(dockContents);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(moduleLibraryWidget);
    dockContents->setLayout(layout);
}

void SchematicWindow::addModuleToSchematic(const QSchematic::Items::Item *item)
{
    if (!item) {
        return;
    }

    /* Create a deep copy of the item */
    std::shared_ptr<QSchematic::Items::Item> itemCopy = item->deepCopy();
    if (!itemCopy) {
        return;
    }

    /* Set item position to view center */
    QPointF viewCenter = ui->schematicView->mapToScene(
        ui->schematicView->viewport()->rect().center());
    itemCopy->setPos(viewCenter);

    /* Add to scene */
    scene.undoStack()->push(new QSchematic::Commands::ItemAdd(&scene, std::move(itemCopy)));
}

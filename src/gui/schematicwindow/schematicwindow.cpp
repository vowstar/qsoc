// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#include "gui/schematicwindow/schematicwindow.h"
#include "common/qsocmodulemanager.h"
#include "common/qsocprojectmanager.h"
#include "gui/schematicwindow/modulelibrary/customitemfactory.h"
#include "gui/schematicwindow/modulelibrary/modulewidget.h"

#include "./ui_schematicwindow.h"

#include <qschematic/commands/item_add.hpp>
#include <qschematic/items/item.hpp>
#include <qschematic/items/itemfactory.hpp>

#include <QFileDialog>
#include <QGridLayout>
#include <QMessageBox>
#include <QStandardPaths>

#include <functional>

#include <gpds/archiver_xml.hpp>
#include <gpds/container.hpp>

SchematicWindow::SchematicWindow(QWidget *parent, QSocProjectManager *projectManager)
    : QMainWindow(parent)
    , ui(new Ui::SchematicWindow)
    , moduleLibraryWidget(nullptr)
    , moduleManager(nullptr)
    , projectManager(projectManager)
{
    qDebug() << "SchematicWindow: Constructor called with projectManager:"
             << (projectManager ? "valid" : "null");

    qDebug() << "SchematicWindow: Setting up UI";
    ui->setupUi(this);
    qDebug() << "SchematicWindow: UI setup completed";

    // Register custom item factory for SocModuleItem
    auto factoryFunc
        = std::bind(&ModuleLibrary::CustomItemFactory::from_container, std::placeholders::_1);
    QSchematic::Items::Factory::instance().setCustomItemsFactory(factoryFunc);

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

    /* Initialize module manager */
    if (projectManager) {
        moduleManager = new QSocModuleManager(this, projectManager);
    }

    /* Initialize the module library */
    qDebug() << "SchematicWindow: Initializing module library";
    initializeModuleLibrary();
    qDebug() << "SchematicWindow: Module library initialized";

    qDebug() << "SchematicWindow: Constructor completed successfully";
}

SchematicWindow::~SchematicWindow()
{
    delete ui;
}

void SchematicWindow::initializeModuleLibrary()
{
    qDebug() << "SchematicWindow::initializeModuleLibrary: Starting with moduleManager:"
             << (moduleManager ? "valid" : "null");

    /* Create the module library widget */
    qDebug() << "SchematicWindow::initializeModuleLibrary: Creating ModuleWidget";
    moduleLibraryWidget = new ModuleLibrary::ModuleWidget(this, moduleManager);
    qDebug() << "SchematicWindow::initializeModuleLibrary: ModuleWidget created successfully";

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
    QWidget *dockContents = ui->dockWidgetModuleList->widget();
    auto    *layout       = new QGridLayout(dockContents);
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
    const std::shared_ptr<QSchematic::Items::Item> itemCopy = item->deepCopy();
    if (!itemCopy) {
        return;
    }

    /* Set item position to view center */
    const QPointF viewCenter = ui->schematicView->mapToScene(
        ui->schematicView->viewport()->rect().center());
    itemCopy->setPos(viewCenter);

    /* Add to scene */
    scene.undoStack()->push(new QSchematic::Commands::ItemAdd(&scene, itemCopy));
}

void SchematicWindow::setProjectManager(QSocProjectManager *projectManager)
{
    if (!projectManager) {
        return;
    }

    this->projectManager = projectManager;

    /* Initialize or update module manager */
    if (!moduleManager) {
        moduleManager = new QSocModuleManager(this, projectManager);

        /* Recreate the module library widget with the new module manager */
        if (moduleLibraryWidget) {
            /* Remove the old widget from layout */
            QWidget *dockContents = ui->dockWidgetModuleList->widget();
            if (dockContents->layout()) {
                QLayoutItem *item = dockContents->layout()->takeAt(0);
                if (item) {
                    delete item; // Only delete the layout item, not the widget
                }
            }
            delete moduleLibraryWidget; // Delete the widget directly
            moduleLibraryWidget = nullptr;
        }

        /* Create new module library widget with module manager */
        moduleLibraryWidget = new ModuleLibrary::ModuleWidget(this, moduleManager);

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
        QWidget *dockContents = ui->dockWidgetModuleList->widget();
        if (dockContents->layout()) {
            delete dockContents->layout();
        }
        auto *layout = new QGridLayout(dockContents);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(moduleLibraryWidget);
        dockContents->setLayout(layout);

        /* Expand all items initially */
        moduleLibraryWidget->expandAll();
    } else {
        /* Update existing module manager */
        moduleManager->setProjectManager(projectManager);

        /* Refresh the module list */
        if (moduleLibraryWidget) {
            moduleLibraryWidget->setModuleManager(moduleManager);
        }
    }
}

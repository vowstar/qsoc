// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#include "gui/schematicwindow/schematicwindow.h"
#include "common/qsocmodulemanager.h"
#include "common/qsocprojectmanager.h"
#include "gui/schematicwindow/customwire.h"
#include "gui/schematicwindow/modulelibrary/customitemfactory.h"
#include "gui/schematicwindow/modulelibrary/modulewidget.h"
#include "gui/schematicwindow/modulelibrary/socmoduleitem.h"

#include "./ui_schematicwindow.h"

#include <qschematic/commands/item_add.hpp>
#include <qschematic/commands/item_remove.hpp>
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
    , m_currentFilePath("")
{
    ui->setupUi(this);

    // Register custom item factory for SocModuleItem
    auto factoryFunc
        = std::bind(&ModuleLibrary::CustomItemFactory::from_container, std::placeholders::_1);
    QSchematic::Items::Factory::instance().setCustomItemsFactory(factoryFunc);

    // Register custom wire factory for bus visualization
    scene.setWireFactory([]() -> std::shared_ptr<QSchematic::Items::Wire> {
        return std::make_shared<SchematicCustom::CustomWire>();
    });

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

    /* Auto-name wires when netlist changes */
    connect(&scene, &QSchematic::Scene::netlistChanged, this, &SchematicWindow::autoNameWires);

    /* Auto-generate instance names when items are added (drag/drop, paste, etc.) */
    connect(&scene, &QSchematic::Scene::itemAdded, this, &SchematicWindow::onItemAdded);

    ui->actionUndo->setEnabled(scene.undoStack()->canUndo());
    ui->actionRedo->setEnabled(scene.undoStack()->canRedo());

    connect(scene.undoStack(), &QUndoStack::canUndoChanged, [this](bool canUndo) {
        ui->actionUndo->setEnabled(canUndo);
    });
    connect(scene.undoStack(), &QUndoStack::canRedoChanged, [this](bool canRedo) {
        ui->actionRedo->setEnabled(canRedo);
    });
    connect(scene.undoStack(), &QUndoStack::cleanChanged, this, &SchematicWindow::updateWindowTitle);

    scene.setParent(ui->schematicView);
    scene.setSettings(settings);
    ui->schematicView->setSettings(settings);
    ui->schematicView->setScene(&scene);

    /* Ensure view can receive keyboard events */
    ui->schematicView->setFocusPolicy(Qt::StrongFocus);
    ui->schematicView->setFocus();

    ui->undoViewCommandHistory->setStack(scene.undoStack());

    scene.clear();
    scene.setSceneRect(-500, -500, 3000, 3000);

    /* Install event filter for double-click and ShortcutOverride handling */
    ui->schematicView->installEventFilter(this); // For ShortcutOverride (Delete key fix)
    ui->schematicView->viewport()->installEventFilter(this); // For double-click events

    /* Initialize module manager */
    if (projectManager) {
        moduleManager = new QSocModuleManager(this, projectManager);
    }

    /* Initialize the module library */
    initializeModuleLibrary();

    /* Set scene reference for module library (for generating unique names during drag) */
    if (moduleLibraryWidget) {
        moduleLibraryWidget->setScene(&scene);
    }

    /* Set initial window title */
    updateWindowTitle();
}

SchematicWindow::~SchematicWindow()
{
    delete ui;
}

void SchematicWindow::initializeModuleLibrary()
{
    /* Create the module library widget */
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

    /* Generate unique instance name for SocModuleItem */
    auto socModuleItem = std::dynamic_pointer_cast<ModuleLibrary::SocModuleItem>(itemCopy);
    if (socModuleItem) {
        QString moduleName   = socModuleItem->moduleName();
        QString instanceName = generateUniqueInstanceName(scene, moduleName);
        socModuleItem->setInstanceName(instanceName);
    }

    /* Set item position to view center */
    const QPointF viewCenter = ui->schematicView->mapToScene(
        ui->schematicView->viewport()->rect().center());
    itemCopy->setPos(viewCenter);

    /* Add to scene */
    scene.undoStack()->push(new QSchematic::Commands::ItemAdd(&scene, itemCopy));
}

QSet<QString> SchematicWindow::getExistingInstanceNames(const QSchematic::Scene &scene)
{
    QSet<QString> existingNames;
    for (const auto &node : scene.nodes()) {
        auto socItem = std::dynamic_pointer_cast<ModuleLibrary::SocModuleItem>(node);
        if (socItem) {
            existingNames.insert(socItem->instanceName());
        }
    }
    return existingNames;
}

QString SchematicWindow::generateUniqueInstanceName(
    const QSchematic::Scene &scene, const QString &moduleName)
{
    const QSet<QString> existingNames = getExistingInstanceNames(scene);

    /* Find unique name: u_<modulename>_N */
    int     index = 0;
    QString candidateName;
    do {
        candidateName = QString("u_%1_%2").arg(moduleName).arg(index++);
    } while (existingNames.contains(candidateName));

    return candidateName;
}

void SchematicWindow::onItemAdded(std::shared_ptr<QSchematic::Items::Item> item)
{
    /* Only process SocModuleItems */
    auto socItem = std::dynamic_pointer_cast<ModuleLibrary::SocModuleItem>(item);
    if (!socItem) {
        return;
    }

    QString moduleName   = socItem->moduleName();
    QString instanceName = socItem->instanceName();

    /* Determine if we need a new unique name */
    bool needsUniqueName = (instanceName == moduleName); // Fresh from library

    if (!needsUniqueName) {
        /* Check for name conflicts with existing instances */
        const QSet<QString> existingNames = getExistingInstanceNames(scene);
        if (existingNames.contains(instanceName)) {
            needsUniqueName = true;
        }
    }

    /* Generate and assign unique name if needed */
    if (needsUniqueName) {
        QString uniqueName = generateUniqueInstanceName(scene, moduleName);
        socItem->setInstanceName(uniqueName);
    }
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

        /* Set scene reference for drag preview */
        moduleLibraryWidget->setScene(&scene);

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

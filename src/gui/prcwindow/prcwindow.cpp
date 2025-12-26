// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#include "gui/prcwindow/prcwindow.h"
#include "common/qsocprojectmanager.h"
#include "gui/prcwindow/prcconfigdialog.h"
#include "gui/prcwindow/prcitemfactory.h"
#include "gui/prcwindow/prclibrarywidget.h"
#include "gui/prcwindow/prcprimitiveitem.h"

#include "./ui_prcwindow.h"

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

/**
 * @brief Construct PRC editor window
 * @param[in] parent Parent widget
 * @param[in] projectManager Project manager instance
 */
PrcWindow::PrcWindow(QWidget *parent, QSocProjectManager *projectManager)
    : QMainWindow(parent)
    , ui(new Ui::PrcWindow)
    , prcLibraryWidget(nullptr)
    , projectManager(projectManager)
    , m_currentFilePath("")
{
    ui->setupUi(this);

    /* Setup permanent status bar label */
    statusBarPermanentLabel = new QLabel(this);
    statusBarPermanentLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    statusBar()->addPermanentWidget(statusBarPermanentLabel, 1);

    /* Register custom item factory for PRC items */
    auto factoryFunc = std::bind(&PrcLibrary::PrcItemFactory::from_container, std::placeholders::_1);
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

    /* Auto-name wires and update dynamic ports when netlist changes */
    connect(&scene, &QSchematic::Scene::netlistChanged, this, &PrcWindow::autoNameWires);
    connect(&scene, &QSchematic::Scene::netlistChanged, this, &PrcWindow::updateAllDynamicPorts);

    /* Auto-generate instance names when items are added (drag/drop, paste, etc.) */
    connect(&scene, &QSchematic::Scene::itemAdded, this, &PrcWindow::onItemAdded);

    /* Handle controller edit request from context menu */
    connect(
        &scene,
        &PrcLibrary::PrcScene::editControllerRequested,
        this,
        &PrcWindow::handleEditController);

    ui->actionUndo->setEnabled(scene.undoStack()->canUndo());
    ui->actionRedo->setEnabled(scene.undoStack()->canRedo());

    connect(scene.undoStack(), &QUndoStack::canUndoChanged, [this](bool canUndo) {
        ui->actionUndo->setEnabled(canUndo);
    });
    connect(scene.undoStack(), &QUndoStack::canRedoChanged, [this](bool canRedo) {
        ui->actionRedo->setEnabled(canRedo);
    });
    connect(scene.undoStack(), &QUndoStack::cleanChanged, this, &PrcWindow::updateWindowTitle);

    scene.setParent(ui->prcView);
    scene.setSettings(settings);
    ui->prcView->setSettings(settings);
    ui->prcView->setScene(&scene);

    /* Ensure view can receive keyboard events */
    ui->prcView->setFocusPolicy(Qt::StrongFocus);
    ui->prcView->setFocus();

    ui->undoViewCommandHistory->setStack(scene.undoStack());

    scene.clear();
    scene.setSceneRect(-500, -500, 3000, 3000);

    /* Install event filter for double-click and ShortcutOverride handling */
    ui->prcView->installEventFilter(this);             // For ShortcutOverride (Delete key fix)
    ui->prcView->viewport()->installEventFilter(this); // For double-click events

    /* Initialize the PRC library */
    initializePrcLibrary();

    /* Set scene for library widget */
    if (prcLibraryWidget) {
        prcLibraryWidget->setScene(&scene);
    }

    /* Set initial window title */
    updateWindowTitle();
}

/**
 * @brief Destructor
 */
PrcWindow::~PrcWindow()
{
    delete ui;
}

/**
 * @brief Initialize PRC library widget and connect signals
 */
void PrcWindow::initializePrcLibrary()
{
    /* Create the PRC library widget */
    prcLibraryWidget = new PrcLibrary::PrcLibraryWidget(this);

    /* Add the PRC library widget to the dock widget */
    QWidget *dockContents = ui->dockWidgetPrcList->widget();
    if (!dockContents) {
        dockContents = new QWidget();
        ui->dockWidgetPrcList->setWidget(dockContents);
    }

    auto *layout = new QGridLayout(dockContents);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(prcLibraryWidget);
    dockContents->setLayout(layout);
}

/**
 * @brief Collect all existing PRC controller names from scene
 * @param[in] scene QSchematic scene to scan
 * @return Set of existing controller names
 */
QSet<QString> PrcWindow::getExistingControllerNames(const QSchematic::Scene &scene)
{
    QSet<QString> existingNames;
    for (const auto &node : scene.nodes()) {
        auto prcItem = std::dynamic_pointer_cast<PrcLibrary::PrcPrimitiveItem>(node);
        if (prcItem) {
            existingNames.insert(prcItem->primitiveName());
        }
    }
    return existingNames;
}

/**
 * @brief Generate unique controller name with auto-increment suffix
 * @param[in] scene QSchematic scene to scan
 * @param[in] prefix Name prefix (e.g., "clk_src_")
 * @return Unique controller name
 */
QString PrcWindow::generateUniqueControllerName(const QSchematic::Scene &scene, const QString &prefix)
{
    const QSet<QString> existingNames = getExistingControllerNames(scene);

    int     index = 0;
    QString candidateName;
    do {
        candidateName = QString("%1%2").arg(prefix).arg(index++);
    } while (existingNames.contains(candidateName));

    return candidateName;
}

/**
 * @brief Handle item added to scene (drag-drop from library)
 * @param[in] item Added item
 */
void PrcWindow::onItemAdded(std::shared_ptr<QSchematic::Items::Item> item)
{
    /* Check if it's a new PrcPrimitiveItem that needs configuration */
    auto prcItem = std::dynamic_pointer_cast<PrcLibrary::PrcPrimitiveItem>(item);
    if (prcItem && prcItem->needsConfiguration()) {
        /* Clear the flag and show configuration dialog */
        prcItem->setNeedsConfiguration(false);
        handlePrcItemDoubleClick(prcItem.get());
    }
}

/**
 * @brief Set project manager reference
 * @param[in] projectManager Project manager instance
 */
void PrcWindow::setProjectManager(QSocProjectManager *projectManager)
{
    if (!projectManager) {
        return;
    }

    this->projectManager = projectManager;
}

/**
 * @brief Get access to the PRC scene
 * @return Reference to PrcScene
 */
PrcLibrary::PrcScene &PrcWindow::prcScene()
{
    return scene;
}

/**
 * @brief Get read-only access to the PRC scene
 * @return Const reference to PrcScene
 */
const PrcLibrary::PrcScene &PrcWindow::prcScene() const
{
    return scene;
}

/**
 * @brief Handle controller edit request from context menu
 * @param[in] type Controller type (ClockCtrl/ResetCtrl/PowerCtrl)
 * @param[in] name Controller name
 */
void PrcWindow::handleEditController(int type, const QString &name)
{
    /* Convert scene type to dialog type */
    PrcLibrary::PrcControllerDialog::ControllerType ctrlType;
    switch (static_cast<PrcLibrary::PrcScene::ControllerType>(type)) {
    case PrcLibrary::PrcScene::ClockCtrl:
        ctrlType = PrcLibrary::PrcControllerDialog::ClockController;
        break;
    case PrcLibrary::PrcScene::ResetCtrl:
        ctrlType = PrcLibrary::PrcControllerDialog::ResetController;
        break;
    case PrcLibrary::PrcScene::PowerCtrl:
        ctrlType = PrcLibrary::PrcControllerDialog::PowerController;
        break;
    default:
        return;
    }

    /* Show controller dialog */
    PrcLibrary::PrcControllerDialog dialog(ctrlType, name, &scene, this);

    /* Handle delete request */
    connect(&dialog, &PrcLibrary::PrcControllerDialog::deleteRequested, [this, name, type]() {
        switch (static_cast<PrcLibrary::PrcScene::ControllerType>(type)) {
        case PrcLibrary::PrcScene::ClockCtrl:
            scene.removeClockController(name);
            break;
        case PrcLibrary::PrcScene::ResetCtrl:
            scene.removeResetController(name);
            break;
        case PrcLibrary::PrcScene::PowerCtrl:
            scene.removePowerController(name);
            break;
        }
    });

    if (dialog.exec() == QDialog::Accepted) {
        /* Apply controller changes */
        switch (ctrlType) {
        case PrcLibrary::PrcControllerDialog::ClockController:
            scene.setClockController(name, dialog.getClockControllerDef());
            break;
        case PrcLibrary::PrcControllerDialog::ResetController:
            scene.setResetController(name, dialog.getResetControllerDef());
            break;
        case PrcLibrary::PrcControllerDialog::PowerController:
            scene.setPowerController(name, dialog.getPowerControllerDef());
            break;
        }
    }
}

// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#include "gui/prcwindow/prcwindow.h"
#include "common/qsocprojectmanager.h"
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
    connect(&scene, &QSchematic::Scene::netlistChanged, this, &PrcWindow::autoNameWires);

    /* Auto-generate instance names when items are added (drag/drop, paste, etc.) */
    connect(&scene, &QSchematic::Scene::itemAdded, this, &PrcWindow::onItemAdded);

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

    /* Connect signals */
    connect(
        prcLibraryWidget,
        &PrcLibrary::PrcLibraryWidget::primitiveSelected,
        this,
        &PrcWindow::onPrimitiveSelected);

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
 * @brief Handle primitive selection from library
 * @param[in] primitiveType Type of primitive to create
 */
void PrcWindow::onPrimitiveSelected(PrcLibrary::PrimitiveType primitiveType)
{
    /* Create unique name for the primitive */
    QString prefix;
    switch (primitiveType) {
    case PrcLibrary::ClockSource:
        prefix = "clk_src_";
        break;
    case PrcLibrary::ClockTarget:
        prefix = "clk_tgt_";
        break;
    case PrcLibrary::ResetSource:
        prefix = "rst_src_";
        break;
    case PrcLibrary::ResetTarget:
        prefix = "rst_tgt_";
        break;
    case PrcLibrary::PowerDomain:
        prefix = "pwr_dom_";
        break;
    }

    QString uniqueName = generateUniqueControllerName(scene, prefix);

    /* Create the primitive item */
    auto item = std::make_shared<PrcLibrary::PrcPrimitiveItem>(primitiveType, uniqueName);

    /* Place at center of current view */
    QPointF viewCenter = ui->prcView->mapToScene(ui->prcView->viewport()->rect().center());
    item->setPos(viewCenter);

    /* Add to scene using undo command */
    scene.undoStack()->push(new QSchematic::Commands::ItemAdd(&scene, item));

    /* Show configuration dialog */
    handlePrcItemDoubleClick(item.get());
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
 * @brief Handle item added to scene
 * @param[in] item Added item
 */
void PrcWindow::onItemAdded(std::shared_ptr<QSchematic::Items::Item> item)
{
    Q_UNUSED(item);
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

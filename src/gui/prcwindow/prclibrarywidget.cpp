// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#include "prclibrarywidget.h"

#include <qschematic/items/itemmimedata.hpp>
#include <qschematic/scene.hpp>

#include <QBoxLayout>
#include <QDebug>
#include <QDrag>
#include <QIcon>

using namespace PrcLibrary;

/* PrcLibraryListWidget Implementation */

PrcLibraryListWidget::PrcLibraryListWidget(QWidget *parent)
    : QListWidget(parent)
    , scene(nullptr)
{
    setDragDropMode(QAbstractItemView::DragOnly);
    setDragEnabled(true);
    setSelectionMode(QAbstractItemView::SingleSelection);
}

void PrcLibraryListWidget::setScene(QSchematic::Scene *scene)
{
    this->scene = scene;
}

void PrcLibraryListWidget::startDrag(Qt::DropActions supportedActions)
{
    QListWidgetItem *currentItem = this->currentItem();
    if (!currentItem) {
        return;
    }

    /* Get primitive type from item data */
    int           typeInt = currentItem->data(Qt::UserRole).toInt();
    PrimitiveType type    = static_cast<PrimitiveType>(typeInt);

    /* Generate unique name prefix based on type */
    QString prefix;
    switch (type) {
    case ClockInput:
    case ClockTarget:
        prefix = "clk_";
        break;
    case ResetSource:
    case ResetTarget:
        prefix = "rst_";
        break;
    case PowerDomain:
        prefix = "pd_";
        break;
    }

    /* Generate unique name if scene is available */
    QString uniqueName = prefix + "0";
    if (scene) {
        QSet<QString> existingNames;
        for (const auto &node : scene->nodes()) {
            auto prcItem = std::dynamic_pointer_cast<PrcPrimitiveItem>(node);
            if (prcItem) {
                existingNames.insert(prcItem->primitiveName());
            }
        }

        int index = 0;
        do {
            uniqueName = QString("%1%2").arg(prefix).arg(index++);
        } while (existingNames.contains(uniqueName));
    }

    /* Create the primitive item for dragging */
    auto item = std::make_shared<PrcPrimitiveItem>(type, uniqueName);
    item->setNeedsConfiguration(true); /* Mark for config dialog after drop */

    /* Create MimeData with the item */
    auto *mimeData = new QSchematic::Items::MimeData(item);

    /* Create the drag object with preview */
    auto   *drag = new QDrag(this);
    QPointF hotSpot;
    drag->setMimeData(mimeData);
    drag->setPixmap(item->toPixmap(hotSpot, 1.0));
    drag->setHotSpot(hotSpot.toPoint());

    /* Execute the drag */
    drag->exec(supportedActions, Qt::CopyAction);
}

/* PrcLibraryWidget Implementation */

PrcLibraryWidget::PrcLibraryWidget(QWidget *parent)
    : QWidget(parent)
    , listWidget(nullptr)
    , scene(nullptr)
{
    listWidget = new PrcLibraryListWidget(this);
    listWidget->setViewMode(QListView::ListMode);
    listWidget->setResizeMode(QListView::Adjust);
    listWidget->setIconSize(QSize(32, 32));
    listWidget->setSpacing(2);

    /* Layout */
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(listWidget);
    layout->setContentsMargins(0, 0, 0, 0);
    setLayout(layout);

    initializeLibrary();
}

void PrcLibraryWidget::setScene(QSchematic::Scene *scene)
{
    this->scene = scene;
    listWidget->setScene(scene);
}

void PrcLibraryWidget::initializeLibrary()
{
    struct PrimitiveInfo
    {
        PrimitiveType type;        /**< Primitive type enum */
        QString       name;        /**< Display name */
        QString       description; /**< Tooltip description */
        QColor        color;       /**< Icon color */
    };

    const QList<PrimitiveInfo> primitives = {
        /* Clock Domain */
        {ClockInput, "Clock Input", "Clock input source (input:)", QColor(173, 216, 230)},
        {ClockTarget,
         "Clock Target",
         "Clock target with MUX/ICG/DIV (target:)",
         QColor(144, 238, 144)},

        /* Reset Domain */
        {ResetSource, "Reset Source", "Reset source signal (source:)", QColor(255, 182, 193)},
        {ResetTarget,
         "Reset Target",
         "Reset target with synchronizer (target:)",
         QColor(255, 160, 160)},

        /* Power Domain */
        {PowerDomain,
         "Power Domain",
         "Power domain with dependencies (domain:)",
         QColor(144, 238, 144)},
    };

    for (const auto &prim : primitives) {
        auto *item = new QListWidgetItem(prim.name);
        item->setToolTip(prim.description);
        item->setData(Qt::UserRole, static_cast<int>(prim.type));

        QPixmap pixmap(32, 32);
        pixmap.fill(prim.color);
        item->setIcon(QIcon(pixmap));

        listWidget->addItem(item);
    }
}

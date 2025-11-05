// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#include "schematiclibraryview.h"
#include "schematicmodule.h"

#include <qschematic/items/item.hpp>
#include <qschematic/items/itemmimedata.hpp>
#include <qschematic/scene.hpp>

#include "gui/schematicwindow/schematicwindow.h"

#include <QDebug>
#include <QDrag>
#include <QPainter>
#include <QSet>

SchematicLibraryView::SchematicLibraryView(QWidget *parent)
    : QTreeView(parent)
    , scale_(1.0)
    , scene_(nullptr)
{
    /* Configuration */
    setDragDropMode(QAbstractItemView::DragOnly);
    setDragEnabled(true);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setHeaderHidden(true);
    setIconSize(QSize(28, 28));
}

void SchematicLibraryView::setPixmapScale(qreal scale)
{
    scale_ = scale;
}

void SchematicLibraryView::setScene(QSchematic::Scene *scene)
{
    scene_ = scene;
}

void SchematicLibraryView::startDrag(Qt::DropActions supportedActions)
{
    const QModelIndexList indexes = selectedIndexes();
    if (indexes.count() != 1) {
        return;
    }

    /* Get supported MIMEs of the selected indexes */
    QMimeData *data = model()->mimeData(indexes);
    if (!data) {
        return;
    }

    /* Retrieve the ItemMimeData to get the pixmap */
    auto *mimeData = qobject_cast<QSchematic::Items::MimeData *>(data);
    if (!mimeData) {
        delete data;
        return;
    }

    /* Generate preview name for drag - onItemAdded will verify on drop */
    auto item    = mimeData->item();
    auto socItem = std::dynamic_pointer_cast<SchematicModule>(item);
    if (socItem && scene_) {
        QString previewName
            = SchematicWindow::generateUniqueInstanceName(*scene_, socItem->moduleName());
        socItem->setInstanceName(previewName);
    }

    /* Create the drag object with preview name */
    auto   *drag = new QDrag(this);
    QPointF hotSpot;
    drag->setMimeData(data);
    drag->setPixmap(item->toPixmap(hotSpot, scale_));
    drag->setHotSpot(hotSpot.toPoint());

    /* Execute the drag */
    drag->exec(supportedActions, Qt::CopyAction);
}

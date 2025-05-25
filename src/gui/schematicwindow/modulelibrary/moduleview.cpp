// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#include "moduleview.h"

#include <qschematic/items/item.hpp>
#include <qschematic/items/itemmimedata.hpp>

#include <QDebug>
#include <QDrag>
#include <QPainter>

using namespace ModuleLibrary;

ModuleView::ModuleView(QWidget *parent)
    : QTreeView(parent)
    , scale_(1.0)
{
    /* Configuration */
    setDragDropMode(QAbstractItemView::DragOnly);
    setDragEnabled(true);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setHeaderHidden(true);
    setIconSize(QSize(28, 28));
}

void ModuleView::setPixmapScale(qreal scale)
{
    scale_ = scale;
}

void ModuleView::startDrag(Qt::DropActions supportedActions)
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

    /* Create the drag object */
    auto   *drag = new QDrag(this);
    QPointF hotSpot;
    drag->setMimeData(data);
    drag->setPixmap(mimeData->item()->toPixmap(hotSpot, scale_));
    drag->setHotSpot(hotSpot.toPoint());

    /* Execute the drag */
    drag->exec(supportedActions, Qt::CopyAction);
}

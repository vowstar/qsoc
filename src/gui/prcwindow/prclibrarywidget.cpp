// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#include "prclibrarywidget.h"

#include <qschematic/scene.hpp>

#include <QBoxLayout>
#include <QDebug>
#include <QIcon>

using namespace PrcLibrary;

PrcLibraryWidget::PrcLibraryWidget(QWidget *parent)
    : QWidget(parent)
    , listWidget_(nullptr)
    , scene_(nullptr)
{
    listWidget_ = new QListWidget(this);
    listWidget_->setViewMode(QListView::ListMode);
    listWidget_->setResizeMode(QListView::Adjust);
    listWidget_->setIconSize(QSize(32, 32));
    listWidget_->setSpacing(2);

    /* Layout */
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(listWidget_);
    layout->setContentsMargins(0, 0, 0, 0);
    setLayout(layout);

    initializeLibrary();
    connect(listWidget_, &QListWidget::itemClicked, this, &PrcLibraryWidget::onItemClicked);
}

void PrcLibraryWidget::setScene(QSchematic::Scene *scene)
{
    scene_ = scene;
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
        {ClockSource, "Clock Source", "Clock input/generator", QColor(173, 216, 230)},
        {ClockTarget, "Clock Target", "Clock processing element", QColor(135, 206, 250)},
        {ResetSource, "Reset Source", "Reset generator", QColor(255, 182, 193)},
        {ResetTarget, "Reset Target", "Reset consumer", QColor(255, 160, 160)},
        {PowerDomain, "Power Domain", "Power domain controller", QColor(144, 238, 144)},
    };

    for (const auto &prim : primitives) {
        auto *item = new QListWidgetItem(prim.name);
        item->setToolTip(prim.description);
        item->setData(Qt::UserRole, static_cast<int>(prim.type));

        QPixmap pixmap(32, 32);
        pixmap.fill(prim.color);
        item->setIcon(QIcon(pixmap));

        listWidget_->addItem(item);
    }
}

void PrcLibraryWidget::onItemClicked(QListWidgetItem *item)
{
    if (!item) {
        return;
    }

    int           typeInt = item->data(Qt::UserRole).toInt();
    PrimitiveType type    = static_cast<PrimitiveType>(typeInt);

    emit primitiveSelected(type);
}

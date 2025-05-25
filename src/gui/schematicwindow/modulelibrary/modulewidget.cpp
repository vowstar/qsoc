// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#include "modulewidget.h"
#include "modulemodel.h"
#include "moduleview.h"

#include <QBoxLayout>
#include <QDebug>

using namespace ModuleLibrary;

ModuleWidget::ModuleWidget(QWidget *parent)
    : QWidget(parent)
    , model_(new ModuleModel(this))
    , view_(new ModuleView(this))
{
    /* Set up view with model */
    view_->setModel(model_);
    connect(view_, &ModuleView::clicked, this, &ModuleWidget::itemClickedSlot);

    /* Main layout */
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(view_);
    layout->setContentsMargins(0, 0, 0, 0);
    setLayout(layout);

    /* Expand all items initially */
    view_->expandAll();
}

void ModuleWidget::expandAll()
{
    if (view_) {
        view_->expandAll();
    }
}

void ModuleWidget::setPixmapScale(qreal scale)
{
    if (view_) {
        view_->setPixmapScale(scale);
    }
}

void ModuleWidget::itemClickedSlot(const QModelIndex &index)
{
    /* Sanity check */
    if (!index.isValid()) {
        return;
    }

    /* Get the item from the model */
    const QSchematic::Items::Item *item = model_->itemFromIndex(index);
    if (!item) {
        return;
    }

    /* Emit the signal */
    emit itemClicked(item);
}

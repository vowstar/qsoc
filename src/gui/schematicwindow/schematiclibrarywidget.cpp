// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#include "schematiclibrarywidget.h"
#include "common/qsocmodulemanager.h"
#include "schematiclibrarymodel.h"
#include "schematiclibraryview.h"

#include <QBoxLayout>
#include <QDebug>

SchematicLibraryWidget::SchematicLibraryWidget(QWidget *parent, QSocModuleManager *moduleManager)
    : QWidget(parent)
    , model_(nullptr)
    , view_(nullptr)
    , scene_(nullptr)
{
    try {
        model_ = new SchematicLibraryModel(this, moduleManager);
        view_  = new SchematicLibraryView(this);

        /* Set up view with model */
        view_->setModel(model_);

        connect(view_, &SchematicLibraryView::clicked, this, &SchematicLibraryWidget::itemClickedSlot);

        /* Main layout */
        auto *layout = new QVBoxLayout(this);
        layout->addWidget(view_);
        layout->setContentsMargins(0, 0, 0, 0);
        setLayout(layout);

        /* Expand all items initially */
        view_->expandAll();
    } catch (const std::exception &e) {
        qDebug() << "SchematicLibraryWidget: Exception in constructor:" << e.what();
        throw;
    } catch (...) {
        qDebug() << "SchematicLibraryWidget: Unknown exception in constructor";
        throw;
    }
}

void SchematicLibraryWidget::expandAll()
{
    if (view_) {
        view_->expandAll();
    }
}

void SchematicLibraryWidget::setPixmapScale(qreal scale)
{
    if (view_) {
        view_->setPixmapScale(scale);
    }
}

void SchematicLibraryWidget::setModuleManager(QSocModuleManager *moduleManager)
{
    if (model_) {
        model_->setModuleManager(moduleManager);
        if (view_) {
            view_->expandAll();
        }
    }
}

void SchematicLibraryWidget::setScene(QSchematic::Scene *scene)
{
    scene_ = scene;
    if (view_) {
        view_->setScene(scene);
    }
}

void SchematicLibraryWidget::itemClickedSlot(const QModelIndex &index)
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

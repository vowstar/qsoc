// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#include "modulewidget.h"
#include "common/qsocmodulemanager.h"
#include "modulemodel.h"
#include "moduleview.h"

#include <QBoxLayout>
#include <QDebug>

using namespace ModuleLibrary;

ModuleWidget::ModuleWidget(QWidget *parent, QSocModuleManager *moduleManager)
    : QWidget(parent)
    , model_(nullptr)
    , view_(nullptr)
    , scene_(nullptr)
{
    qDebug() << "ModuleWidget: Constructor called with moduleManager:"
             << (moduleManager ? "valid" : "null");

    try {
        qDebug() << "ModuleWidget: Creating ModuleModel";
        model_ = new ModuleModel(this, moduleManager);
        qDebug() << "ModuleWidget: ModuleModel created successfully";

        qDebug() << "ModuleWidget: Creating ModuleView";
        view_ = new ModuleView(this);
        qDebug() << "ModuleWidget: ModuleView created successfully";

        /* Set up view with model */
        qDebug() << "ModuleWidget: Setting model to view";
        view_->setModel(model_);
        qDebug() << "ModuleWidget: Model set to view successfully";

        qDebug() << "ModuleWidget: Connecting signals";
        connect(view_, &ModuleView::clicked, this, &ModuleWidget::itemClickedSlot);
        qDebug() << "ModuleWidget: Signals connected successfully";

        /* Main layout */
        qDebug() << "ModuleWidget: Creating layout";
        auto *layout = new QVBoxLayout(this);
        layout->addWidget(view_);
        layout->setContentsMargins(0, 0, 0, 0);
        setLayout(layout);
        qDebug() << "ModuleWidget: Layout created and set successfully";

        /* Expand all items initially */
        qDebug() << "ModuleWidget: Expanding all items";
        view_->expandAll();
        qDebug() << "ModuleWidget: All items expanded successfully";

        qDebug() << "ModuleWidget: Constructor completed successfully";
    } catch (const std::exception &e) {
        qDebug() << "ModuleWidget: Exception in constructor:" << e.what();
        throw;
    } catch (...) {
        qDebug() << "ModuleWidget: Unknown exception in constructor";
        throw;
    }
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

void ModuleWidget::setModuleManager(QSocModuleManager *moduleManager)
{
    if (model_) {
        model_->setModuleManager(moduleManager);
        if (view_) {
            view_->expandAll();
        }
    }
}

void ModuleWidget::setScene(QSchematic::Scene *scene)
{
    scene_ = scene;
    if (view_) {
        view_->setScene(scene);
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

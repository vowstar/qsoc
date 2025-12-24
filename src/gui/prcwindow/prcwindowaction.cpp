// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#include "gui/prcwindow/prcwindow.h"

#include "./ui_prcwindow.h"

#include <QDebug>
#include <QIcon>

void PrcWindow::on_actionQuit_triggered()
{
    close();
}

void PrcWindow::on_actionShowGrid_triggered(bool checked)
{
    const QString iconName = checked ? "view-grid-on" : "view-grid-off";
    const QIcon   icon(QIcon::fromTheme(iconName));
    ui->actionShowGrid->setIcon(icon);
    settings.showGrid = checked;
    scene.setSettings(settings);
    ui->prcView->setSettings(settings);
}

void PrcWindow::on_actionSelectItem_triggered()
{
    ui->actionSelectItem->setChecked(true);
    ui->actionAddWire->setChecked(false);
    scene.setMode(QSchematic::Scene::NormalMode);
}

void PrcWindow::on_actionAddWire_triggered()
{
    ui->actionAddWire->setChecked(true);
    ui->actionSelectItem->setChecked(false);
    scene.setMode(QSchematic::Scene::WireMode);
}

void PrcWindow::on_actionUndo_triggered()
{
    if (scene.undoStack()->canUndo()) {
        scene.undoStack()->undo();
    }
}

void PrcWindow::on_actionRedo_triggered()
{
    if (scene.undoStack()->canRedo()) {
        scene.undoStack()->redo();
    }
}

#include "gui/schematicwindow/schematicwindow.h"

#include "./ui_schematicwindow.h"

#include <QIcon>
#include <QPrintDialog>
#include <QPrinter>

void SchematicWindow::on_actionQuit_triggered()
{
    close();
}

void SchematicWindow::on_actionShowGrid_triggered(bool checked)
{
    const QString iconName = checked ? "view-grid-on" : "view-grid-off";
    const QIcon   icon(QIcon::fromTheme(iconName));
    ui->actionShowGrid->setIcon(icon);
    settings.showGrid = checked;
    scene.setSettings(settings);
    ui->schematicView->setSettings(settings);
}

void SchematicWindow::on_actionSelectItem_triggered()
{
    ui->actionSelectItem->setChecked(true);
    ui->actionAddWire->setChecked(false);
}

void SchematicWindow::on_actionAddWire_triggered()
{
    ui->actionAddWire->setChecked(true);
    ui->actionSelectItem->setChecked(false);
}

void SchematicWindow::on_actionUndo_triggered()
{
    if (scene.undoStack()->canUndo()) {
        scene.undoStack()->undo();
    }
}

void SchematicWindow::on_actionRedo_triggered()
{
    if (scene.undoStack()->canRedo()) {
        scene.undoStack()->redo();
    }
}

void SchematicWindow::on_actionPrint_triggered()
{
    QPrinter printer(QPrinter::HighResolution);
    if (QPrintDialog(&printer).exec() == QDialog::Accepted) {
        QPainter painter(&printer);
        painter.setRenderHint(QPainter::Antialiasing);
        scene.render(&painter);
    }
}

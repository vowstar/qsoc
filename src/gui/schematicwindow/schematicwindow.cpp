#include "gui/schematicwindow/schematicwindow.h"

#include "./ui_schematicwindow.h"

#include <qschematic/view.h>

SchematicWindow::SchematicWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::SchematicWindow)
{
    ui->setupUi(this);

    settings.debug               = false;
    settings.showGrid            = true;
    settings.routeStraightAngles = true;

    // scene.setParent(ui->schematicView);
    scene.setSettings(settings);
    ui->schematicView->setSettings(settings);
    ui->schematicView->setScene(&scene);

    scene.clear();
    scene.setSceneRect(-500, -500, 3000, 3000);
}

SchematicWindow::~SchematicWindow()
{
    delete ui;
}

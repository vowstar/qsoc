#include "gui/schematicwindow/schematicwindow.h"

#include "./ui_schematicwindow.h"

SchematicWindow::SchematicWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::SchematicWindow)
{
    ui->setupUi(this);
}

SchematicWindow::~SchematicWindow()
{
    delete ui;
}

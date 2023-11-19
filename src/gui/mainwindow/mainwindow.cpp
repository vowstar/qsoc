#include "gui/mainwindow/mainwindow.h"

#include "./ui_mainwindow.h"

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    ui->toolButtonSchematicEditor->setDefaultAction(ui->actionSchematicEditor);
    ui->toolButtonModuleEditor->setDefaultAction(ui->actionModuleEditor);
}

MainWindow::~MainWindow()
{
    delete ui;
}

#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "gui/schematicwindow/schematicwindow.h"

#include <QMainWindow>

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE
/**
 * @brief   The MainWindow class
 * @details This class is the main window class for the socstudio application.
 *          It is responsible for displaying the main window.
 */
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    /**
     * @brief Constructor for MainWindow
     * @param parent parent object
     * @details This constructor will initialize the main window
     */
    MainWindow(QWidget *parent = nullptr);
    /**
     * @brief Destructor for MainWindow
     * @details This destructor will free the main window
     */
    ~MainWindow();

private slots:
    void on_actionQuit_triggered();

    void on_actionSchematicEditor_triggered();

private:
    /* Main window UI */
    Ui::MainWindow *ui;
    SchematicWindow schematicWindow;
};
#endif // MAINWINDOW_H

#ifndef SCHEMATICWINDOW_H
#define SCHEMATICWINDOW_H

#include <QMainWindow>

#include <qschematic/scene.hpp>
#include <qschematic/settings.hpp>
#include <qschematic/view.hpp>

QT_BEGIN_NAMESPACE
namespace Ui {
class SchematicWindow;
}
QT_END_NAMESPACE
/**
 * @brief   The SchematicWindow class
 * @details This class is the schematic window class for the socstudio application.
 *          It is responsible for displaying the schematic window.
 */
class SchematicWindow : public QMainWindow
{
    Q_OBJECT

public:
    /**
     * @brief Constructor for SchematicWindow
     * @param parent parent object
     * @details This constructor will initialize the schematic window
     */
    SchematicWindow(QWidget *parent = nullptr);
    /**
     * @brief Destructor for SchematicWindow
     * @details This destructor will free the schematic window
     */
    ~SchematicWindow();

private slots:
    void on_actionPrint_triggered();

private slots:
    void on_actionRedo_triggered();

private slots:
    void on_actionUndo_triggered();

private slots:
    void on_actionAddWire_triggered();

private slots:
    void on_actionSelectItem_triggered();

private slots:
    void on_actionShowGrid_triggered(bool checked);

private slots:
    void on_actionQuit_triggered();

private:
    /* Main window UI */
    Ui::SchematicWindow *ui;

    QSchematic::Scene    scene;
    QSchematic::Settings settings;
};
#endif // SCHEMATICWINDOW_H

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
 * @brief The SchematicWindow class.
 * @details This class is the schematic window class for the socstudio
 *          application. It is responsible for displaying the schematic window.
 */
class SchematicWindow : public QMainWindow
{
    Q_OBJECT

public:
    /**
     * @brief Constructor for SchematicWindow.
     * @details This constructor will initialize the schematic window.
     * @param[in] parent parent object
     */
    SchematicWindow(QWidget *parent = nullptr);

    /**
     * @brief Destructor for SchematicWindow.
     * @details This destructor will free the schematic window.
     */
    ~SchematicWindow();

private slots:
    /**
     * @brief Print schematic file.
     * @details This function will print the schematic file.
     */
    void on_actionPrint_triggered();

    /**
     * @brief Redo Action.
     * @details This function is triggered to redo the last undone action.
     */
    void on_actionRedo_triggered();

    /**
     * @brief Undo Action.
     * @details This function is triggered to undo the last action performed.
     */
    void on_actionUndo_triggered();

    /**
     * @brief Add Wire.
     * @details This function is triggered to add a wire to the schematic.
     */
    void on_actionAddWire_triggered();

    /**
     * @brief Select Item.
     * @details This function is triggered to select an item within the
     *          schematic, based on the 'checked' state.
     */
    void on_actionSelectItem_triggered();

    /**
     * @brief Show Grid.
     * @details This function toggles the grid display on the schematic, based
     *          on the 'checked' state.
     */
    void on_actionShowGrid_triggered(bool checked);

    /**
     * @brief Quit schematic editor.
     * @details This function is triggered to quit the schematic editor.
     */
    void on_actionQuit_triggered();

private:
    /* Main window UI. */
    Ui::SchematicWindow *ui;

    /* Schematic scene. */
    QSchematic::Scene scene;

    /* Schematic settings. */
    QSchematic::Settings settings;
};
#endif // SCHEMATICWINDOW_H

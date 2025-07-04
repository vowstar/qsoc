// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#ifndef SCHEMATICWINDOW_H
#define SCHEMATICWINDOW_H

#include <QMainWindow>

#include <qschematic/scene.hpp>
#include <qschematic/settings.hpp>
#include <qschematic/view.hpp>

class QSocModuleManager;
class QSocProjectManager;

namespace ModuleLibrary {
class ModuleWidget;
}

QT_BEGIN_NAMESPACE
namespace Ui {
class SchematicWindow;
}
QT_END_NAMESPACE
/**
 * @brief The SchematicWindow class.
 * @details This class is the schematic window class for the qsoc
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
     * @param[in] projectManager project manager instance
     */
    SchematicWindow(QWidget *parent = nullptr, QSocProjectManager *projectManager = nullptr);

    /**
     * @brief Destructor for SchematicWindow.
     * @details This destructor will free the schematic window.
     */
    ~SchematicWindow();

    /**
     * @brief Set the project manager.
     * @details This function sets the project manager and initializes the module manager.
     * @param[in] projectManager project manager instance
     */
    void setProjectManager(QSocProjectManager *projectManager);

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
    /**
     * @brief Initialize the module library.
     * @details This function initializes the module library.
     */
    void initializeModuleLibrary();

    /**
     * @brief Add a module to the schematic.
     * @details This function adds a module to the schematic.
     * @param[in] item Item to add
     */
    void addModuleToSchematic(const QSchematic::Items::Item *item);

    /* Main window UI. */
    Ui::SchematicWindow *ui;

    /* Schematic scene. */
    QSchematic::Scene scene;

    /* Schematic settings. */
    QSchematic::Settings settings;

    /* Module library widget. */
    ModuleLibrary::ModuleWidget *moduleLibraryWidget;

    /* Module manager. */
    QSocModuleManager *moduleManager;

    /* Project manager. */
    QSocProjectManager *projectManager;
};
#endif // SCHEMATICWINDOW_H

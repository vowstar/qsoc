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

    /**
     * @brief Open a schematic file.
     * @details Opens and loads a schematic file from the specified path.
     *          Updates the window title and marks the undo stack as clean.
     * @param[in] filePath absolute path to the schematic file
     */
    void openFile(const QString &filePath);

private slots:
    /* Application Actions */

    /**
     * @brief Quit schematic editor.
     * @details Closes the schematic editor window.
     */
    void on_actionQuit_triggered();

    /* File Actions */

    /**
     * @brief Open schematic file.
     * @details Opens a schematic file (*.soc_sch).
     */
    void on_actionOpen_triggered();

    /**
     * @brief Save schematic file.
     * @details Saves the schematic file as *.soc_sch.
     */
    void on_actionSave_triggered();

    /**
     * @brief Save schematic file as.
     * @details Saves the schematic file to a new location.
     */
    void on_actionSaveAs_triggered();

    /**
     * @brief Close the current file.
     * @details Closes the current file (with save prompt if modified) and
     *          resets to "untitled" state. The window remains open.
     */
    void on_actionClose_triggered();

    /**
     * @brief Print schematic file.
     * @details Prints the schematic file.
     */
    void on_actionPrint_triggered();

    /* Edit Actions */

    /**
     * @brief Undo action.
     * @details Undoes the last action performed.
     */
    void on_actionUndo_triggered();

    /**
     * @brief Redo action.
     * @details Redoes the last undone action.
     */
    void on_actionRedo_triggered();

    /* View Actions */

    /**
     * @brief Show grid toggle.
     * @details Toggles the grid display on the schematic.
     */
    void on_actionShowGrid_triggered(bool checked);

    /* Tool Actions */

    /**
     * @brief Select item tool.
     * @details Activates the item selection tool.
     */
    void on_actionSelectItem_triggered();

    /**
     * @brief Add wire tool.
     * @details Activates the wire drawing tool.
     */
    void on_actionAddWire_triggered();

protected:
    /**
     * @brief Handle window close event.
     * @details Prompts the user to save changes if the document has been modified.
     * @param[in] event close event
     */
    void closeEvent(QCloseEvent *event) override;

private:
    /* Module Library Management */

    /**
     * @brief Initialize the module library.
     * @details Initializes the module library widget and connects signals.
     */
    void initializeModuleLibrary();

    /**
     * @brief Add a module to the schematic.
     * @details Adds a module to the schematic at the view center.
     * @param[in] item Item to add
     */
    void addModuleToSchematic(const QSchematic::Items::Item *item);

    /* File Management Helpers */

    /**
     * @brief Update window title based on file name and modification status.
     * @details Sets the title to "untitled" or the file name, with "*" prefix if modified.
     */
    void updateWindowTitle();

    /**
     * @brief Check if changes should be saved before closing.
     * @details Prompts user if the document has been modified.
     * @return true if it's safe to proceed (saved/discarded/no changes), false if cancelled
     */
    bool checkSaveBeforeClose();

    /**
     * @brief Save the schematic to a file.
     * @details Performs the actual save operation using gpds serialization.
     * @param[in] path absolute path to save the file
     */
    void saveToFile(const QString &path);

    /**
     * @brief Get the current file name.
     * @details Returns "untitled" if no file is open, otherwise the file name.
     * @return current file name
     */
    QString getCurrentFileName() const;

    /**
     * @brief Close the current file and reset to untitled state.
     * @details Clears the scene, undo stack, and resets file path to "untitled".
     */
    void closeFile();

    /* Member Variables */

    /* Main window UI */
    Ui::SchematicWindow *ui;

    /* Schematic scene */
    QSchematic::Scene scene;

    /* Schematic settings */
    QSchematic::Settings settings;

    /* Module library widget */
    ModuleLibrary::ModuleWidget *moduleLibraryWidget;

    /* Module manager */
    QSocModuleManager *moduleManager;

    /* Project manager */
    QSocProjectManager *projectManager;

    /* Current file path (empty string means untitled) */
    QString m_currentFilePath;
};
#endif // SCHEMATICWINDOW_H

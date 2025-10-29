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
class SocModuleItem;
} // namespace ModuleLibrary

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

    /**
     * @brief Export netlist.
     * @details Exports the current schematic to .soc_net format.
     */
    void on_actionExportNetlist_triggered();

protected:
    /**
     * @brief Handle window close event.
     * @details Prompts the user to save changes if the document has been modified.
     * @param[in] event close event
     */
    void closeEvent(QCloseEvent *event) override;

    /**
     * @brief Event filter for view mouse events.
     * @details Handles wire double-click for renaming.
     * @param[in] watched watched object
     * @param[in] event event to filter
     * @return true if event is handled
     */
    bool eventFilter(QObject *watched, QEvent *event) override;

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

    /* Instance Management */

    /**
     * @brief Get existing instance names from scene.
     * @details Collects all instance names currently in use.
     * @return Set of existing instance names
     */
    QSet<QString> getExistingInstanceNames() const;

    /**
     * @brief Generate unique instance name.
     * @details Generates instance name in format u_<modulename>_N with auto-increment.
     * @param[in] moduleName Module name
     * @return Unique instance name
     */
    QString generateUniqueInstanceName(const QString &moduleName);

    /* Netlist Management */

    /**
     * @brief Auto-name wires that don't have names.
     * @details Called when netlist changes to auto-generate wire names.
     */
    void autoNameWires();

    /**
     * @brief Get wire start position for label placement.
     * @param[in] wireNet wire net to get start position from
     * @return Start position of the wire, or null point if not found
     */
    QPointF getWireStartPos(const QSchematic::Items::WireNet *wireNet) const;

    /**
     * @brief Handle item added to scene.
     * @details Auto-generates unique instance names for SocModuleItems added via drag/drop.
     * @param[in] item pointer to the added item
     */
    void onItemAdded(std::shared_ptr<QSchematic::Items::Item> item);

    /**
     * @brief Handle label double-click for renaming instance.
     * @details Shows input dialog to rename the instance.
     * @param[in] socItem pointer to the SocModuleItem object
     */
    void handleLabelDoubleClick(ModuleLibrary::SocModuleItem *socItem);

    /**
     * @brief Handle wire double-click for renaming.
     * @details Shows input dialog to rename the wire/net.
     * @param[in] wireNet pointer to the WireNet object
     */
    void handleWireDoubleClick(QSchematic::Items::WireNet *wireNet);

    /**
     * @brief Automatically generate wire name based on connections.
     * @details Generates name like "instance_port" or "unnamed_N".
     * @param[in] wireNet pointer to the WireNet object
     * @return generated name string
     */
    QString autoGenerateWireName(const QSchematic::Items::WireNet *wireNet) const;

    /**
     * @brief Export netlist to .soc_net file.
     * @details Extracts connectivity and writes YAML format file.
     * @param[in] filePath path to save the .soc_net file
     * @return true if export succeeded, false otherwise
     */
    bool exportNetlist(const QString &filePath);

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

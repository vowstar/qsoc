// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "common/qsocprojectmanager.h"
#include "gui/schematicwindow/schematicwindow.h"

#include <QDir>
#include <QMainWindow>

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE
/**
 * @brief The MainWindow class.
 * @details This class is the main window class for the qsoc application.
 *          It is responsible for displaying the main window.
 */
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    /**
     * @brief Constructor for MainWindow.
     * @details This constructor will initialize the main window.
     * @param[in] parent parent object.
     */
    MainWindow(QWidget *parent = nullptr);

    /**
     * @brief Destructor for MainWindow.
     * @details This destructor will free the main window.
     */
    ~MainWindow();

private slots:
    /**
     * @brief Quit action.
     * @details This function triggers the main window's close event when
     *          the user selects the "Quit" action from the menu or toolbar.
     *          It uses Qt's standard close() mechanism which will prompt
     *          for saving any unsaved changes if implemented.
     */
    void on_actionQuit_triggered();

    /**
     * @brief Open schematic editor.
     * @details This function configures and displays the schematic editor window.
     *          It sets the schematic window's parent to the main window,
     *          configures it as an independent window using Qt::Window flag,
     *          and then displays it to the user. The schematic editor allows
     *          users to create and edit circuit diagrams.
     */
    void on_actionSchematicEditor_triggered();

    /**
     * @brief New project action.
     * @details This function manages the complete workflow of creating a new project:
     *          1. Closes any existing project silently
     *          2. Shows a save dialog for the user to specify project name and location
     *          3. Extracts project information from the selected path
     *          4. Configures the project manager with the new project details
     *          5. Creates the project directory structure
     *          6. Handles error conditions with appropriate user feedback
     *          7. Updates the lastProjectDir for future use
     *          8. Sets up the project tree view to display the new project structure
     */
    void on_actionNewProject_triggered();

    /**
     * @brief Open an existing project.
     * @details This function handles opening an existing project file,
     *          loading its configuration, and displaying it in the tree view.
     *          It performs the following steps:
     *          1. Closes any existing project silently
     *          2. Shows an open dialog for the user to select a project file
     *          3. Extracts project information from the selected file
     *          4. Configures the project manager and loads the project
     *          5. Handles error conditions with appropriate user feedback
     *          6. Updates the lastProjectDir for future use
     *          7. Displays the project structure in the tree view
     */
    void on_actionOpenProject_triggered();

    /**
     * @brief Close the current project.
     * @details This function calls the private closeProject method with
     *          silent=false parameter to close the currently open project
     *          while providing feedback to the user via the status bar.
     *          It clears the project tree view and resets project manager state.
     */
    void on_actionCloseProject_triggered();

    /**
     * @brief Open the project in the file explorer.
     * @details This function triggers the system's file explorer to open
     *          the directory of the currently loaded project, allowing users
     *          to easily access project files and folders directly from the
     *          file system.
     */
    void on_actionOpenProjectInFileExplorer_triggered();

    /**
     * @brief Refresh the project tree view.
     * @details This function refreshes the project tree view to display
     *          the latest project structure. It updates the project tree
     *          view to reflect any changes in the project directory.
     */
    void on_actionRefresh_triggered();

    /**
     * @brief Handle double-click on project tree item.
     * @details Opens the appropriate editor for the double-clicked file.
     *          For .soc_sch files, opens the schematic editor.
     * @param[in] index model index of the double-clicked item
     */
    void onTreeItemDoubleClicked(const QModelIndex &index);

private:
    /**
     * @brief Close the current project with option for silent mode.
     * @details This function handles clearing the project tree view,
     *          resetting the project manager state, and optionally
     *          notifying the user via the status bar.
     * @param silent If true, suppresses the status bar notification.
     */
    void closeProject(bool silent = false);

    /**
     * @brief Sets up the project tree view with directories
     * @param projectName Name of the project to display in tree
     * @details Creates a tree view model if not exists, adds the project
     *          as root item with its directory structure (Bus, Module,
     *          Schematic, Output) as child nodes, sets appropriate icons,
     *          and expands the tree view to show the project structure.
     *
     *          This function also scans and displays files in each directory:
     *          - *.soc_bus files in Bus directory
     *          - *.soc_mod files in Module directory
     *          - *.soc_sch files in Schematic directory
     *          - *.soc_net files in Output directory
     *          - *.v (Verilog) files in Output directory
     *          - *.csv files in Output directory
     *
     *          Each file is displayed with a document icon and stores its full
     *          path in the item's user data for later access. File types are
     *          processed separately to allow for different icon assignment
     *          in future implementations.
     *
     *          The function automatically expands parent nodes after adding
     *          child items - the project node is always expanded, while
     *          directory nodes (Bus, Module, Schematic, Output) are expanded
     *          only if they contain at least one file.
     */
    void setupProjectTreeView(const QString &projectName);

    /**
     * @brief Auto-open project if exactly one .soc_pro file exists in current directory
     * @details Scans current working directory for .soc_pro files. If exactly one
     *          is found, automatically opens it. If zero or multiple files found,
     *          does nothing. This provides convenience for single-project directories.
     */
    void autoOpenSingleProject();

    /* Main window UI */
    Ui::MainWindow *ui;
    /* Last used project directory */
    QString lastProjectDir = QDir::currentPath();
    /* Project manager instance (parent-managed) */
    QSocProjectManager *projectManager = nullptr;
    /* Schematic window object */
    SchematicWindow schematicWindow;
};

#endif // MAINWINDOW_H

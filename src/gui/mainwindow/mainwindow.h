// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "common/qsocprojectmanager.h"
#include "gui/schematicwindow/schematicwindow.h"

#include <QDir>
#include <QLabel>
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
    /* Application Actions (Qt Auto-connected) */

    /**
     * @brief Quit action.
     * @details Triggers the main window's close event when the user selects
     *          the "Quit" action from the menu or toolbar.
     */
    void on_actionQuit_triggered();

    /* Project Management Actions (Qt Auto-connected) */

    /**
     * @brief New project action.
     * @details Creates a new project with user-specified name and location.
     */
    void on_actionNewProject_triggered();

    /**
     * @brief Open an existing project.
     * @details Opens and loads an existing project file.
     */
    void on_actionOpenProject_triggered();

    /**
     * @brief Close the current project.
     * @details Closes the currently open project with user feedback.
     */
    void on_actionCloseProject_triggered();

    /**
     * @brief Open the project in the file explorer.
     * @details Opens the project directory in the system's file explorer.
     */
    void on_actionOpenProjectInFileExplorer_triggered();

    /**
     * @brief Refresh the project tree view.
     * @details Refreshes the project tree to reflect file system changes.
     */
    void on_actionRefresh_triggered();

    /* Editor Actions (Qt Auto-connected) */

    /**
     * @brief Open schematic editor action.
     * @details Opens the schematic editor with a new untitled file.
     */
    void on_actionSchematicEditor_triggered();

    /* Manual Signal Handlers */

    /**
     * @brief Handle double-click on project tree item.
     * @details Dispatches to appropriate editor based on file extension.
     *          This is a manually connected slot (not Qt Auto-connection).
     * @param[in] index model index of the double-clicked item
     */
    void handleTreeDoubleClick(const QModelIndex &index);

private:
    /* Project Management Helpers */
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
     * @brief Auto-open project if exactly one .soc_pro file exists in current directory.
     * @details Scans current working directory for .soc_pro files. If exactly one
     *          is found, automatically opens it. If zero or multiple files found,
     *          does nothing. This provides convenience for single-project directories.
     */
    void autoOpenSingleProject();

    /**
     * @brief Update window title with project path.
     * @details Updates the window title to show project file path. If path exceeds
     *          60 characters, truncates middle portion and replaces with "...".
     */
    void updateWindowTitle();

    /**
     * @brief Truncate string middle with ellipsis.
     * @details If string exceeds maxLen, keeps beginning and end, replaces middle with "...".
     * @param str Input string
     * @param maxLen Maximum length (including ellipsis)
     * @return Truncated string
     */
    static QString truncateMiddle(const QString &str, int maxLen);

    /* Editor Management */

    /**
     * @brief Open schematic editor with optional file.
     * @details Unified method to open schematic editor. Ensures project manager
     *          is set and module list is loaded. If a file path is provided,
     *          opens that file; otherwise opens with "untitled".
     * @param[in] filePath optional path to schematic file (empty = new file)
     */
    void openSchematicEditor(const QString &filePath = QString());

    /* Member Variables */

    /* Main window UI */
    Ui::MainWindow *ui;
    /* Last used project directory */
    QString lastProjectDir = QDir::currentPath();
    /* Project manager instance (parent-managed) */
    QSocProjectManager *projectManager = nullptr;
    /* Schematic window object */
    SchematicWindow schematicWindow;
    /* Permanent status bar label (not affected by clearMessage or menu hover) */
    QLabel *statusBarPermanentLabel = nullptr;
};

#endif // MAINWINDOW_H

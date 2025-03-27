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
     * @details This function will quit the application.
     */
    void on_actionQuit_triggered();

    /**
     * @brief Open schematic editor.
     * @details This function will open the schematic editor.
     */
    void on_actionSchematicEditor_triggered();

    /**
     * @brief New project action.
     * @details This function will create a new project.
     */
    void on_actionNewProject_triggered();

    /**
     * @brief Open an existing project.
     * @details This function handles opening an existing project file,
     *          loading its configuration, and displaying it in the tree view.
     */
    void on_actionOpenProject_triggered();

    /**
     * @brief Close the current project.
     * @details This function closes the currently open project and
     *          clears the project tree view.
     */
    void on_actionCloseProject_triggered();

private:
    /**
     * @brief Close the current project with option for silent mode.
     * @details This function handles clearing the project tree view,
     *          resetting the project manager state, and optionally
     *          notifying the user via the status bar.
     * @param silent If true, suppresses the status bar notification.
     */
    void closeProject(bool silent = false);

    /* Main window UI */
    Ui::MainWindow *ui;
    /* Last used project directory */
    QString lastProjectDir = QDir::homePath();
    /* Project manager instance (parent-managed) */
    QSocProjectManager *projectManager = nullptr;
    /* Schematic window object */
    SchematicWindow schematicWindow;
};
#endif // MAINWINDOW_H

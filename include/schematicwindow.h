#ifndef SCHEMATICWINDOW_H
#define SCHEMATICWINDOW_H

#include <QMainWindow>

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

private:
    /* Main window UI */
    Ui::SchematicWindow *ui;
};
#endif // SCHEMATICWINDOW_H

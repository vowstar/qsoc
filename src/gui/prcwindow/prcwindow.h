// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#ifndef PRCWINDOW_H
#define PRCWINDOW_H

#include "prcprimitiveitem.h"
#include "prcscene.h"

#include <QLabel>
#include <QMainWindow>
#include <QMap>

#include <qschematic/settings.hpp>
#include <qschematic/view.hpp>

class QSocProjectManager;

namespace PrcLibrary {
class PrcLibraryWidget;
}

QT_BEGIN_NAMESPACE
namespace Ui {
class PrcWindow;
}
QT_END_NAMESPACE

/**
 * @brief PRC (Power/Reset/Clock) editor window
 * @details Provides graphical interface for editing PRC configurations using
 *          QSchematic-based visual editing. Supports drag-and-drop primitive
 *          placement, wire routing, and netlist export.
 */
class PrcWindow : public QMainWindow
{
    Q_OBJECT

public:
    /**
     * @brief Construct PRC editor window
     * @param[in] parent Parent widget
     * @param[in] projectManager Project manager instance
     */
    PrcWindow(QWidget *parent = nullptr, QSocProjectManager *projectManager = nullptr);

    /**
     * @brief Destructor
     */
    ~PrcWindow();

    /**
     * @brief Set project manager reference
     * @param[in] projectManager Project manager instance
     */
    void setProjectManager(QSocProjectManager *projectManager);

    /**
     * @brief Open PRC file and load into scene
     * @param[in] filePath Path to .soc_prc file
     */
    void openFile(const QString &filePath);

    /**
     * @brief Get access to the PRC scene
     * @return Reference to PrcScene
     */
    PrcLibrary::PrcScene &prcScene();

    /**
     * @brief Get read-only access to the PRC scene
     * @return Const reference to PrcScene
     */
    const PrcLibrary::PrcScene &prcScene() const;

    /**
     * @brief Collect all existing PRC element names from scene
     * @param[in] scene QSchematic scene to scan
     * @return Set of existing element names
     */
    static QSet<QString> getExistingControllerNames(const QSchematic::Scene &scene);

    /**
     * @brief Generate unique element name with auto-increment suffix
     * @param[in] scene QSchematic scene to scan
     * @param[in] prefix Name prefix (e.g., "clk_")
     * @return Unique element name
     */
    static QString generateUniqueControllerName(
        const QSchematic::Scene &scene, const QString &prefix);

private slots:
    /* Application Actions */

    /**
     * @brief Handle quit action
     */
    void on_actionQuit_triggered();

    /* File Actions */

    /**
     * @brief Handle open file action
     */
    void on_actionOpen_triggered();

    /**
     * @brief Handle save file action
     */
    void on_actionSave_triggered();

    /**
     * @brief Handle save as action
     */
    void on_actionSaveAs_triggered();

    /**
     * @brief Handle close file action
     */
    void on_actionClose_triggered();

    /**
     * @brief Handle print action
     */
    void on_actionPrint_triggered();

    /* Edit Actions */

    /**
     * @brief Handle undo action
     */
    void on_actionUndo_triggered();

    /**
     * @brief Handle redo action
     */
    void on_actionRedo_triggered();

    /* View Actions */

    /**
     * @brief Handle show grid toggle
     * @param[in] checked Grid visibility state
     */
    void on_actionShowGrid_triggered(bool checked);

    /* Tool Actions */

    /**
     * @brief Handle select item tool activation
     */
    void on_actionSelectItem_triggered();

    /**
     * @brief Handle add wire tool activation
     */
    void on_actionAddWire_triggered();

    /**
     * @brief Handle export netlist action
     */
    void on_actionExportNetlist_triggered();

    /* Manual Signal Handlers */

    /**
     * @brief Handle controller edit request from context menu
     * @param[in] type Controller type (ClockCtrl/ResetCtrl/PowerCtrl)
     * @param[in] name Controller name
     */
    void handleEditController(int type, const QString &name);

protected:
    /* Event Handlers */

    /**
     * @brief Handle window close event
     * @param[in] event Close event
     */
    void closeEvent(QCloseEvent *event) override;

    /**
     * @brief Handle event filtering for double-click and shortcuts
     * @param[in] watched Object being watched
     * @param[in] event Event to filter
     * @retval true Event handled
     * @retval false Event not handled
     */
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    /* PRC Library Management */

    /**
     * @brief Initialize PRC library widget and connect signals
     */
    void initializePrcLibrary();

    /* File Management Helpers */

    /**
     * @brief Update window title with file status
     */
    void updateWindowTitle();

    /**
     * @brief Check if save needed before closing
     * @retval true Continue closing
     * @retval false User cancelled
     */
    bool checkSaveBeforeClose();

    /**
     * @brief Save schematic to file
     * @param[in] path File path to save
     */
    void saveToFile(const QString &path);

    /**
     * @brief Get current file name
     * @return File name or "untitled"
     */
    QString getCurrentFileName() const;

    /**
     * @brief Close current file and clear scene
     */
    void closeFile();

    /* Netlist Management */

    /**
     * @brief Auto-generate names for unnamed wires
     */
    void autoNameWires();

    /**
     * @brief Get wire start position for label placement
     * @param[in] wireNet Wire net to query
     * @return Start position
     */
    QPointF getWireStartPos(const QSchematic::Items::WireNet *wireNet) const;

    /**
     * @brief Collect all existing wire names
     * @return Set of wire names
     */
    QSet<QString> getExistingWireNames() const;

    /**
     * @brief Connection information for wire endpoint
     */
    struct ConnectionInfo
    {
        QString instanceName; /**< Connected instance name */
        QString portName;     /**< Connected port name */
        int     portPosition; /**< Port position enum value */
    };

    /**
     * @brief Find connection at wire start point
     * @param[in] wireNet Wire net to query
     * @return Connection information
     */
    ConnectionInfo findStartConnection(const QSchematic::Items::WireNet *wireNet) const;

    /**
     * @brief Handle item added to scene
     * @param[in] item Added item
     */
    void onItemAdded(std::shared_ptr<QSchematic::Items::Item> item);

    /**
     * @brief Handle PRC item double-click
     * @param[in] prcItem Clicked PRC item
     */
    void handlePrcItemDoubleClick(QSchematic::Items::Item *prcItem);

    /**
     * @brief Handle wire double-click
     * @param[in] wireNet Clicked wire net
     */
    void handleWireDoubleClick(QSchematic::Items::WireNet *wireNet);

    /**
     * @brief Auto-generate wire name based on connections
     * @param[in] wireNet Wire net to name
     * @return Generated name
     */
    QString autoGenerateWireName(const QSchematic::Items::WireNet *wireNet) const;

    /**
     * @brief Update dynamic ports on all target primitives
     * @details Called when netlist changes to ensure each target has
     *          at least one available input port.
     */
    void updateAllDynamicPorts();

    /**
     * @brief Export PRC netlist to file
     * @param[in] filePath Output file path
     * @retval true Export successful
     * @retval false Export failed
     */
    bool exportNetlist(const QString &filePath);

    /* Link Parameter Management */

    /**
     * @brief Get link parameters for a wire net
     * @param[in] wireNetName Wire net name (source->target format)
     * @return Link parameters (default if not found)
     */
    PrcLibrary::ClockLinkParams getLinkParams(const QString &wireNetName) const;

    /**
     * @brief Set link parameters for a wire net
     * @param[in] wireNetName Wire net name (source->target format)
     * @param[in] params Link parameters to set
     */
    void setLinkParams(const QString &wireNetName, const PrcLibrary::ClockLinkParams &params);

    /**
     * @brief Check if wire net has link parameters configured
     * @param[in] wireNetName Wire net name
     * @return true if configured
     */
    bool hasLinkParams(const QString &wireNetName) const;

    /**
     * @brief Remove link parameters for a wire net
     * @param[in] wireNetName Wire net name
     */
    void removeLinkParams(const QString &wireNetName);

    /**
     * @brief Get all link parameters keyed by target name then source name
     * @return Map of target->source->ClockLinkParams
     */
    QMap<QString, QMap<QString, PrcLibrary::ClockLinkParams>> getAllLinkParamsByTarget() const;

    /**
     * @brief Wire connection info from analysis
     */
    struct WireConnectionInfo
    {
        QString sourceName;  /**< Source primitive name (ClockInput/ResetSource) */
        QString targetName;  /**< Target primitive name (ClockTarget/ResetTarget) */
        QString wireNetName; /**< Wire net name */
    };

    /**
     * @brief Analyze actual wire connections from scene
     * @details Traverses wire manager to find all source->target connections.
     *          Only considers connections between compatible types:
     *          - ClockInput -> ClockTarget
     *          - ResetSource -> ResetTarget
     *          - PowerDomain -> PowerDomain
     * @return List of wire connections
     */
    QList<WireConnectionInfo> analyzeWireConnections() const;

    /**
     * @brief Get set of source names connected to a specific target via wires
     * @param[in] targetName Target primitive name
     * @return Set of connected source names
     */
    QSet<QString> getConnectedSources(const QString &targetName) const;

    /* Member Variables */

    Ui::PrcWindow                *ui;                                /**< Main window UI */
    PrcLibrary::PrcScene          scene;                             /**< PRC scene */
    QSchematic::Settings          settings;                          /**< Scene settings */
    PrcLibrary::PrcLibraryWidget *prcLibraryWidget;                  /**< PRC library widget */
    QSocProjectManager           *projectManager;                    /**< Project manager */
    QString                       m_currentFilePath;                 /**< Current file path */
    QLabel                       *statusBarPermanentLabel = nullptr; /**< Status bar label */

    /**
     * @brief Link parameters storage
     * @details Maps wire net name to link-level clock parameters.
     *          This stores ICG/DIV/INV/STA_GUIDE operations for each wire.
     *          Wire names use "source->target" format.
     */
    QMap<QString, PrcLibrary::ClockLinkParams> m_linkParams;
};
#endif // PRCWINDOW_H

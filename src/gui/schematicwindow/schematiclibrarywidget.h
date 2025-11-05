// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#ifndef MODULELIBRARY_QSOCWIDGET_H
#define MODULELIBRARY_QSOCWIDGET_H

#include <QWidget>

class QSocModuleManager;

namespace QSchematic {
class Scene;
}

namespace QSchematic::Items {
class Item;
}

namespace ModuleLibrary {

class ModuleModel;
class ModuleView;

/**
 * @brief The ModuleWidget class.
 * @details This class is the module library widget class for the module
 *          application. It is responsible for displaying the module
 *          library widget.
 */
class ModuleWidget : public QWidget
{
    Q_OBJECT

public:
    /**
     * @brief Constructor for ModuleWidget.
     * @details This constructor will initialize the module library widget.
     * @param[in] parent Parent object
     * @param[in] moduleManager QSocModuleManager instance
     */
    explicit ModuleWidget(QWidget *parent = nullptr, QSocModuleManager *moduleManager = nullptr);

    /**
     * @brief Destructor for ModuleWidget.
     * @details This destructor will free the module library widget.
     */
    ~ModuleWidget() override = default;

    /**
     * @brief Expand all items in the tree view.
     * @details This function will expand all items in the tree view.
     */
    void expandAll();

    /**
     * @brief Set the module manager.
     * @details This function sets the module manager and refreshes the model.
     * @param[in] moduleManager QSocModuleManager instance
     */
    void setModuleManager(QSocModuleManager *moduleManager);

    /**
     * @brief Set the schematic scene for generating unique instance names during drag.
     * @param[in] scene pointer to the schematic scene
     */
    void setScene(QSchematic::Scene *scene);

signals:
    /**
     * @brief Signal emitted when an item is clicked.
     * @details This signal is emitted when an item is clicked.
     * @param[in] item Item that was clicked
     */
    void itemClicked(const QSchematic::Items::Item *item);

public slots:
    /**
     * @brief Set the pixmap scale.
     * @details This function will set the pixmap scale.
     * @param[in] scale Pixmap scale
     */
    void setPixmapScale(qreal scale);

private slots:
    /**
     * @brief Slot called when an item is clicked.
     * @details This slot is called when an item is clicked.
     * @param[in] index Index of the item that was clicked
     */
    void itemClickedSlot(const QModelIndex &index);

private:
    ModuleModel       *model_; /**< Module library model */
    ModuleView        *view_;  /**< Module library view */
    QSchematic::Scene *scene_; /**< Schematic scene for unique name generation */
};

} // namespace ModuleLibrary

#endif // MODULELIBRARY_QSOCWIDGET_H

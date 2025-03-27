// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#ifndef MODULELIBRARY_QSOCWIDGET_H
#define MODULELIBRARY_QSOCWIDGET_H

#include <QWidget>

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
     */
    explicit ModuleWidget(QWidget *parent = nullptr);

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
    ModuleModel *model_; /**< Module library model */
    ModuleView  *view_;  /**< Module library view */
};

} // namespace ModuleLibrary

#endif // MODULELIBRARY_QSOCWIDGET_H

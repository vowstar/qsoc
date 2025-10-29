// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#ifndef MODULELIBRARY_QSOCVIEW_H
#define MODULELIBRARY_QSOCVIEW_H

#include <QTreeView>

namespace QSchematic {
class Scene;
}

namespace ModuleLibrary {

/**
 * @brief The ModuleView class.
 * @details This class is the module library view class for the module
 *          application. It is responsible for displaying the module
 *          library.
 */
class ModuleView : public QTreeView
{
    Q_OBJECT

public:
    /**
     * @brief Constructor for ModuleView.
     * @details This constructor will initialize the module library view.
     * @param[in] parent Parent object
     */
    explicit ModuleView(QWidget *parent = nullptr);

    /**
     * @brief Destructor for ModuleView.
     * @details This destructor will free the module library view.
     */
    ~ModuleView() override = default;

public slots:
    /**
     * @brief Set the pixmap scale.
     * @details This function will set the pixmap scale.
     * @param[in] scale Pixmap scale
     */
    void setPixmapScale(qreal scale);

    /**
     * @brief Set the schematic scene for generating unique instance names.
     * @param[in] scene pointer to the schematic scene
     */
    void setScene(QSchematic::Scene *scene);

protected:
    /**
     * @brief Start dragging.
     * @details This function will start dragging.
     * @param[in] supportedActions Supported actions
     */
    void startDrag(Qt::DropActions supportedActions) override;

private:
    qreal              scale_; /**< Pixmap scale */
    QSchematic::Scene *scene_; /**< Schematic scene for unique name generation */
};

} // namespace ModuleLibrary

#endif // MODULELIBRARY_QSOCVIEW_H

// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#ifndef PRCLIBRARYWIDGET_H
#define PRCLIBRARYWIDGET_H

#include "prcprimitiveitem.h"

#include <QListWidget>
#include <QWidget>

namespace QSchematic {
class Scene;
}

namespace PrcLibrary {

/**
 * @brief Custom list widget with drag-and-drop support for PRC primitives
 */
class PrcLibraryListWidget : public QListWidget
{
    Q_OBJECT

public:
    explicit PrcLibraryListWidget(QWidget *parent = nullptr);
    ~PrcLibraryListWidget() override = default;

    /**
     * @brief Set schematic scene for unique name generation
     * @param[in] scene Target schematic scene
     */
    void setScene(QSchematic::Scene *scene);

protected:
    /**
     * @brief Start drag operation with PrcPrimitiveItem preview
     * @param[in] supportedActions Supported drop actions
     */
    void startDrag(Qt::DropActions supportedActions) override;

private:
    QSchematic::Scene *scene; /**< Target schematic scene */
};

/**
 * @brief Library widget for PRC primitives with drag-and-drop support
 *
 * @details Provides a list view of available primitive types that can be
 *          dragged and dropped onto the schematic scene.
 */
class PrcLibraryWidget : public QWidget
{
    Q_OBJECT

public:
    explicit PrcLibraryWidget(QWidget *parent = nullptr);
    ~PrcLibraryWidget() override = default;

    /**
     * @brief Set schematic scene for item placement
     * @param[in] scene Target schematic scene
     */
    void setScene(QSchematic::Scene *scene);

private:
    /**
     * @brief Initialize library with primitive types
     */
    void initializeLibrary();

    PrcLibraryListWidget *listWidget; /**< Primitive list view */
    QSchematic::Scene    *scene;      /**< Target schematic scene */
};

} // namespace PrcLibrary

#endif // PRCLIBRARYWIDGET_H

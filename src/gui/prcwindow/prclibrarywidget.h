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
 * @brief Library widget for PRC primitives with drag-and-drop support
 *
 * @details Provides a list view of available primitive types that can be
 *          selected and placed onto the schematic scene.
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

signals:
    /**
     * @brief Emitted when primitive type selected for placement
     * @param primitiveType Selected primitive type
     */
    void primitiveSelected(PrimitiveType primitiveType);

private slots:
    /**
     * @brief Handle list item click
     * @param[in] item Clicked list widget item
     */
    void onItemClicked(QListWidgetItem *item);

private:
    /**
     * @brief Initialize library with primitive types
     */
    void initializeLibrary();

    QListWidget       *listWidget_; /**< Primitive list view */
    QSchematic::Scene *scene_;      /**< Target schematic scene */
};

} // namespace PrcLibrary

#endif // PRCLIBRARYWIDGET_H

// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef PRCCONTROLLERGROUPING_H
#define PRCCONTROLLERGROUPING_H

#include "gui/prcwindow/prcprimitiveitem.h"

#include <memory>
#include <variant>

#include <QList>
#include <QString>
#include <QStringList>

namespace PrcLibrary {

using PrcItemList = QList<std::shared_ptr<PrcPrimitiveItem>>;

/**
 * @brief Partitions PRC primitives by the controller they belong to.
 * @details A diagram may hold several controllers of one family. The export
 *          used to emit only the first, dropping the rest without a word, so
 *          the grouping is kept here where it can be tested on its own.
 */
class PrcControllerGrouping
{
public:
    /**
     * @brief Controller a primitive is assigned to.
     * @param[in] item Primitive to inspect.
     * @param[in] fallback Name to use when nothing is assigned.
     * @return The controller name.
     */
    static QString controllerOf(
        const std::shared_ptr<PrcPrimitiveItem> &item, const QString &fallback)
    {
        if (!item) {
            return fallback;
        }
        const QString name = std::visit(
            [](const auto &params) -> QString { return params.controller; }, item->params());
        return name.isEmpty() ? fallback : name;
    }

    /**
     * @brief Distinct controller names across a group of primitives.
     * @param[in] items Primitives of one family.
     * @param[in] fallback Name unassigned primitives count as.
     * @return Controller names in the order they first appear.
     */
    static QStringList controllerNames(const PrcItemList &items, const QString &fallback)
    {
        QStringList names;
        for (const auto &item : items) {
            const QString name = controllerOf(item, fallback);
            if (!names.contains(name)) {
                names << name;
            }
        }
        return names;
    }

    /**
     * @brief Primitives assigned to one controller.
     * @param[in] items Primitives of one family.
     * @param[in] controller Controller to select.
     * @param[in] fallback Name unassigned primitives count as.
     * @return The matching primitives.
     */
    static PrcItemList itemsOfController(
        const PrcItemList &items, const QString &controller, const QString &fallback)
    {
        PrcItemList selected;
        for (const auto &item : items) {
            if (controllerOf(item, fallback) == controller) {
                selected << item;
            }
        }
        return selected;
    }
};

} // namespace PrcLibrary

#endif // PRCCONTROLLERGROUPING_H

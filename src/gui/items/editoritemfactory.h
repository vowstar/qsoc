// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef EDITORITEMFACTORY_H
#define EDITORITEMFACTORY_H

#include "gui/prcwindow/prcitemfactory.h"
#include "gui/schematicwindow/schematicitemfactory.h"

#include <memory>

#include <qschematic/items/itemfactory.hpp>

/**
 * @brief Item factory covering every editor.
 * @details QSchematic keeps a single custom item factory for the whole
 *          process. The editors are constructed together, so whichever
 *          installed its own factory last decided which documents could be
 *          deserialized at all. Dispatching on the type id serves both.
 */
class EditorItemFactory
{
public:
    /**
     * @brief Build an item from its serialized form.
     * @param[in] container Serialized item.
     * @return The item, or nullptr when no editor claims the type.
     */
    static std::shared_ptr<QSchematic::Items::Item> from_container(const gpds::container &container)
    {
        if (auto item = SchematicItemFactory::from_container(container)) {
            return item;
        }
        return PrcLibrary::PrcItemFactory::from_container(container);
    }

    /**
     * @brief Install this factory as the process-wide custom factory.
     */
    static void install()
    {
        QSchematic::Items::Factory::instance().setCustomItemsFactory(
            [](const gpds::container &container) { return from_container(container); });
    }
};

#endif // EDITORITEMFACTORY_H

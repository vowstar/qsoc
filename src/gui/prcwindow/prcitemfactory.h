// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#ifndef PRCITEMFACTORY_H
#define PRCITEMFACTORY_H

#include <gpds/container.hpp>
#include <memory>
#include <qschematic/items/item.hpp>

namespace PrcLibrary {

/**
 * @brief Factory for creating PRC items from serialized containers
 * @details Used by QSchematic::Scene to deserialize PRC items during file load
 */
class PrcItemFactory
{
public:
    /**
     * @brief Create an item from a GPDS container
     * @param[in] container Serialized item data
     * @return Shared pointer to the created item, or nullptr if unknown type
     */
    static std::shared_ptr<QSchematic::Items::Item> from_container(const gpds::container &container);
};

} // namespace PrcLibrary

#endif // PRCITEMFACTORY_H

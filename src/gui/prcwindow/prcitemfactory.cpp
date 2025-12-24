// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#include "prcitemfactory.h"
#include "prcconnector.h"
#include "prcitemtypes.h"
#include "prcprimitiveitem.h"

#include <qschematic/items/itemfactory.hpp>

using namespace PrcLibrary;

std::shared_ptr<QSchematic::Items::Item> PrcItemFactory::from_container(
    const gpds::container &container)
{
    /* Extract the item type */
    QSchematic::Items::Item::ItemType itemType = QSchematic::Items::Factory::extractType(container);

    /* Create appropriate item based on type */
    switch (itemType) {
    case PrcPrimitiveItem::Type: {
        /* Extract primitive_type from nested container */
        PrimitiveType primType = ClockInput; /* Default */
        if (auto primContainer = container.get_value<gpds::container *>("primitive")) {
            primType = static_cast<PrimitiveType>(
                (*primContainer)->get_value<int>("primitive_type").value_or(ClockInput));
        }
        return std::make_shared<PrcPrimitiveItem>(primType);
    }

    case PrcConnectorType:
        /* Create a default PrcConnector (will be populated by from_container) */
        return std::make_shared<PrcConnector>();

    default:
        break;
    }

    return nullptr;
}

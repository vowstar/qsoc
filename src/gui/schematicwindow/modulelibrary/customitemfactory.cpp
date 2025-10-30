// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#include "customitemfactory.h"
#include "gui/schematicwindow/customwire.h"
#include "itemtypes.h"
#include "socmoduleconnector.h"
#include "socmoduleitem.h"

#include <qschematic/items/itemfactory.hpp>

using namespace ModuleLibrary;

std::shared_ptr<QSchematic::Items::Item> CustomItemFactory::from_container(
    const gpds::container &container)
{
    // Extract the type
    QSchematic::Items::Item::ItemType itemType = QSchematic::Items::Factory::extractType(container);

    switch (static_cast<ItemType>(itemType)) {
    case ItemType::SocModuleItemType:
        return std::make_shared<SocModuleItem>(QString(), YAML::Node(), ItemType::SocModuleItemType);

    case ItemType::SocModuleConnectorType:
        return std::make_shared<SocModuleConnector>(
            QPoint(), QString(), SocModuleConnector::Input, SocModuleConnector::Left, nullptr);

    case ItemType::CustomWireType:
        return std::make_shared<SchematicCustom::CustomWire>(ItemType::CustomWireType);

    default:
        break;
    }

    return {};
}

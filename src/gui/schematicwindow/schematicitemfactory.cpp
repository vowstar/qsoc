// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#include "schematicitemfactory.h"
#include "schematicconnector.h"
#include "schematicitemtypes.h"
#include "schematicmodule.h"
#include "schematicwire.h"

#include <qschematic/items/itemfactory.hpp>

std::shared_ptr<QSchematic::Items::Item> SchematicItemFactory::from_container(
    const gpds::container &container)
{
    // Extract the type
    QSchematic::Items::Item::ItemType itemType = QSchematic::Items::Factory::extractType(container);

    switch (static_cast<SchematicItemType>(itemType)) {
    case SchematicItemType::SchematicModuleType:
        return std::make_shared<SchematicModule>(
            QString(), YAML::Node(), SchematicItemType::SchematicModuleType);

    case SchematicItemType::SchematicConnectorType:
        return std::make_shared<SchematicConnector>(
            QPoint(), QString(), SchematicConnector::Input, SchematicConnector::Left, nullptr);

    case SchematicItemType::SchematicWireType:
        return std::make_shared<SchematicWire>(SchematicItemType::SchematicWireType);

    default:
        break;
    }

    return {};
}

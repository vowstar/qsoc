// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#ifndef SCHEMATICITEMTYPES_H
#define SCHEMATICITEMTYPES_H

#include <qschematic/items/item.hpp>

enum SchematicItemType {
    SchematicModuleType    = QSchematic::Items::Item::QSchematicItemUserType + 1,
    SchematicConnectorType = QSchematic::Items::Item::QSchematicItemUserType + 2,
    SchematicWireType      = QSchematic::Items::Item::QSchematicItemUserType + 3
};

#endif // SCHEMATICITEMTYPES_H

// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#ifndef MODULELIBRARY_ITEMTYPES_H
#define MODULELIBRARY_ITEMTYPES_H

#include <qschematic/items/item.hpp>

namespace ModuleLibrary {

enum ItemType {
    SocModuleItemType      = QSchematic::Items::Item::QSchematicItemUserType + 1,
    SocModuleConnectorType = QSchematic::Items::Item::QSchematicItemUserType + 2
};

} // namespace ModuleLibrary

#endif // MODULELIBRARY_ITEMTYPES_H

// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#ifndef SCHEMATICITEMFACTORY_H
#define SCHEMATICITEMFACTORY_H

#include <gpds/container.hpp>
#include <memory>
#include <qschematic/items/item.hpp>

class SchematicItemFactory
{
public:
    static std::shared_ptr<QSchematic::Items::Item> from_container(const gpds::container &container);
};

#endif // SCHEMATICITEMFACTORY_H

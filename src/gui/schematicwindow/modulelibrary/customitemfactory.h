// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#ifndef CUSTOMITEMFACTORY_H
#define CUSTOMITEMFACTORY_H

#include <gpds/container.hpp>
#include <memory>
#include <qschematic/items/item.hpp>

namespace ModuleLibrary {

class CustomItemFactory
{
public:
    static std::shared_ptr<QSchematic::Items::Item> from_container(const gpds::container &container);
};

} // namespace ModuleLibrary

#endif // CUSTOMITEMFACTORY_H

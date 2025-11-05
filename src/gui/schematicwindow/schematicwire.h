// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#ifndef CUSTOMWIRE_H
#define CUSTOMWIRE_H

#include "modulelibrary/itemtypes.h"

#include <qschematic/items/wire.hpp>

namespace SchematicCustom {

/**
 * @brief Custom wire with bus visualization support
 * @details Draws thicker background line for bus connections
 */
class CustomWire : public QSchematic::Items::Wire
{
    Q_OBJECT

public:
    explicit CustomWire(int type = ModuleLibrary::CustomWireType, QGraphicsItem *parent = nullptr);

    bool isBusWire() const;
    void setBusWire(bool isBus);

    gpds::container to_container() const override;
    void            from_container(const gpds::container &container) override;

protected:
    void paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget) override;

private:
    bool m_isBusWire = false;
};

} // namespace SchematicCustom

#endif // CUSTOMWIRE_H

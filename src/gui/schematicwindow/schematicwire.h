// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#ifndef SCHEMATICWIRE_H
#define SCHEMATICWIRE_H

#include "schematicitemtypes.h"

#include <qschematic/items/wire.hpp>

/**
 * @brief Custom wire with bus visualization support
 * @details Draws thicker background line for bus connections
 */
class SchematicWire : public QSchematic::Items::Wire
{
    Q_OBJECT

public:
    explicit SchematicWire(int type = SchematicWireType, QGraphicsItem *parent = nullptr);

    bool isBusWire() const;
    void setBusWire(bool isBus);

    gpds::container to_container() const override;
    void            from_container(const gpds::container &container) override;

protected:
    void paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget) override;

private:
    bool m_isBusWire = false;
};

#endif // SCHEMATICWIRE_H

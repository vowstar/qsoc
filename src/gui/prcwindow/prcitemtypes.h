// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#ifndef PRCITEMTYPES_H
#define PRCITEMTYPES_H

#include <QGraphicsItem>

/* Custom PRC item types (must not overlap with QSchematic types or SchematicWindow types) */
constexpr int PrcConnectorType = QGraphicsItem::UserType + 200;

#endif // PRCITEMTYPES_H

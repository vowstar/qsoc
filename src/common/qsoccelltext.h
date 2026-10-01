// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCCELLTEXT_H
#define QSOCCELLTEXT_H

#include <QString>

/**
 * @brief Text of the cell files of the qsoc_cell unit.
 */
namespace QSocCellText {

QString clock(); /**< qsoc_cell_clock.v */
QString reset(); /**< qsoc_cell_reset.v */
QString power(); /**< qsoc_cell_power.v */

} // namespace QSocCellText

#endif // QSOCCELLTEXT_H

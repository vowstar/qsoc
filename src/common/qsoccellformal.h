// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCCELLFORMAL_H
#define QSOCCELLFORMAL_H

#include <QMap>
#include <QString>
#include <QStringList>

/**
 * @brief Formal checks for the primitive cell files.
 * @details Each harness drives one cell from free inputs and states what the
 *          cell guarantees. The same files run under SymbiYosys and, with
 *          FORMAL_EXTERNAL_RESET defined and fclk as the clock, under other
 *          SystemVerilog formal tools.
 */
namespace QSocCellFormal {

/**
 * @brief Harnesses, SymbiYosys job and file list for some cell files.
 * @param cellFiles Cell file names, any of qsoc_cell_clock.v, qsoc_cell_reset.v and
 *                  qsoc_cell_power.v. Others are ignored.
 * @param cellDir   Directory of the cell files, relative to the output.
 * @return Output file name to content: one <cell>_formal.sv per cell file,
 *         check.sby and cell_formal.fl. Empty when no cell file is known.
 */
QMap<QString, QString> generate(const QStringList &cellFiles, const QString &cellDir);

/**
 * @brief Tasks of the job generate() writes for these cell files.
 */
QStringList tasks(const QStringList &cellFiles);

} // namespace QSocCellFormal

#endif // QSOCCELLFORMAL_H

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
 * @brief Harnesses and SymbiYosys job for some cell files.
 * @param sources Paths of the cell and role files, relative to the job
 *                directory. The harness of qsoc_cell_clock.v, qsoc_cell_reset.v
 *                or qsoc_cell_power.v is included when its file is listed.
 *                The cells build on the roles, so list every role too.
 * @return Output file name to content: one <cell>_formal.sv per listed cell
 *         file and check.sby. Empty when no cell file is listed.
 */
QMap<QString, QString> generate(const QStringList &sources);

/**
 * @brief Tasks of the job generate() writes for these files.
 */
QStringList tasks(const QStringList &sources);

/**
 * @brief Role contract checks: each bound role module against its generic body.
 * @param generic Bound role module name to its generic file text.
 * @param sources Paths of the bound role files and the cell models, relative
 *                to the job directory.
 * @return One <role>_contract.sv per role and contract.sby. A combinational
 *         role is proven equal for every input, a gate role equal once its
 *         latch has loaded, qsoc_sync equal after reset for several STAGES
 *         and both RESET_VALUE levels.
 */
QMap<QString, QString> contracts(const QMap<QString, QString> &generic, const QStringList &sources);

/**
 * @brief Tasks of the job contracts() writes for these roles.
 */
QStringList contractTasks(const QStringList &roles);

} // namespace QSocCellFormal

#endif // QSOCCELLFORMAL_H

// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCIOMUXFORMAL_H
#define QSOCIOMUXFORMAL_H

struct QSocIomuxFormalCollateral;
struct QSocIomuxPlan;

namespace QSocIomuxFormal {

QSocIomuxFormalCollateral generate(const QSocIomuxPlan &plan);
/**
 * @brief Harness for the pad module: the declared constraints, per pin.
 *
 * Empty when no pad cell is declared or it carries no constraint. The cell
 * itself is a stub built from the library port table, so the proof speaks
 * about the wiring this generator emits and nothing inside the cell.
 */
QSocIomuxFormalCollateral generatePad(const QSocIomuxPlan &plan);

} // namespace QSocIomuxFormal

#endif // QSOCIOMUXFORMAL_H

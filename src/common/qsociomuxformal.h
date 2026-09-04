// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCIOMUXFORMAL_H
#define QSOCIOMUXFORMAL_H

#include <QtGlobal>

struct QSocIomuxFormalCollateral;
struct QSocIomuxPlan;

namespace QSocIomuxFormal {

/** Pins one routing proof job takes when the caller does not say. */
constexpr quint32 kDefaultBankPins = 16;

/**
 * @brief Harness and job for the routing proof.
 *
 * The harness takes `PIN_LO` and `PIN_HI` parameters and asserts only the
 * pins between them, so the job file cuts the design into banks of
 * `bankPins` pins, one prove and one bmc task each, that run in parallel.
 * A design that fits one bank gets the plain `prove`, `bmc`, `cover` tasks.
 */
QSocIomuxFormalCollateral generate(const QSocIomuxPlan &plan, quint32 bankPins = kDefaultBankPins);
/**
 * @brief Harness for the pad shell: the declared constraints, per pin.
 *
 * Empty when no pad cell is declared or none carries a constraint. Every
 * cell the shell instantiates is a stub built from the library port table,
 * and a class's constraints are written into its stub, so the proof speaks
 * about the wiring this generator emits and nothing inside the cells, and
 * the design file carries no verification code.
 */
QSocIomuxFormalCollateral generatePad(const QSocIomuxPlan &plan);
/** The files every proof of the block reads: the design first, then the harnesses. */
QString generateFileList(const QSocIomuxPlan &plan);

} // namespace QSocIomuxFormal

#endif // QSOCIOMUXFORMAL_H

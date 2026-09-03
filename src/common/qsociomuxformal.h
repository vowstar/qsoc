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
 * @brief Harness for the pad module: the declared constraints, per pin.
 *
 * Empty when no pad cell is declared or it carries no constraint. The
 * constraints are written here, reaching the pad module's nets through the
 * `u_pad` instance, so the design file carries no verification code. The
 * cell itself is a stub built from the library port table, so the proof
 * speaks about the wiring this generator emits and nothing inside the cell.
 */
QSocIomuxFormalCollateral generatePad(const QSocIomuxPlan &plan);
/** The files every proof of the block reads: the design first, then the harnesses. */
QString generateFileList(const QSocIomuxPlan &plan);

} // namespace QSocIomuxFormal

#endif // QSOCIOMUXFORMAL_H

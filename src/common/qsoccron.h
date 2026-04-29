// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCCRON_H
#define QSOCCRON_H

#include <QString>

/**
 * @brief Minimal 5-field cron parser scoped to the scheduler.
 * @details Supports `minute hour dom month dow` with `*`, `N`, `N-M`,
 *          `*\/S`, `N-M/S`, and comma-lists. No L/W/?/aliases. Day-of-week
 *          accepts `7` as Sunday alias. All times interpreted in local
 *          timezone.
 *
 *          One representation handles both recurring (`*\/5 * * * *`) and
 *          one-shot pinned-date (`30 14 27 2 *`) tasks, eliminating the
 *          interval/runAt tagged-union special case.
 */
namespace QSocCron {

/**
 * @brief Validate a 5-field cron expression.
 * @return true if all fields parse and produce non-empty match sets.
 */
bool isValid(const QString &expr);

/**
 * @brief Strictly-greater-than next match in epoch ms.
 * @param expr   5-field cron expression.
 * @param fromMs Anchor; the result satisfies `result > fromMs` rounded to
 *               the next whole minute boundary (cron has minute resolution).
 * @return Epoch ms of the next match, or 0 on invalid expression / no
 *         match within 366 days.
 * @details DoM/DoW OR semantics: when both fields are constrained
 *          (neither is wildcard), a calendar day matches if EITHER matches
 *          (Vixie cron). When one is wildcard, the other constrains alone.
 *          Caller is expected to feed `lastFiredAt > 0 ? lastFiredAt :
 *          createdAt` so the same-minute reschedule is impossible.
 */
qint64 nextRunMs(const QString &expr, qint64 fromMs);

/**
 * @brief Translate `/loop` interval tokens (`5m`, `2h`, `1d`) into cron.
 * @details Mirrors the claude-code skill table:
 *          - `Nm` (N <= 59 and 60 % N == 0) -> `*\/N * * * *`
 *          - `Nm` (N >= 60 and N % 60 == 0 and 24 % (N/60) == 0) -> `0 *\/H * * *`
 *          - `Nh` (24 % N == 0)             -> `0 *\/N * * *`
 *          - `Nd`                           -> `0 0 *\/N * *`
 *          - `Ns`: rounded up to whole minutes (>=1m), then as `Nm` above.
 *          Returns empty QString when N does not divide its unit
 *          (`7m`, `90m`, `5h`); the caller must surface this as a parse
 *          error to the user, not silently approximate.
 */
QString intervalToCron(const QString &token);

/**
 * @brief Render a cron expression as a short human token for `/loop list`.
 * @details Best-effort: recognized common shapes return short forms
 *          (`Every 5 minutes`, `Daily 09:00`, `Hourly :07`, `Weekly Mon
 *          09:00`). Anything else falls through to the raw expression.
 */
QString cronToHuman(const QString &expr);

} /* namespace QSocCron */

#endif /* QSOCCRON_H */

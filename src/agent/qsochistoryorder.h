// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCHISTORYORDER_H
#define QSOCHISTORYORDER_H

#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

/**
 * @brief Pure ordering/dedup for cross-project command history recall.
 * @details The global history holds commands from every project. For
 *          Up-arrow / Ctrl-R recall the current project's commands should be
 *          surfaced before other projects', and a command repeated across
 *          projects should appear once at its most-recent position. Kept side
 *          effect free so the policy is unit-testable; the CLI maps the
 *          returned indices back to its aligned paste maps.
 */
namespace QSocHistoryOrder {

/**
 * @brief Compute kept entry indices in recall order.
 * @param displays Per-entry display text, in chronological (file) order.
 * @param current  Per-entry flag: true when the entry belongs to the active
 *                 project. Must be the same length as @p displays.
 * @return Indices into @p displays, with other-project entries first and
 *         current-project entries last, deduped by display text keeping the
 *         most recent occurrence. The result is ordered so the last element
 *         is what Up-arrow reaches first.
 */
inline QList<int> orderedDedup(const QStringList &displays, const QList<bool> &current)
{
    QList<int> ordered;
    ordered.reserve(displays.size());
    for (int i = 0; i < displays.size() && i < current.size(); ++i) {
        if (!current[i]) {
            ordered.append(i);
        }
    }
    for (int i = 0; i < displays.size() && i < current.size(); ++i) {
        if (current[i]) {
            ordered.append(i);
        }
    }

    QSet<QString> seen;
    QList<int>    kept;
    for (int j = ordered.size() - 1; j >= 0; --j) {
        const QString &display = displays[ordered[j]];
        if (seen.contains(display)) {
            continue;
        }
        seen.insert(display);
        kept.prepend(ordered[j]);
    }
    return kept;
}

} // namespace QSocHistoryOrder

#endif // QSOCHISTORYORDER_H

// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsoclinediff.h"

#include <QVector>

QStringList QSocLineDiff::splitLines(const QString &text)
{
    /* Use Qt::KeepEmptyParts so trailing newlines produce a tail empty
     * entry — that way "a\n" and "a" don't collide and the diff stays
     * faithful to what the file actually contains. */
    if (text.isEmpty()) {
        return {};
    }
    return text.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
}

QList<QSocLineDiff::DiffLine> QSocLineDiff::computeLineDiff(
    const QString &oldText, const QString &newText, int contextLines)
{
    QList<DiffLine>   result;
    const QStringList oldLines = splitLines(oldText);
    const QStringList newLines = splitLines(newText);
    const int         oldCount = static_cast<int>(oldLines.size());
    const int         newCount = static_cast<int>(newLines.size());

    /* Edge cases short-circuit so the LCS table doesn't allocate when
     * we know the answer up front. */
    if (oldCount == 0 && newCount == 0) {
        return result;
    }
    if (oldCount == 0) {
        result.append(
            {.kind = Kind::Hunk, .text = QStringLiteral("@@ -0,0 +1,%1 @@").arg(newCount)});
        for (const QString &line : newLines) {
            result.append({.kind = Kind::Add, .text = QStringLiteral("+") + line});
        }
        return result;
    }
    if (newCount == 0) {
        result.append(
            {.kind = Kind::Hunk, .text = QStringLiteral("@@ -1,%1 +0,0 @@").arg(oldCount)});
        for (const QString &line : oldLines) {
            result.append({.kind = Kind::Del, .text = QStringLiteral("-") + line});
        }
        return result;
    }

    /* Classic LCS DP table. Table size is (oldCount+1) x (newCount+1). For
     * very large inputs (>5000 lines on each side) this would balloon, but
     * file edit diffs are usually small enough that O(n*m) is fine. */
    QVector<QVector<int>> table(oldCount + 1, QVector<int>(newCount + 1, 0));
    for (int oldIdx = 1; oldIdx <= oldCount; oldIdx++) {
        for (int newIdx = 1; newIdx <= newCount; newIdx++) {
            if (oldLines[oldIdx - 1] == newLines[newIdx - 1]) {
                table[oldIdx][newIdx] = table[oldIdx - 1][newIdx - 1] + 1;
            } else {
                table[oldIdx][newIdx] = qMax(table[oldIdx - 1][newIdx], table[oldIdx][newIdx - 1]);
            }
        }
    }

    /* Walk the DP table back to a flat operation list, then reverse so we
     * emit lines in source order. Operation kinds: Context (=), Add (+),
     * Del (-). We compute the full edit script first, then collapse it
     * into hunks with N lines of context around each change. */
    QList<DiffLine> ops;
    int             oldIdx = oldCount;
    int             newIdx = newCount;
    while (oldIdx > 0 && newIdx > 0) {
        if (oldLines[oldIdx - 1] == newLines[newIdx - 1]) {
            ops.append({.kind = Kind::Context, .text = QStringLiteral(" ") + oldLines[oldIdx - 1]});
            oldIdx--;
            newIdx--;
        } else if (table[oldIdx - 1][newIdx] > table[oldIdx][newIdx - 1]) {
            /* Strict > so a tie favours Add (consume newLines first) — after
             * the script is reversed for display order this lands the Del
             * line BEFORE the Add line, matching traditional unified diff. */
            ops.append({.kind = Kind::Del, .text = QStringLiteral("-") + oldLines[oldIdx - 1]});
            oldIdx--;
        } else {
            ops.append({.kind = Kind::Add, .text = QStringLiteral("+") + newLines[newIdx - 1]});
            newIdx--;
        }
    }
    while (oldIdx > 0) {
        ops.append({.kind = Kind::Del, .text = QStringLiteral("-") + oldLines[oldIdx - 1]});
        oldIdx--;
    }
    while (newIdx > 0) {
        ops.append({.kind = Kind::Add, .text = QStringLiteral("+") + newLines[newIdx - 1]});
        newIdx--;
    }
    /* The script was built from end to start; reverse for natural reading. */
    QList<DiffLine> script;
    script.reserve(ops.size());
    for (int idx = ops.size() - 1; idx >= 0; idx--) {
        script.append(ops[idx]);
    }

    /* Collapse runs of context: keep `contextLines` around each change
     * cluster, drop the rest, and inject a hunk header at the start of
     * each visible region. */
    if (contextLines < 0) {
        contextLines = 0;
    }

    /* Track per-script-position the original old/new line numbers. */
    QVector<int> oldLineNumbers(script.size(), 0);
    QVector<int> newLineNumbers(script.size(), 0);
    int          oldCursor = 1;
    int          newCursor = 1;
    for (int idx = 0; idx < script.size(); idx++) {
        oldLineNumbers[idx] = oldCursor;
        newLineNumbers[idx] = newCursor;
        switch (script[idx].kind) {
        case Kind::Context:
            oldCursor++;
            newCursor++;
            break;
        case Kind::Add:
            newCursor++;
            break;
        case Kind::Del:
            oldCursor++;
            break;
        case Kind::Hunk:
            break;
        }
    }

    /* Mark which script positions are "kept" — every change plus its
     * neighbouring context window. */
    QVector<bool> keep(script.size(), false);
    for (int idx = 0; idx < script.size(); idx++) {
        if (script[idx].kind == Kind::Add || script[idx].kind == Kind::Del) {
            int start = qMax(0, idx - contextLines);
            int end   = qMin(static_cast<int>(script.size()) - 1, idx + contextLines);
            for (int kept = start; kept <= end; kept++) {
                keep[kept] = true;
            }
        }
    }

    /* Walk kept ranges, emitting one hunk header per contiguous range. */
    int idx = 0;
    while (idx < script.size()) {
        if (!keep[idx]) {
            idx++;
            continue;
        }
        int hunkStart = idx;
        while (idx < script.size() && keep[idx]) {
            idx++;
        }
        int hunkEnd = idx; /* exclusive */

        int oldStart = oldLineNumbers[hunkStart];
        int newStart = newLineNumbers[hunkStart];
        int oldRun   = 0;
        int newRun   = 0;
        for (int kept = hunkStart; kept < hunkEnd; kept++) {
            switch (script[kept].kind) {
            case Kind::Context:
                oldRun++;
                newRun++;
                break;
            case Kind::Add:
                newRun++;
                break;
            case Kind::Del:
                oldRun++;
                break;
            case Kind::Hunk:
                break;
            }
        }
        result.append(
            {Kind::Hunk,
             QStringLiteral("@@ -%1,%2 +%3,%4 @@")
                 .arg(oldStart)
                 .arg(oldRun)
                 .arg(newStart)
                 .arg(newRun)});
        for (int kept = hunkStart; kept < hunkEnd; kept++) {
            result.append(script[kept]);
        }
    }

    return result;
}

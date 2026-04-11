// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "cli/qagenthistorysearch.h"

QAgentHistorySearch::QAgentHistorySearch(const QStringList &historyRef)
    : history(&historyRef)
{
    rewind();
}

void QAgentHistorySearch::reset()
{
    scanCursor = history ? static_cast<int>(history->size()) - 1 : -1;
    seen.clear();
}

void QAgentHistorySearch::rewind()
{
    reset();
    lastQuery.clear();
}

QAgentHistorySearch::Match QAgentHistorySearch::findNext(const QString &query)
{
    Match result;

    if (query.isEmpty()) {
        /* Empty query: rewind so the next non-empty search starts fresh. */
        reset();
        lastQuery.clear();
        return result;
    }

    /* Switching queries restarts the scan from the newest entry and drops
     * the seen-set so previously skipped matches reappear. */
    if (query != lastQuery) {
        reset();
        lastQuery = query;
    }

    if (!history) {
        return result;
    }

    while (scanCursor >= 0) {
        const QString &entry = (*history)[scanCursor];
        scanCursor--;
        if (seen.contains(entry)) {
            continue;
        }
        if (entry.contains(query)) {
            seen.insert(entry);
            result.index = scanCursor + 1; /* Adjust for the decrement above */
            result.text  = entry;
            return result;
        }
    }

    /* Exhausted: no more unique matches. Caller can choose to restart. */
    return result;
}

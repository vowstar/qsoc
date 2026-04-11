// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QAGENTHISTORYSEARCH_H
#define QAGENTHISTORYSEARCH_H

#include <QSet>
#include <QString>
#include <QStringList>

/**
 * @brief Reverse-i-search helper for the agent input history.
 * @details Pure-logic class (no Qt signals) that walks a QStringList of
 *          history entries from newest to oldest looking for a substring
 *          match. Duplicate entries are skipped so Ctrl+R advances through
 *          unique matches only.
 *
 *          Usage pattern (owned by the REPL, constructed fresh per search):
 *            QAgentHistorySearch search(history);
 *            auto result = search.findNext("foo");   // first match
 *            result = search.findNext("foo");        // next older unique match
 *            search.reset();                         // start over
 */
class QAgentHistorySearch
{
public:
    struct Match
    {
        int     index = -1; /* Position in history list (-1 = no match) */
        QString text;       /* Matched entry */
    };

    explicit QAgentHistorySearch(const QStringList &history);

    /**
     * @brief Find the next older unique substring match for the given query.
     * @param query Substring to search for (empty → no match)
     * @return Match with index>=0 and text on success; index<0 on no match.
     *         Calling again with the same query advances to the next older
     *         unique match. Calling with a different query resets the scan.
     */
    Match findNext(const QString &query);

    /**
     * @brief Reset internal cursor and seen-set so the next findNext() starts
     *        fresh from the newest entry.
     */
    void reset();

    /**
     * @brief Discard all state so the helper can be reused with a new history.
     */
    void rewind();

private:
    const QStringList *history    = nullptr;
    int                scanCursor = -1; /* Next index to examine (walks backward) */
    QString            lastQuery;
    QSet<QString>      seen;
};

#endif // QAGENTHISTORYSEARCH_H

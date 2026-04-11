// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCLINEDIFF_H
#define QSOCLINEDIFF_H

#include <QList>
#include <QString>
#include <QStringList>

#include <cstdint>

/**
 * @brief Pure-logic line-diff engine for displaying file edits.
 * @details Computes a unified-diff-style sequence of lines between two
 *          QStrings using the classic Longest Common Subsequence algorithm.
 *          Output is a flat list of DiffLine entries that the REPL can
 *          forward to the scroll view with appropriate styles.
 */
class QSocLineDiff
{
public:
    enum class Kind : std::uint8_t {
        Context, /* Unchanged line, shown for context */
        Add,     /* Line present in newText but not oldText */
        Del,     /* Line present in oldText but not newText */
        Hunk,    /* Synthetic header marking a contiguous change region */
    };

    struct DiffLine
    {
        Kind    kind;
        QString text;
    };

    /**
     * @brief Compute a unified-style line diff between two text blobs.
     * @param oldText      Original text (will be split on '\n').
     * @param newText      Modified text (will be split on '\n').
     * @param contextLines Number of unchanged lines to keep around each
     *                     change hunk. Default 3.
     * @return Flat list of DiffLine entries in display order.
     */
    static QList<DiffLine> computeLineDiff(
        const QString &oldText, const QString &newText, int contextLines = 3);

private:
    static QStringList splitLines(const QString &text);
};

#endif // QSOCLINEDIFF_H

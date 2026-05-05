// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QTUISCROLLVIEW_H
#define QTUISCROLLVIEW_H

#include "tui/qtuiscreen.h"

#include <QList>
#include <QStringList>

/**
 * @brief Scrollable text content area with line history
 * @details Stores all output lines. Renders a viewport window.
 *          Auto-scrolls to bottom when new content arrives (unless user scrolled up).
 *          Each line can be normal, dim (thinking), or bold (tool), or
 *          carry a list of styled runs for mixed-style markdown output.
 */
class QTuiScrollView
{
public:
    enum LineStyle : int {
        Normal      = 0,
        Dim         = 1,
        Bold        = 2,
        DiffAdd     = 3, /* '+' lines, rendered green */
        DiffDel     = 4, /* '-' lines, rendered red */
        DiffHunk    = 5, /* '@@' headers, rendered yellow + bold */
        DiffContext = 6, /* unchanged context lines, dim */
        Styled      = 7, /* runs[] holds mixed-style segments; text unused */
    };

    /* Append a complete line */
    void appendLine(const QString &text, LineStyle style = Normal);

    /* Append a styled-runs line. Each run carries its own bold/italic/
     * dim/underline/fg/bg attributes; render() paints them cell-by-cell
     * with soft-wrap that preserves run boundaries. */
    void appendStyledLine(const QList<QTuiStyledRun> &runs);

    /* Replace the most-recently-appended line with a new styled-runs
     * payload. Used by the streaming markdown pipeline so the
     * in-progress final line gets repainted on every chunk without
     * trailing duplicates. No-op if the buffer is empty. */
    void replaceLastStyledLine(const QList<QTuiStyledRun> &runs);

    /* Append partial text (streaming). Completes on \n. */
    void appendPartial(const QString &text, LineStyle style = Normal);

    /* Render the viewport to a screen buffer region.
     * Reserves 1 column on the right for ASCII scrollbar. */
    void render(QTuiScreen &screen, int startRow, int height, int width);

    static constexpr int MAX_LINES = 65536;

    /* Scroll control */
    void scrollUp(int lines = 1);
    void scrollDown(int lines = 1);
    void scrollToBottom();
    bool isAtBottom() const;

    /* Clear all content */
    void clear();

    int totalLines() const { return static_cast<int>(lines.size()); }

    /* Get all content as plain text (for dumping after alt screen exit) */
    QString toPlainText() const;

private:
    struct Line
    {
        QString              text;
        LineStyle            style = Normal;
        QList<QTuiStyledRun> runs; /* Populated when style == Styled */
    };

    QList<Line> lines;
    QString     partialLine; /* Current incomplete line (streaming) */
    LineStyle   partialStyle = Normal;
    int         scrollOffset = 0; /* 0 = at bottom (auto-scroll) */
};

#endif // QTUISCROLLVIEW_H

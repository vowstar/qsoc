// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QTUISCROLLVIEW_H
#define QTUISCROLLVIEW_H

#include "tui/qtuiscreen.h"

#include <QStringList>

/**
 * @brief Scrollable text content area with line history
 * @details Stores all output lines. Renders a viewport window.
 *          Auto-scrolls to bottom when new content arrives (unless user scrolled up).
 *          Each line can be normal, dim (thinking), or bold (tool).
 */
class QTuiScrollView
{
public:
    enum LineStyle : int { Normal = 0, Dim = 1, Bold = 2 };

    /* Append a complete line */
    void appendLine(const QString &text, LineStyle style = Normal);

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
        QString   text;
        LineStyle style = Normal;
    };

    QList<Line> lines;
    QString     partialLine; /* Current incomplete line (streaming) */
    LineStyle   partialStyle = Normal;
    int         scrollOffset = 0; /* 0 = at bottom (auto-scroll) */
};

#endif // QTUISCROLLVIEW_H

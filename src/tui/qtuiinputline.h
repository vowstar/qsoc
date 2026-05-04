// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QTUIINPUTLINE_H
#define QTUIINPUTLINE_H

#include "tui/qtuiwidget.h"

#include <QString>
#include <QVector>

/**
 * @brief User input line widget at bottom of screen
 * @details Supports multi-line text (split on \n) with soft-wrap on the
 *          terminal width, up to MAX_VISIBLE_LINES visual rows. Tracks a
 *          cursor position for IME placement and line editing feedback.
 */
class QTuiInputLine : public QTuiWidget
{
public:
    static constexpr int MAX_VISIBLE_LINES = 10;

    int  lineCount() const override;
    void render(QTuiScreen &screen, int startY, int width) override;

    void    setText(const QString &newText);
    void    clear();
    QString getText() const { return text; }

    /**
     * @brief Tell the widget the current terminal width so lineCount() and
     *        cursor metrics can account for soft-wrap of long logical lines.
     * @details The compositor must call this before lineCount() during
     *          layout, since the input row height depends on how many
     *          visual rows the wrapped text occupies. A non-positive value
     *          is ignored.
     */
    void setTerminalWidth(int cols);

    /**
     * @brief Set the dim placeholder shown on an empty, non-search input line.
     * @details Rendered after the "> " prompt in dim style when the buffer is
     *          empty so new users can see available shortcuts at a glance.
     *          Pass an empty string to disable.
     */
    void setPlaceholder(const QString &hint) { placeholder = hint; }

    /**
     * @brief Set the dim trailing hint shown after the buffer text.
     * @details Used for inline slash-command argument hints like
     *          "/effort " → "<dim>off|low|medium|high</dim>". Rendered only
     *          when the buffer is non-empty, occupies a single visual row,
     *          and not in search mode. Empty hint disables the feature.
     */
    void setTrailingHint(const QString &hint) { trailingHint = hint; }

    /* Set cursor position (QChar index into text) for cursor rendering */
    void setCursorPos(int pos);
    int  getCursorPos() const { return cursorPos; }

    /* Cursor screen row (0-indexed within the input widget's visible area) */
    int cursorLine() const;

    /* Cursor screen column (0-indexed, includes prompt prefix + visual width) */
    int cursorColumn() const;

    /**
     * @brief Enable or disable reverse-i-search display mode.
     * @details When active, the normal text buffer is hidden and the widget
     *          renders "(bck-i-search)`<query>': <match>" or
     *          "(failing bck-i-search)`<query>': " on no match. The cursor
     *          is parked at the end of the query portion for IME support.
     */
    void setSearchMode(bool active, const QString &query, const QString &match, bool failed);
    bool isSearchMode() const { return searchMode; }

private:
    /**
     * @brief One visual (post-wrap) row of the input area.
     * @details Each logical line (separated by \n in the raw buffer) yields
     *          one or more VisualRow entries depending on how it wraps at
     *          the current terminal width. promptWidth is non-zero only on
     *          the first visual row of a logical line; continuation rows
     *          render with no prefix so the wrapped content uses the full
     *          width and stays close to its visual neighbors.
     */
    struct VisualRow
    {
        QString prompt;       /* Prefix to render at column 0 (may be empty) */
        int     promptWidth;  /* Visual width cells consumed by prompt */
        int     contentStart; /* QChar offset into text where this row's content begins */
        int     contentLen;   /* Length in QChars of the content slice (no prompt) */
    };

    QVector<VisualRow> buildVisualRows() const;
    int                takeFitChars(int startIdx, int capacity) const;

    QString text;
    int     cursorPos     = 0;
    int     terminalWidth = 80;
    QString placeholder;
    QString trailingHint;

    bool    searchMode = false;
    QString searchQuery;
    QString searchMatch;
    bool    searchFailed = false;
};

#endif // QTUIINPUTLINE_H

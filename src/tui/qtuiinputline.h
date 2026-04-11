// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QTUIINPUTLINE_H
#define QTUIINPUTLINE_H

#include "tui/qtuiwidget.h"

#include <QString>

/**
 * @brief User input line widget at bottom of screen
 * @details Supports multi-line text (split on \n), up to MAX_VISIBLE_LINES
 *          visible rows. Tracks a cursor position for IME placement and
 *          line editing feedback.
 */
class QTuiInputLine : public QTuiWidget
{
public:
    static constexpr int MAX_VISIBLE_LINES = 10;

    int  lineCount() const override;
    void render(QTuiScreen &screen, int startY, int width) override;

    void    setText(const QString &text);
    void    clear();
    QString getText() const { return text; }

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
     *          when the buffer is non-empty, single-line, and not in search
     *          mode. Empty hint disables the feature.
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
    QString text;
    int     cursorPos = 0;
    QString placeholder;
    QString trailingHint;

    bool    searchMode = false;
    QString searchQuery;
    QString searchMatch;
    bool    searchFailed = false;
};

#endif // QTUIINPUTLINE_H

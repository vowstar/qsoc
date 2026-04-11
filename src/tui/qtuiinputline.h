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

    /* Set cursor position (QChar index into text) for cursor rendering */
    void setCursorPos(int pos);
    int  getCursorPos() const { return cursorPos; }

    /* Cursor screen row (0-indexed within the input widget's visible area) */
    int cursorLine() const;

    /* Cursor screen column (0-indexed, includes prompt prefix + visual width) */
    int cursorColumn() const;

private:
    QString text;
    int     cursorPos = 0;
};

#endif // QTUIINPUTLINE_H

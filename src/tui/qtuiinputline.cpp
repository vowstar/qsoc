// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "tui/qtuiinputline.h"

int QTuiInputLine::lineCount() const
{
    return 1; /* Always show input line */
}

void QTuiInputLine::render(QTuiScreen &screen, int startY, int width)
{
    QString display;
    if (text.startsWith("!")) {
        display = "! " + text.mid(1);
    } else {
        display = "> " + text;
    }

    screen.putString(0, startY, display.left(width));
}

void QTuiInputLine::setText(const QString &newText)
{
    text = newText;
}

void QTuiInputLine::clear()
{
    text.clear();
}

int QTuiInputLine::cursorColumn() const
{
    /* "> " prefix = 2 columns, then text visual width */
    return 2 + QTuiText::visualWidth(text);
}

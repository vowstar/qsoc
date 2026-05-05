// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "tui/qtuiscreen.h"

#include "tui/qtuiwidget.h"

QTuiCell QTuiScreen::defaultCell;

QTuiScreen::QTuiScreen(int width, int height)
{
    resize(width, height);
}

void QTuiScreen::resize(int width, int height)
{
    cols = width;
    rows = height;
    cells.resize(rows);
    prevCells.resize(rows);
    for (int row = 0; row < rows; row++) {
        cells[row].resize(cols);
        prevCells[row].resize(cols);
    }
    fullRedraw = true;
}

void QTuiScreen::clear()
{
    for (int row = 0; row < rows; row++) {
        for (int col = 0; col < cols; col++) {
            cells[row][col] = QTuiCell();
        }
    }
}

QTuiCell &QTuiScreen::at(int col, int row)
{
    if (row >= 0 && row < rows && col >= 0 && col < cols) {
        return cells[row][col];
    }
    return defaultCell;
}

const QTuiCell &QTuiScreen::at(int col, int row) const
{
    if (row >= 0 && row < rows && col >= 0 && col < cols) {
        return cells[row][col];
    }
    return defaultCell;
}

void QTuiScreen::putChar(
    int         col,
    int         row,
    QChar       ch,
    bool        bold,
    bool        dim,
    bool        inverted,
    QTuiFgColor fgColor,
    QTuiBgColor bgColor)
{
    if (row < 0 || row >= rows || col < 0 || col >= cols) {
        return;
    }
    auto &cell     = cells[row][col];
    cell.character = ch;
    cell.bold      = bold;
    cell.dim       = dim;
    cell.inverted  = inverted;
    cell.fgColor   = fgColor;
    cell.bgColor   = bgColor;
}

void QTuiScreen::putString(
    int            col,
    int            row,
    const QString &text,
    bool           bold,
    bool           dim,
    bool           inverted,
    QTuiFgColor    fgColor,
    QTuiBgColor    bgColor)
{
    if (row < 0 || row >= rows) {
        return;
    }
    int idx = 0;
    int len = text.length();
    int pos = col;

    while (idx < len && pos < cols) {
        QChar chr = text[idx];

        /* Skip ANSI CSI sequences */
        if (chr == '\033' && idx + 1 < len && text[idx + 1] == '[') {
            idx += 2;
            while (idx < len) {
                ushort code = text[idx].unicode();
                idx++;
                if (code >= 0x40 && code <= 0x7E) {
                    break;
                }
            }
            continue;
        }
        if (chr == '\033') {
            idx += 2;
            continue;
        }

        if (pos >= 0) {
            putChar(pos, row, chr, bold, dim, inverted, fgColor, bgColor);
        }
        /* Wide chars (CJK / emoji) occupy two terminal cells; advance
         * the write cursor by the visual width so consecutive wide
         * chars do not stomp on each other's trailing cells. The
         * trailing cell is left as the default ' ' which toAnsi then
         * skips, keeping the screen buffer in sync with what the
         * terminal actually shows. */
        pos += QTuiText::isWideChar(chr.unicode()) ? 2 : 1;
        idx++;
    }
}

void QTuiScreen::hline(int row, QChar ch)
{
    if (row < 0 || row >= rows) {
        return;
    }
    for (int col = 0; col < cols; col++) {
        cells[row][col].character = ch;
        cells[row][col].bold      = false;
        cells[row][col].dim       = true;
        cells[row][col].inverted  = false;
    }
}

QString QTuiScreen::toAnsi()
{
    QString output;
    output.reserve(rows * cols * 2);

    /* Cursor home */
    output += "\033[H";

    bool        currentBold      = false;
    bool        currentItalic    = false;
    bool        currentDim       = false;
    bool        currentUnderline = false;
    bool        currentInverted  = false;
    QTuiFgColor currentColor     = QTuiFgColor::Default;
    QTuiBgColor currentBg        = BG_DEFAULT;

    for (int row = 0; row < rows; row++) {
        /* Check if this row changed (skip unchanged rows for performance) */
        if (!fullRedraw && row < prevCells.size()) {
            bool rowChanged = false;
            for (int col = 0; col < cols; col++) {
                if (cells[row][col] != prevCells[row][col]) {
                    rowChanged = true;
                    break;
                }
            }
            if (!rowChanged) {
                /* Move cursor to next row */
                if (row < rows - 1) {
                    output += QString("\033[%1;1H").arg(row + 2);
                }
                continue;
            }
        }

        /* Position cursor at row start */
        output += QString("\033[%1;1H").arg(row + 1);

        for (int col = 0; col < cols;) {
            const QTuiCell &cell = cells[row][col];
            /* Wide chars (CJK, emoji) occupy two cells visually. The
             * second cell is left as the default ' ' by paintRow but
             * must not be emitted, otherwise the terminal renders an
             * extra space and pushes following content one cell to the
             * right. Detect, emit once, skip the trailing cell. */
            const int charWidth = QTuiText::isWideChar(cell.character.unicode()) ? 2 : 1;

            /* Emit style changes */
            bool needReset = false;
            if (cell.bold != currentBold || cell.italic != currentItalic || cell.dim != currentDim
                || cell.underline != currentUnderline || cell.inverted != currentInverted
                || cell.fgColor != currentColor || cell.bgColor != currentBg) {
                needReset = true;
            }

            if (needReset) {
                output += "\033[0m";
                currentBold      = false;
                currentItalic    = false;
                currentDim       = false;
                currentUnderline = false;
                currentInverted  = false;
                currentColor     = QTuiFgColor::Default;
                currentBg        = BG_DEFAULT;

                QString attrs;
                if (cell.bold) {
                    attrs += "1;";
                }
                if (cell.dim) {
                    attrs += "2;";
                }
                if (cell.italic) {
                    attrs += "3;";
                }
                if (cell.underline) {
                    attrs += "4;";
                }
                if (cell.inverted) {
                    attrs += "7;";
                }
                if (cell.fgColor != QTuiFgColor::Default) {
                    attrs += "38;5;" + QString::number(static_cast<int>(cell.fgColor)) + ";";
                }
                if (cell.bgColor != BG_DEFAULT) {
                    attrs += "48;5;" + QString::number(static_cast<int>(cell.bgColor)) + ";";
                }
                if (!attrs.isEmpty()) {
                    attrs.chop(1);
                    output += "\033[" + attrs + "m";
                }
                currentBold      = cell.bold;
                currentItalic    = cell.italic;
                currentDim       = cell.dim;
                currentUnderline = cell.underline;
                currentInverted  = cell.inverted;
                currentColor     = cell.fgColor;
                currentBg        = cell.bgColor;
            }

            output += cell.character;
            col += charWidth;
        }

        /* Clear to end of line (handles terminal width mismatch) */
        output += "\033[K";
    }

    /* Reset attributes at end */
    if (currentBold || currentDim || currentInverted || currentColor != QTuiFgColor::Default
        || currentBg != BG_DEFAULT) {
        output += "\033[0m";
    }

    /* Save current frame as previous */
    prevCells  = cells;
    fullRedraw = false;

    return output;
}

void QTuiScreen::invalidate()
{
    fullRedraw = true;
}

// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "tui/qtuiblock.h"

#include "tui/qtuiwidget.h"

namespace {

/* Emit one screen row as a self-contained ANSI string without any
 * cursor positioning escapes. The output stays in flow with whatever
 * came before / after, which is what the cooked-mode dumper wants. */
QString rowToAnsi(const QTuiScreen &screen, int row, int width)
{
    QString     output;
    QString     currentLink;
    bool        curBold      = false;
    bool        curItalic    = false;
    bool        curDim       = false;
    bool        curUnderline = false;
    bool        curInverted  = false;
    QTuiFgColor curFg        = QTuiFgColor::Default;
    QTuiBgColor curBg        = BG_DEFAULT;
    bool        anyStyle     = false;

    auto resetStyle = [&]() {
        if (anyStyle) {
            output += QStringLiteral("\033[0m");
        }
        curBold      = false;
        curItalic    = false;
        curDim       = false;
        curUnderline = false;
        curInverted  = false;
        curFg        = QTuiFgColor::Default;
        curBg        = BG_DEFAULT;
        anyStyle     = false;
    };

    for (int col = 0; col < width;) {
        const QTuiCell &cell = screen.at(col, row);
        const int       chW  = QTuiText::isWideChar(cell.character.unicode()) ? 2 : 1;

        const bool changed = cell.bold != curBold || cell.italic != curItalic || cell.dim != curDim
                             || cell.underline != curUnderline || cell.inverted != curInverted
                             || cell.fgColor != curFg || cell.bgColor != curBg;
        if (changed) {
            resetStyle();
            QString attrs;
            if (cell.bold) {
                attrs += QStringLiteral("1;");
            }
            if (cell.dim) {
                attrs += QStringLiteral("2;");
            }
            if (cell.italic) {
                attrs += QStringLiteral("3;");
            }
            if (cell.underline) {
                attrs += QStringLiteral("4;");
            }
            if (cell.inverted) {
                attrs += QStringLiteral("7;");
            }
            if (cell.fgColor != QTuiFgColor::Default) {
                attrs += QStringLiteral("38;5;%1;").arg(static_cast<int>(cell.fgColor));
            }
            if (cell.bgColor != BG_DEFAULT) {
                attrs += QStringLiteral("48;5;%1;").arg(static_cast<int>(cell.bgColor));
            }
            if (!attrs.isEmpty()) {
                attrs.chop(1);
                output += QStringLiteral("\033[") + attrs + QLatin1Char('m');
                anyStyle = true;
            }
            curBold      = cell.bold;
            curItalic    = cell.italic;
            curDim       = cell.dim;
            curUnderline = cell.underline;
            curInverted  = cell.inverted;
            curFg        = cell.fgColor;
            curBg        = cell.bgColor;
        }

        if (cell.hyperlink != currentLink) {
            if (!currentLink.isEmpty()) {
                output += QStringLiteral("\x1b]8;;\x1b\\");
            }
            if (!cell.hyperlink.isEmpty()) {
                output += QStringLiteral("\x1b]8;;");
                output += cell.hyperlink;
                output += QStringLiteral("\x1b\\");
            }
            currentLink = cell.hyperlink;
        }

        output += cell.character;
        col += chW;
    }

    if (!currentLink.isEmpty()) {
        output += QStringLiteral("\x1b]8;;\x1b\\");
    }
    resetStyle();
    return output;
}

} // namespace

QString QTuiBlock::toAnsi(int width)
{
    if (width <= 0) {
        return QString();
    }
    layout(width);
    const int rows = rowCount();
    if (rows <= 0) {
        return QString();
    }
    QTuiScreen screen(width, rows);
    for (int row = 0; row < rows; ++row) {
        paintRow(screen, row, row, xOffset(), width, /*focused=*/false, /*selected=*/false);
    }
    QString out;
    for (int row = 0; row < rows; ++row) {
        out.append(rowToAnsi(screen, row, width));
        out.append(QLatin1Char('\n'));
    }
    return out;
}

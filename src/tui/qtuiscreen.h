// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QTUISCREEN_H
#define QTUISCREEN_H

#include <QString>
#include <QVector>

#include <cstdint>

/**
 * @brief Foreground color enum for cell text. Default leaves the terminal's
 *        current foreground untouched. Named values are 256-palette indices
 *        chosen for a warm, muted aesthetic on dark backgrounds.
 */
enum class QTuiFgColor : std::uint8_t {
    Default = 0,
    Red     = 167, /* warm red */
    Green   = 142, /* olive green */
    Yellow  = 214, /* amber */
    Blue    = 109, /* muted blue */
    Magenta = 175, /* dusty pink */
    Cyan    = 108, /* sage green */
    Orange  = 208, /* warm orange */
    Gray    = 246, /* neutral gray */
};

/**
 * @brief A single terminal cell with character and style attributes
 */
struct QTuiCell
{
    QChar       character = ' ';
    bool        bold      = false;
    bool        dim       = false;
    bool        inverted  = false;
    QTuiFgColor fgColor   = QTuiFgColor::Default;

    bool operator==(const QTuiCell &other) const
    {
        return character == other.character && bold == other.bold && dim == other.dim
               && inverted == other.inverted && fgColor == other.fgColor;
    }
    bool operator!=(const QTuiCell &other) const { return !(*this == other); }
};

/**
 * @brief 2D terminal screen buffer with ANSI output
 * @details Full-screen buffer that renders to ANSI escape sequences.
 *          Tracks previous frame for differential output.
 */
class QTuiScreen
{
public:
    QTuiScreen() = default;
    QTuiScreen(int width, int height);

    void resize(int width, int height);
    void clear();

    int width() const { return cols; }
    int height() const { return rows; }

    /* Cell access */
    QTuiCell       &at(int col, int row);
    const QTuiCell &at(int col, int row) const;

    /* Drawing primitives */
    void putChar(
        int         col,
        int         row,
        QChar       ch,
        bool        bold     = false,
        bool        dim      = false,
        bool        inverted = false,
        QTuiFgColor fgColor  = QTuiFgColor::Default);
    void putString(
        int            col,
        int            row,
        const QString &text,
        bool           bold     = false,
        bool           dim      = false,
        bool           inverted = false,
        QTuiFgColor    fgColor  = QTuiFgColor::Default);

    /* Draw a horizontal line of a character across full width */
    void hline(int row, QChar ch = '-');

    /* Convert current buffer to ANSI escape sequence string.
     * Compares with previous frame for minimal output. */
    QString toAnsi();

    /* Force full redraw on next toAnsi() call */
    void invalidate();

private:
    int                        cols = 0;
    int                        rows = 0;
    QVector<QVector<QTuiCell>> cells;
    QVector<QVector<QTuiCell>> prevCells; /* Previous frame for diff */
    bool                       fullRedraw = true;

    static QTuiCell defaultCell;
};

#endif // QTUISCREEN_H

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
 * @brief Background color. 0 = terminal default, non-zero = 256-palette index.
 */
using QTuiBgColor                       = std::uint8_t;
static constexpr QTuiBgColor BG_DEFAULT = 0;

/**
 * @brief A single terminal cell with character and style attributes
 */
struct QTuiCell
{
    QChar       character = ' ';
    bool        bold      = false;
    bool        italic    = false;
    bool        dim       = false;
    bool        underline = false;
    bool        inverted  = false;
    QTuiFgColor fgColor   = QTuiFgColor::Default;
    QTuiBgColor bgColor   = BG_DEFAULT;

    bool operator==(const QTuiCell &other) const
    {
        return character == other.character && bold == other.bold && italic == other.italic
               && dim == other.dim && underline == other.underline && inverted == other.inverted
               && fgColor == other.fgColor && bgColor == other.bgColor;
    }
    bool operator!=(const QTuiCell &other) const { return !(*this == other); }
};

/**
 * @brief A contiguous run of text sharing the same style attributes.
 * @details Bridge type between higher-level renderers (markdown,
 *          syntax highlight) and the cell-grid backing the screen.
 *          Keep field names short so renderer-side construction stays
 *          dense; QTuiCell mirrors the same flags using its own naming
 *          convention.
 */
struct QTuiStyledRun
{
    QString     text;
    bool        bold      = false;
    bool        italic    = false;
    bool        dim       = false;
    bool        underline = false;
    QTuiFgColor fg        = QTuiFgColor::Default;
    QTuiBgColor bg        = BG_DEFAULT;
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
        QTuiFgColor fgColor  = QTuiFgColor::Default,
        QTuiBgColor bgColor  = BG_DEFAULT);
    void putString(
        int            col,
        int            row,
        const QString &text,
        bool           bold     = false,
        bool           dim      = false,
        bool           inverted = false,
        QTuiFgColor    fgColor  = QTuiFgColor::Default,
        QTuiBgColor    bgColor  = BG_DEFAULT);

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

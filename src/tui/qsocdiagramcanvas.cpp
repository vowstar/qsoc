// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "tui/qsocdiagram_p.h"
#include "tui/qtuiwidget.h"
#include <algorithm>

namespace QSocDiagram::Detail {
Canvas::Canvas(int columns, int lines, int available)
    : width(columns)
    , height(lines)
    , availableWidth(available)
{
    require(columns > 0 && lines > 0 && qint64(columns) * lines <= canvasLimit, Error::Limit);
    cells.resize(columns * lines);
}
void Canvas::put(int x, int y, char32_t scalar, Role role)
{
    require(x >= 0 && x < width && y >= 0 && y < height, Error::Limit);
    cells[y * width + x] = {.scalar = scalar, .role = role};
}
char32_t Canvas::at(int x, int y) const
{
    return cells[y * width + x].scalar;
}
void Canvas::text(int x, int y, const QString &value, Role role)
{
    for (const char32_t scalar : value.toUcs4()) {
        put(x++, y, scalar, role);
        if (QTuiText::isWideChar(scalar))
            put(x++, y, 0, role);
    }
}
void Canvas::horizontal(int x1, int x2, int y, char32_t scalar, Role role)
{
    for (int x = std::min(x1, x2); x <= std::max(x1, x2); ++x) {
        const auto previous = at(x, y);
        put(x, y, previous == U'│' || previous == U'┆' ? U'╪' : scalar, role);
    }
}
void Canvas::vertical(int x, int y1, int y2, char32_t scalar, Role role)
{
    for (int y = std::min(y1, y2); y <= std::max(y1, y2); ++y) {
        const auto previous = at(x, y);
        put(x, y, previous == U'─' || previous == U'┄' ? U'╪' : scalar, role);
    }
}
void Canvas::box(int x, int y, int columns, int lines, bool rounded)
{
    horizontal(x, x + columns - 1, y, U'─', Role::Node);
    horizontal(x, x + columns - 1, y + lines - 1, U'─', Role::Node);
    vertical(x, y, y + lines - 1, U'│', Role::Node);
    vertical(x + columns - 1, y, y + lines - 1, U'│', Role::Node);
    put(x, y, rounded ? U'╭' : U'┌', Role::Node);
    put(x + columns - 1, y, rounded ? U'╮' : U'┐', Role::Node);
    put(x, y + lines - 1, rounded ? U'╰' : U'└', Role::Node);
    put(x + columns - 1, y + lines - 1, rounded ? U'╯' : U'┘', Role::Node);
}
Result Canvas::result() const
{
    Result result;
    for (int y = 0; y < height; ++y) {
        int end = width;
        while (end && at(end - 1, y) == U' ')
            --end;
        require(end <= availableWidth, Error::TooWide);
        QList<Span> spans;
        for (int x = 0; x < end; ++x) {
            const auto &cell = cells[y * width + x];
            if (!cell.scalar)
                continue;
            if (spans.isEmpty() || spans.last().role != cell.role)
                spans.append({.text = {}, .role = cell.role});
            spans.last().text += QString::fromUcs4(&cell.scalar, 1);
        }
        result.rows.append(spans);
    }
    return result;
}
} // namespace QSocDiagram::Detail

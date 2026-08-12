// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QTUIANSI_H
#define QTUIANSI_H

#include "tui/qtuiscreen.h"

#include <QList>
#include <QString>

/**
 * @brief Minimal SGR parser: external ANSI text to styled screen spans.
 * @details Understands the SGR subset a status script realistically
 *          emits: reset, bold, dim, inverse, 16-color and 256-color
 *          foreground/background, and 24-bit color (quantized to the
 *          256 palette). Every other escape sequence (cursor movement,
 *          OSC, unsupported SGR) is consumed and dropped so it can
 *          never corrupt the screen.
 */
namespace QTuiAnsi {

struct Span
{
    QString     text;
    bool        bold     = false;
    bool        dim      = false;
    bool        inverted = false;
    QTuiFgColor fg       = QTuiFgColor::Default;
    QTuiBgColor bg       = BG_DEFAULT;
};

/**
 * @brief Split ANSI text into styled spans.
 * @param text Text possibly containing escape sequences.
 * @return Spans in display order; escape bytes never appear in span text.
 */
QList<Span> parse(const QString &text);

/**
 * @brief Quantize a 24-bit color to the closest xterm-256 index.
 * @return Palette index in [16, 255].
 */
int rgbTo256(int red, int green, int blue);

} // namespace QTuiAnsi

#endif // QTUIANSI_H

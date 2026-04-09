// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QTUIWIDGET_H
#define QTUIWIDGET_H

#include "tui/qtuiscreen.h"

/**
 * @brief Base class for TUI widgets that render to a screen buffer
 */
class QTuiWidget
{
public:
    virtual ~QTuiWidget() = default;

    /* Return number of lines this widget currently occupies (0 = hidden) */
    virtual int lineCount() const = 0;

    /* Render to screen buffer starting at row startY */
    virtual void render(QTuiScreen &screen, int startY, int width) = 0;
};

/* Terminal text utilities */
namespace QTuiText {

bool    isWideChar(uint code);
int     visualWidth(const QString &text);
QString truncate(const QString &text, int maxWidth);
QString formatNumber(qint64 value);
QString formatDuration(qint64 seconds);

} // namespace QTuiText

#endif // QTUIWIDGET_H

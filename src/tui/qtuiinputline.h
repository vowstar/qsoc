// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QTUIINPUTLINE_H
#define QTUIINPUTLINE_H

#include "tui/qtuiwidget.h"

#include <QString>

/**
 * @brief User input line widget at bottom of screen
 */
class QTuiInputLine : public QTuiWidget
{
public:
    int  lineCount() const override;
    void render(QTuiScreen &screen, int startY, int width) override;

    void setText(const QString &text);
    void clear();

    /* Returns the column where the cursor should be (for IME positioning) */
    int cursorColumn() const;

private:
    QString text;
};

#endif // QTUIINPUTLINE_H

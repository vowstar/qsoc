// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QTUIUSERBLOCK_H
#define QTUIUSERBLOCK_H

#include "tui/qtuiblock.h"

#include <QList>
#include <QString>

/**
 * @brief Scrollback block for the user's input echo.
 * @details Shows what the user submitted to the agent, framed as a
 *          single quote with a blue left edge so it visually offsets
 *          from assistant prose and tool boxes. Multi-line input
 *          (paste, /multiline) flows naturally because every source
 *          line gets its own gutter row.
 *
 *          Copy:
 *          - toPlainText returns the raw input verbatim — what the
 *            user typed, no decoration.
 *          - toMarkdown wraps it in `> ...` blockquote rows so a
 *            paste back into another prompt reads as quoted user
 *            input rather than fresh assistant text.
 */
class QTuiUserBlock : public QTuiBlock
{
public:
    explicit QTuiUserBlock(QString text);

    void layout(int width) override;
    int  rowCount() const override;
    void paintRow(
        QTuiScreen &screen,
        int         screenRow,
        int         viewportRow,
        int         xOffset,
        int         width,
        bool        focused,
        bool        selected) const override;

    QString toPlainText() const override;
    QString toMarkdown() const override;

private:
    QString                     source;
    QStringList                 lines;
    QList<QList<QTuiStyledRun>> rendered;
};

#endif // QTUIUSERBLOCK_H

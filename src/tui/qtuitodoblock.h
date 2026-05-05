// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QTUITODOBLOCK_H
#define QTUITODOBLOCK_H

#include "tui/qtuiblock.h"
#include "tui/qtuitodolist.h"

#include <QList>
#include <QString>

/**
 * @brief Scrollback snapshot of the agent TODO list at a moment in time.
 * @details Pushed by the agent CLI after every todo_list result so the
 *          conversation history shows what was on the plan when the
 *          model checked it. The persistent QTuiTodoList widget is
 *          ephemeral (auto-clears done items after 30s); this block
 *          freezes a copy for retrospect + clipboard yank.
 *
 *          Visual: each row paints `[icon] N. title` with the icon
 *          colored by status — pending dim square, in-progress yellow
 *          hourglass, done green check, failed red cross.
 *
 *          Copy:
 *          - toPlainText returns plain "N. [icon] title" lines.
 *          - toMarkdown emits a GFM task list (`- [ ]` / `- [x]`).
 */
class QTuiTodoBlock : public QTuiBlock
{
public:
    explicit QTuiTodoBlock(QList<QTuiTodoList::TodoItem> items);

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

    bool    isFoldable() const override { return true; }
    QString toPlainText() const override;
    QString toMarkdown() const override;

private:
    QList<QTuiTodoList::TodoItem> items;
    QList<QList<QTuiStyledRun>>   rendered;
};

#endif // QTUITODOBLOCK_H

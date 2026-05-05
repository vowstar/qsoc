// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QTUIASSISTANTTEXTBLOCK_H
#define QTUIASSISTANTTEXTBLOCK_H

#include "tui/qtuiblock.h"

#include <QList>
#include <QString>

/**
 * @brief Markdown-rendered assistant prose block.
 * @details Owns a mutable markdown source string. layout() runs the
 *          shared markdown renderer at the requested width and
 *          caches the resulting wrapped styled-run rows. Streaming
 *          updates (appendMarkdown) invalidate the cache so the next
 *          paint pass refreshes from scratch; the markdown renderer
 *          is fast enough that re-running it on every chunk is fine
 *          and saves the diff bookkeeping that an incremental scheme
 *          would require.
 */
class QTuiAssistantTextBlock : public QTuiBlock
{
public:
    QTuiAssistantTextBlock() = default;
    explicit QTuiAssistantTextBlock(const QString &markdown);

    void           setMarkdown(const QString &markdown);
    void           appendMarkdown(const QString &chunk);
    const QString &markdown() const { return source; }

    /* Force every painted cell to be dim. Used by reasoning blocks so
     * the model's internal monologue reads as a quieter aside even
     * when the markdown has bold/colored runs of its own. */
    void setDimAll(bool dim) { forceDim = dim; }
    bool isDimAll() const { return forceDim; }

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
    QString toMarkdown() const override { return source; }

private:
    /* Each cached row is a list of styled runs whose total visual
     * width is <= layoutWidth. Built fresh on every layout() call
     * after a mutation or width change. */
    QString                     source;
    QList<QList<QTuiStyledRun>> rows;
    bool                        forceDim = false;
};

#endif // QTUIASSISTANTTEXTBLOCK_H

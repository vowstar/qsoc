// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QTUICODEBLOCK_H
#define QTUICODEBLOCK_H

#include "tui/qtuiblock.h"

#include <QList>
#include <QString>

/**
 * @brief Standalone fenced-code-block that lives outside markdown text.
 * @details Created by the compositor's streaming markdown splitter when
 *          a triple-backtick fence opens in the assistant or reasoning
 *          stream. Holds the language tag and the raw code body, plus
 *          a group id that ties multiple blocks from the same stream
 *          run together for auto-fold purposes.
 *
 *          Visual layout:
 *          - Header banner row `┄┄┄ <lang> ┄┄┄` (cyan dim).
 *          - One body row per source line with `▎ ` cyan-dim gutter
 *            followed by syntax-highlighted run via QSocCodeHighlighter.
 *          - Folded form collapses to a single summary row.
 *
 *          Copy semantics:
 *          - toPlainText() returns the raw code body verbatim — no
 *            fences, no banner, no gutter. This is the user-visible
 *            fix for "copy includes non-code special symbols".
 *          - toMarkdown() wraps the body in ` ```<lang>\n...\n``` `
 *            so paste-back into another markdown processor renders as
 *            a fenced code block again.
 *
 *          Reasoning blocks set forceDim so the body reads dim+italic
 *          to match the surrounding monologue palette.
 */
class QTuiCodeBlock : public QTuiBlock
{
public:
    QTuiCodeBlock(QString language, QString sourceCode, bool forceDim, int groupId);

    /* Streaming append: tack one more line of code onto sourceCode and
     * invalidate the layout. The trailing newline is supplied by the
     * caller so consecutive appends concatenate cleanly. */
    void appendBody(const QString &chunk);

    int  groupId() const { return groupId_; }
    bool forceDim() const { return forceDim_; }

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
    int     maxXOffset(int width) const override;
    QString toPlainText() const override;
    QString toMarkdown() const override;

private:
    QString language;
    QString sourceCode;
    bool    forceDim_ = false;
    int     groupId_  = 0;

    /* Cached layout, rebuilt on dirty. Each row is a list of styled
     * runs already including the gutter prefix; the banner row is
     * always at index 0 of the unfolded layout. */
    QList<QList<QTuiStyledRun>> rendered;
};

#endif // QTUICODEBLOCK_H

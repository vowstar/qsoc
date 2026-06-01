// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QTUIDIFFBLOCK_H
#define QTUIDIFFBLOCK_H

#include "tui/qtuiblock.h"
#include "tui/qtuitextlayout.h"

#include <QList>
#include <QString>
#include <QStringList>

/**
 * @brief Unified-diff block with line numbers and patch-format copy.
 * @details Holds a single file's diff record as a list of marker /
 *          payload pairs (`+`, `-`, ` `, `@@`) plus the `--- a/path`
 *          / `+++ b/path` headers.
 *
 *          Rendering puts a decorative gutter (right-aligned line
 *          number + `+`/`-`/space sign) ahead of the content, which
 *          soft-wraps to the viewport width; wrapped continuation rows
 *          carry a blank gutter. The gutter is decorative, so a
 *          mouse-drag copy yields the sign-stripped code content only.
 *
 *          toPlainText()/toMarkdown() still emit the unmodified unified
 *          diff (markers + headers) so Ctrl+Y produces a patch that
 *          `git apply` accepts. The block is foldable.
 */
class QTuiDiffBlock : public QTuiBlock
{
public:
    enum class Kind : std::uint8_t {
        Header,  /* --- a/path or +++ b/path */
        Hunk,    /* @@ -1,3 +1,4 @@ */
        Add,     /* +foo */
        Del,     /* -foo */
        Context, /*  unchanged */
    };

    struct Row
    {
        Kind    kind;
        QString text; /* Already includes the leading marker character */
    };

    explicit QTuiDiffBlock(QString headerA, QString headerB);

    /* Append a row to the diff body. Headers are appended automatically
     * during construction; callers add hunk markers, +, -, and context
     * lines in the order they should display. */
    void addRow(Kind kind, const QString &text);

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

    bool isFoldable() const override { return true; }

    QString toPlainText() const override;
    QString toMarkdown() const override;
    QString selectedLogicalText(
        int rowStartInBlock, int colStart, int rowEndInBlock, int colEnd) const override;

private:
    QString    headerA;
    QString    headerB;
    QList<Row> sourceRows;
    /* Cached visual rows (gutter + soft-wrapped content). A content
     * row's logicalLineIndex points into logicalLines_ (the sign-
     * stripped payload); header / hunk / summary rows carry -1 so a
     * drag-copy maps onto code content only. */
    QList<QTuiVisualRow> rows;
    QStringList          logicalLines_;
};

#endif // QTUIDIFFBLOCK_H

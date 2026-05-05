// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QTUIDIFFBLOCK_H
#define QTUIDIFFBLOCK_H

#include "tui/qtuiblock.h"

#include <QList>
#include <QString>
#include <QStringList>

/**
 * @brief Unified-diff block with patch-format clipboard output.
 * @details Holds a single file's diff record as a list of marker /
 *          payload pairs (`+`, `-`, ` `, `@@`) plus the `--- a/path`
 *          / `+++ b/path` headers. Rendering colors each row by
 *          marker; copying emits an unmodified unified diff that can
 *          be piped into `git apply` or `patch -p1` without further
 *          editing.
 *
 *          The block is foldable (collapsed → one summary row) and
 *          opts into horizontal scrolling for hunks wider than the
 *          viewport. Diff bodies frequently contain long generated
 *          lines and box-drawing tables, so wrapping them visually
 *          would obscure the patch shape.
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
    int  maxXOffset(int width) const override;

    QString toPlainText() const override;
    QString toMarkdown() const override;

private:
    QString    headerA;
    QString    headerB;
    QList<Row> sourceRows;
    /* Cached layout: each entry is the painted styled run for the
     * corresponding sourceRow. Folded form replaces this with a
     * single summary row; the copy paths still walk sourceRows to
     * preserve patch structure. */
    QList<QList<QTuiStyledRun>> rendered;
};

#endif // QTUIDIFFBLOCK_H

// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QTUITOOLBLOCK_H
#define QTUITOOLBLOCK_H

#include "tui/qtuiblock.h"

#include <QList>
#include <QString>

/**
 * @brief Bordered scrollback block for an agent tool invocation.
 * @details Captures one (tool_name, detail, body, status) record so
 *          the user can see what the agent ran and what it got back
 *          inline with the conversation. The block is foldable: a
 *          collapsed tool call shows just the header summary, while
 *          the expanded view shows the body output line by line.
 *
 *          Visual structure:
 *          - Header   `╭ tool_name · detail`
 *          - Body     `│ <output line N>`     (one row per source line)
 *          - Footer   `╰ <status>`            (only when finished)
 *
 *          Copy:
 *          - toPlainText returns `$ tool_name detail\n<body>` so the
 *            content pastes back into a shell-history-shaped form.
 *          - toMarkdown wraps the body in a fenced ```text``` block
 *            so structure round-trips cleanly through markdown.
 */
class QTuiToolBlock : public QTuiBlock
{
public:
    enum class Status : std::uint8_t {
        Running,
        Success,
        Failure,
    };

    QTuiToolBlock(QString toolName, QString detail);

    /* Append a fresh chunk of body output. Multi-line input is split
     * on `\n` so the layout produces one row per source line. */
    void appendBody(const QString &chunk);

    /* Mark the call as finished. Status renders the footer icon: ✓
     * for success, ✗ for failure. The detail label may carry summary
     * text shown after the icon. */
    void finish(Status status, const QString &summary);

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
    QString                     toolName;
    QString                     detail;
    QStringList                 body;
    Status                      status = Status::Running;
    QString                     summary;
    bool                        finished = false;
    QList<QList<QTuiStyledRun>> rows;
};

#endif // QTUITOOLBLOCK_H

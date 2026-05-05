// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "tui/qtuitoolblock.h"

#include "tui/qtuiwidget.h"

#include <utility>

namespace {

QTuiStyledRun dimRun(const QString &text)
{
    QTuiStyledRun run;
    run.text = text;
    run.dim  = true;
    return run;
}

QTuiStyledRun cyanDimRun(const QString &text)
{
    QTuiStyledRun run = dimRun(text);
    run.fg            = QTuiFgColor::Cyan;
    return run;
}

QTuiStyledRun colored(const QString &text, QTuiFgColor color, bool bold = false)
{
    QTuiStyledRun run;
    run.text = text;
    run.bold = bold;
    run.fg   = color;
    return run;
}

} // namespace

QTuiToolBlock::QTuiToolBlock(QString toolName, QString detail)
    : toolName(std::move(toolName))
    , detail(std::move(detail))
{}

void QTuiToolBlock::appendBody(const QString &chunk)
{
    if (chunk.isEmpty()) {
        return;
    }
    /* Re-split the existing tail with the new chunk so a partial line
     * carrying over from a previous append finishes correctly. The
     * trailing empty entry produced by a chunk ending in \n is the
     * "next line is empty so far" placeholder; we keep it so a
     * subsequent appendBody can extend it, and any caller-visible
     * extra row is removed at finalise time. */
    QString carry = body.isEmpty() ? QString() : body.takeLast();
    carry.append(chunk);
    const QStringList parts = carry.split(QLatin1Char('\n'));
    for (int idx = 0; idx < parts.size(); ++idx) {
        body.append(parts[idx]);
    }
    /* Drop a trailing empty if the chunk landed cleanly on a newline
     * boundary. Keeps "alpha\n" from rendering as two rows. */
    if (body.size() > 1 && body.last().isEmpty()) {
        body.removeLast();
    }
    invalidate();
}

void QTuiToolBlock::finish(Status status, const QString &summary)
{
    this->status   = status;
    this->summary  = summary;
    this->finished = true;
    invalidate();
}

void QTuiToolBlock::layout(int width)
{
    Q_UNUSED(width);
    if (!layoutDirty) {
        return;
    }
    layoutDirty = false;
    rows.clear();

    /* Header row: ╭ + tool name in cyan-bold + dim center dot + detail. */
    {
        QList<QTuiStyledRun> header;
        header.append(cyanDimRun(QStringLiteral("╭ ")));
        header.append(colored(toolName, QTuiFgColor::Cyan, /*bold=*/true));
        if (!detail.isEmpty()) {
            header.append(dimRun(QStringLiteral(" · ")));
            QTuiStyledRun detailRun;
            detailRun.text = detail;
            header.append(detailRun);
        }
        rows.append(header);
    }

    /* Folded form keeps just the header plus a one-line summary. */
    if (folded) {
        QList<QTuiStyledRun> hdr = rows.takeLast();
        QTuiStyledRun        suffix;
        suffix.text   = QStringLiteral("  ▸ %1 lines").arg(body.size());
        suffix.dim    = true;
        suffix.italic = true;
        hdr.append(suffix);
        rows.append(hdr);
        return;
    }

    /* Body rows: │ + raw line. Empty body yields zero body rows so a
     * tool with no output reads as just header + footer. */
    for (const QString &line : body) {
        QList<QTuiStyledRun> row;
        row.append(cyanDimRun(QStringLiteral("│ ")));
        QTuiStyledRun text;
        text.text = line;
        text.dim  = true;
        row.append(text);
        rows.append(row);
    }

    /* Footer row appears only after finish() has been called so a
     * still-running tool reads as a live, open box. */
    if (finished) {
        QList<QTuiStyledRun> footer;
        footer.append(cyanDimRun(QStringLiteral("╰ ")));
        switch (status) {
        case Status::Success:
            footer.append(colored(QStringLiteral("✓ "), QTuiFgColor::Green, /*bold=*/true));
            break;
        case Status::Failure:
            footer.append(colored(QStringLiteral("✗ "), QTuiFgColor::Red, /*bold=*/true));
            break;
        case Status::Running:
        default:
            footer.append(dimRun(QStringLiteral("· ")));
            break;
        }
        QTuiStyledRun summaryRun;
        summaryRun.text = summary.isEmpty()
                              ? QStringLiteral("done, %1 line%2")
                                    .arg(body.size())
                                    .arg(body.size() == 1 ? QString() : QStringLiteral("s"))
                              : summary;
        summaryRun.dim  = true;
        footer.append(summaryRun);
        rows.append(footer);
    }
}

int QTuiToolBlock::rowCount() const
{
    return static_cast<int>(rows.size());
}

void QTuiToolBlock::paintRow(
    QTuiScreen &screen,
    int         screenRow,
    int         viewportRow,
    int         xOffset,
    int         width,
    bool        focused,
    bool        selected) const
{
    Q_UNUSED(focused);
    Q_UNUSED(selected);
    if (viewportRow < 0 || viewportRow >= rows.size()) {
        return;
    }
    int       skipped = 0;
    int       painted = 0;
    const int effX    = std::max(0, xOffset);
    for (const QTuiStyledRun &run : rows[viewportRow]) {
        for (const QChar character : run.text) {
            const int chW = QTuiText::isWideChar(character.unicode()) ? 2 : 1;
            if (skipped + chW <= effX) {
                skipped += chW;
                continue;
            }
            if (skipped < effX) {
                skipped += chW;
                continue;
            }
            if (painted + chW > width) {
                return;
            }
            QTuiCell &cell = screen.at(painted, screenRow);
            cell.character = character;
            cell.bold      = run.bold;
            cell.italic    = run.italic;
            cell.dim       = run.dim;
            cell.underline = run.underline;
            cell.inverted  = false;
            cell.fgColor   = run.fg;
            cell.bgColor   = run.bg;
            cell.hyperlink = run.hyperlink;
            painted += chW;
        }
    }
}

QString QTuiToolBlock::toPlainText() const
{
    /* Shell-history shape: `$ tool detail` then the body lines. A
     * follow-up paste reads as a short reproduction script, which
     * matches what users want when copying a tool block. */
    QString out = QStringLiteral("$ %1").arg(toolName);
    if (!detail.isEmpty()) {
        out.append(QLatin1Char(' '));
        out.append(detail);
    }
    out.append(QLatin1Char('\n'));
    for (const QString &line : body) {
        out.append(line);
        out.append(QLatin1Char('\n'));
    }
    return out;
}

QString QTuiToolBlock::toMarkdown() const
{
    /* Wrap the body in a fenced code block tagged `text` so the
     * structure survives a round-trip through another markdown
     * renderer. The header line lives outside the fence as a normal
     * sentence. */
    QString out = QStringLiteral("**%1**").arg(toolName);
    if (!detail.isEmpty()) {
        out.append(QStringLiteral(" — `"));
        out.append(detail);
        out.append(QLatin1Char('`'));
    }
    out.append(QStringLiteral("\n\n```text\n"));
    for (const QString &line : body) {
        out.append(line);
        out.append(QLatin1Char('\n'));
    }
    out.append(QStringLiteral("```\n"));
    return out;
}

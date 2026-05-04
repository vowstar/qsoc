// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "tui/qtuiinputline.h"

#include <QStringList>
#include <QtGlobal>

namespace {
/* Primary and continuation line prompts */
const QString kPromptPrimary = QStringLiteral("> ");
const QString kPromptBang    = QStringLiteral("! ");
const QString kPromptCont    = QStringLiteral(". ");

QStringList splitLines(const QString &text)
{
    /* Split on '\n' preserving empty trailing segment so cursor on blank last line works */
    return text.split(QLatin1Char('\n'));
}

QString promptForLine(int lineIdx, bool startsWithBang)
{
    if (lineIdx == 0) {
        return startsWithBang ? kPromptBang : kPromptPrimary;
    }
    return kPromptCont;
}

/* Strip leading '!' from first line's display when bang prompt is used */
QString contentForDisplay(const QString &line, int lineIdx, bool startsWithBang)
{
    if (lineIdx == 0 && startsWithBang && !line.isEmpty() && line[0] == QLatin1Char('!')) {
        return line.mid(1);
    }
    return line;
}
} // namespace

int QTuiInputLine::takeFitChars(int startIdx, int capacity) const
{
    if (capacity <= 0) {
        return 0;
    }
    int idx   = startIdx;
    int width = 0;
    int len   = static_cast<int>(text.size());
    while (idx < len) {
        QChar qch = text[idx];
        if (qch == QLatin1Char('\n')) {
            break;
        }
        uint code;
        int  charLen;
        if (qch.isHighSurrogate() && idx + 1 < len && text[idx + 1].isLowSurrogate()) {
            code    = QChar::surrogateToUcs4(qch, text[idx + 1]);
            charLen = 2;
        } else {
            code    = qch.unicode();
            charLen = 1;
        }
        if (code < 0x20 || (code >= 0x7F && code < 0xA0)) {
            /* Skip zero-width control chars but still advance */
            idx += charLen;
            continue;
        }
        int cellW = QTuiText::isWideChar(code) ? 2 : 1;
        if (width + cellW > capacity) {
            /* Force-take at least one codepoint when capacity is too narrow
             * for a wide glyph but nothing has fit yet, otherwise we'd
             * spin forever producing empty rows. */
            if (width == 0) {
                idx += charLen;
            }
            break;
        }
        width += cellW;
        idx += charLen;
    }
    return idx - startIdx;
}

QVector<QTuiInputLine::VisualRow> QTuiInputLine::buildVisualRows() const
{
    QVector<VisualRow> rows;
    if (terminalWidth <= 0) {
        return rows;
    }

    bool startsWithBang = text.startsWith(QLatin1Char('!'));

    /* Walk the buffer one logical line at a time, tracking absolute QChar
     * offsets so cursor lookup can map cursorPos onto its visual row. */
    int  logicalIdx   = 0;
    int  logicalStart = 0;
    int  len          = static_cast<int>(text.size());
    int  idx          = 0;
    auto flushLogical = [&](int lineStart, int lineEnd) {
        QString prompt       = promptForLine(logicalIdx, startsWithBang);
        int     promptW      = QTuiText::visualWidth(prompt);
        int     contentStart = lineStart;
        if (logicalIdx == 0 && startsWithBang && contentStart < lineEnd
            && text[contentStart] == QLatin1Char('!')) {
            contentStart++;
        }

        bool firstSegment = true;
        int  segStart     = contentStart;
        while (segStart < lineEnd) {
            int capacity = (firstSegment ? terminalWidth - promptW : terminalWidth);
            if (capacity <= 0) {
                capacity = 1;
            }
            int taken = takeFitChars(segStart, capacity);
            /* Cap the slice at lineEnd in case takeFitChars happened to be
             * called past the logical line boundary (shouldn't happen since
             * '\n' breaks the inner loop, but be defensive). */
            if (segStart + taken > lineEnd) {
                taken = lineEnd - segStart;
            }
            VisualRow row;
            row.prompt       = firstSegment ? prompt : QString();
            row.promptWidth  = firstSegment ? promptW : 0;
            row.contentStart = segStart;
            row.contentLen   = taken;
            rows.append(row);
            if (taken <= 0) {
                /* Defensive: avoid an infinite loop on a degenerate width */
                break;
            }
            segStart += taken;
            firstSegment = false;
        }
        if (firstSegment) {
            /* Empty logical line still occupies one visual row showing the
             * prompt only (e.g. the cursor sits on a blank continuation). */
            VisualRow row;
            row.prompt       = prompt;
            row.promptWidth  = promptW;
            row.contentStart = contentStart;
            row.contentLen   = 0;
            rows.append(row);
        }
    };

    while (idx <= len) {
        if (idx == len || text[idx] == QLatin1Char('\n')) {
            flushLogical(logicalStart, idx);
            if (idx == len) {
                break;
            }
            logicalIdx++;
            idx++;
            logicalStart = idx;
            continue;
        }
        idx++;
    }

    return rows;
}

int QTuiInputLine::lineCount() const
{
    if (searchMode) {
        return 1;
    }
    QVector<VisualRow> rows  = buildVisualRows();
    int                total = rows.isEmpty() ? 1 : static_cast<int>(rows.size());
    return qMin(total, MAX_VISIBLE_LINES);
}

void QTuiInputLine::render(QTuiScreen &screen, int startY, int width)
{
    if (searchMode) {
        const QString label   = searchFailed ? QStringLiteral("(failing bck-i-search)`")
                                             : QStringLiteral("(bck-i-search)`");
        QString       display = label + searchQuery + QStringLiteral("': ") + searchMatch;
        screen.putString(0, startY, display.left(width));
        return;
    }

    /* Empty buffer with a placeholder set: show the dim hint after "> "
     * so new users can discover slash / @ / ! / Ctrl+R shortcuts. */
    if (text.isEmpty() && !placeholder.isEmpty()) {
        screen.putString(0, startY, QStringLiteral("> "));
        QString hint      = placeholder;
        int     available = width - 2;
        if (available > 0) {
            screen.putString(
                2,
                startY,
                QTuiText::truncate(hint, available),
                /*bold=*/false,
                /*dim=*/true);
        }
        return;
    }

    /* Recompute visual rows against the actual render width: the
     * compositor's cached terminalWidth is normally identical, but using
     * the parameter keeps render correct if a caller forgot to update. */
    int saved               = terminalWidth;
    terminalWidth           = width;
    QVector<VisualRow> rows = buildVisualRows();
    terminalWidth           = saved;

    int total    = static_cast<int>(rows.size());
    int firstVis = (total > MAX_VISIBLE_LINES) ? total - MAX_VISIBLE_LINES : 0;
    int visCount = qMin(total - firstVis, MAX_VISIBLE_LINES);

    for (int row = 0; row < visCount; row++) {
        const VisualRow &vrow    = rows[firstVis + row];
        QString          content = text.mid(vrow.contentStart, vrow.contentLen);
        QString          display = vrow.prompt + content;
        screen.putString(0, startY + row, display.left(width));
    }

    /* Trailing hint: only when the buffer occupies a single visual row. */
    if (!trailingHint.isEmpty() && total == 1 && visCount == 1) {
        const VisualRow &vrow      = rows[0];
        QString          content   = text.mid(vrow.contentStart, vrow.contentLen);
        int              column    = vrow.promptWidth + QTuiText::visualWidth(content) + 1;
        int              available = width - column;
        if (available > 0) {
            screen.putString(
                column,
                startY,
                trailingHint.left(available),
                /*bold=*/false,
                /*dim=*/true);
        }
    }
}

void QTuiInputLine::setText(const QString &newText)
{
    text      = newText;
    cursorPos = qMin(cursorPos, static_cast<int>(text.size()));
}

void QTuiInputLine::clear()
{
    text.clear();
    cursorPos = 0;
}

void QTuiInputLine::setTerminalWidth(int cols)
{
    if (cols > 0) {
        terminalWidth = cols;
    }
}

void QTuiInputLine::setCursorPos(int pos)
{
    int maxPos = static_cast<int>(text.size());
    cursorPos  = qBound(0, pos, maxPos);
}

int QTuiInputLine::cursorLine() const
{
    if (searchMode) {
        return 0;
    }
    QVector<VisualRow> rows = buildVisualRows();
    if (rows.isEmpty()) {
        return 0;
    }
    int total    = static_cast<int>(rows.size());
    int firstVis = (total > MAX_VISIBLE_LINES) ? total - MAX_VISIBLE_LINES : 0;

    /* Find the visual row whose [contentStart, contentStart+contentLen]
     * range contains cursorPos. The cursor at a row's right edge belongs
     * to that row when it is the final visual row of its logical line
     * (otherwise it logically belongs to the start of the next row). */
    int found = total - 1;
    for (int i = 0; i < total; i++) {
        const VisualRow &vrow                  = rows[i];
        int              rowEnd                = vrow.contentStart + vrow.contentLen;
        bool             isLastVisualOfLogical = (i == total - 1) || (rows[i + 1].promptWidth > 0);
        if (cursorPos < rowEnd || (cursorPos == rowEnd && isLastVisualOfLogical)) {
            found = i;
            break;
        }
    }

    int visRow = found - firstVis;
    return qBound(0, visRow, MAX_VISIBLE_LINES - 1);
}

int QTuiInputLine::cursorColumn() const
{
    if (searchMode) {
        /* Park cursor right after the query text so IME preedit lands there. */
        const QString label = searchFailed ? QStringLiteral("(failing bck-i-search)`")
                                           : QStringLiteral("(bck-i-search)`");
        return QTuiText::visualWidth(label) + QTuiText::visualWidth(searchQuery);
    }
    QVector<VisualRow> rows = buildVisualRows();
    if (rows.isEmpty()) {
        return 0;
    }
    int total = static_cast<int>(rows.size());
    int found = total - 1;
    for (int i = 0; i < total; i++) {
        const VisualRow &vrow                  = rows[i];
        int              rowEnd                = vrow.contentStart + vrow.contentLen;
        bool             isLastVisualOfLogical = (i == total - 1) || (rows[i + 1].promptWidth > 0);
        if (cursorPos < rowEnd || (cursorPos == rowEnd && isLastVisualOfLogical)) {
            found = i;
            break;
        }
    }
    const VisualRow &vrow = rows[found];
    int     clamp  = qBound(vrow.contentStart, cursorPos, vrow.contentStart + vrow.contentLen);
    QString before = text.mid(vrow.contentStart, clamp - vrow.contentStart);
    return vrow.promptWidth + QTuiText::visualWidth(before);
}

void QTuiInputLine::setSearchMode(
    bool active, const QString &query, const QString &match, bool failed)
{
    searchMode   = active;
    searchQuery  = query;
    searchMatch  = match;
    searchFailed = failed;
}

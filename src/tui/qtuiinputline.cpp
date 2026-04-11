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

/* Compute first visible line index (scroll when content exceeds MAX_VISIBLE_LINES) */
int firstVisibleLine(int totalLines)
{
    if (totalLines <= QTuiInputLine::MAX_VISIBLE_LINES) {
        return 0;
    }
    return totalLines - QTuiInputLine::MAX_VISIBLE_LINES;
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

int QTuiInputLine::lineCount() const
{
    if (searchMode) {
        return 1;
    }
    int total = 1 + static_cast<int>(text.count(QLatin1Char('\n')));
    total     = qMax(total, 1);
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

    QStringList lines          = splitLines(text);
    int         total          = static_cast<int>(lines.size());
    bool        startsWithBang = text.startsWith(QLatin1Char('!'));
    int         firstVis       = firstVisibleLine(total);
    int         visCount       = qMin(total - firstVis, MAX_VISIBLE_LINES);

    for (int row = 0; row < visCount; row++) {
        int     lineIdx = firstVis + row;
        QString prompt  = promptForLine(lineIdx, startsWithBang);
        QString content = contentForDisplay(lines[lineIdx], lineIdx, startsWithBang);
        QString display = prompt + content;
        screen.putString(0, startY + row, display.left(width));
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
    /* Count newlines before cursorPos to find which logical line the cursor is on */
    int logicalLine = 0;
    int limit       = qMin(cursorPos, static_cast<int>(text.size()));
    for (int idx = 0; idx < limit; idx++) {
        if (text[idx] == QLatin1Char('\n')) {
            logicalLine++;
        }
    }

    int total    = 1 + static_cast<int>(text.count(QLatin1Char('\n')));
    int firstVis = firstVisibleLine(total);
    int visRow   = logicalLine - firstVis;
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

    /* Find start of current logical line */
    int lineStart = cursorPos;
    while (lineStart > 0 && text[lineStart - 1] != QLatin1Char('\n')) {
        lineStart--;
    }

    /* Count logical line index to choose prompt prefix */
    int logicalLine = 0;
    for (int idx = 0; idx < lineStart; idx++) {
        if (text[idx] == QLatin1Char('\n')) {
            logicalLine++;
        }
    }

    bool    startsWithBang = text.startsWith(QLatin1Char('!'));
    QString prompt         = promptForLine(logicalLine, startsWithBang);

    /* Content up to cursor on current line */
    QString segment = text.mid(lineStart, cursorPos - lineStart);
    if (logicalLine == 0 && startsWithBang && !segment.isEmpty() && segment[0] == QLatin1Char('!')) {
        segment = segment.mid(1);
    }

    return QTuiText::visualWidth(prompt) + QTuiText::visualWidth(segment);
}

void QTuiInputLine::setSearchMode(
    bool active, const QString &query, const QString &match, bool failed)
{
    searchMode   = active;
    searchQuery  = query;
    searchMatch  = match;
    searchFailed = failed;
}

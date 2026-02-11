// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "cli/qagentstatusline.h"

#include <QTextStream>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <sys/ioctl.h>
#include <unistd.h>
#endif

/* Check if a Unicode code point is a wide (double-width) terminal character */
static bool isWideChar(uint code)
{
    return (code >= 0x1100 && code <= 0x115F)       /* Hangul Jamo */
           || code == 0x2329 || code == 0x232A      /* Angle brackets */
           || (code >= 0x2E80 && code <= 0x303E)    /* CJK Radicals, Kangxi, CJK Symbols */
           || (code >= 0x3040 && code <= 0x33BF)    /* Hiragana, Katakana, Bopomofo */
           || (code >= 0x3400 && code <= 0x4DBF)    /* CJK Extension A */
           || (code >= 0x4E00 && code <= 0x9FFF)    /* CJK Unified Ideographs */
           || (code >= 0xA000 && code <= 0xA4CF)    /* Yi */
           || (code >= 0xAC00 && code <= 0xD7AF)    /* Hangul Syllables */
           || (code >= 0xF900 && code <= 0xFAFF)    /* CJK Compatibility Ideographs */
           || (code >= 0xFE10 && code <= 0xFE19)    /* Vertical Forms */
           || (code >= 0xFE30 && code <= 0xFE6F)    /* CJK Compatibility Forms */
           || (code >= 0xFF01 && code <= 0xFF60)    /* Fullwidth Forms */
           || (code >= 0xFFE0 && code <= 0xFFE6)    /* Fullwidth Signs */
           || (code >= 0x20000 && code <= 0x2FFFF)  /* CJK Extension B-F */
           || (code >= 0x30000 && code <= 0x3FFFF); /* CJK Extension G+ */
}

/* Calculate terminal visual width of a string (handles CJK double-width + ANSI escapes) */
static int visualWidth(const QString &text)
{
    int width = 0;
    int idx   = 0;
    int len   = text.length();

    while (idx < len) {
        QChar ch = text[idx];

        /* Skip ANSI CSI sequences: ESC [ ... final_byte(0x40-0x7E) */
        if (ch == '\033' && idx + 1 < len && text[idx + 1] == '[') {
            idx += 2;
            while (idx < len) {
                ushort code = text[idx].unicode();
                ++idx;
                if (code >= 0x40 && code <= 0x7E) {
                    break;
                }
            }
            continue;
        }

        /* Skip other ESC sequences (ESC + single char) */
        if (ch == '\033') {
            idx += 2;
            continue;
        }

        /* Handle surrogate pairs for characters beyond BMP */
        uint codePoint;
        int  charLen;
        if (ch.isHighSurrogate() && idx + 1 < len && text[idx + 1].isLowSurrogate()) {
            codePoint = QChar::surrogateToUcs4(ch, text[idx + 1]);
            charLen   = 2;
        } else {
            codePoint = ch.unicode();
            charLen   = 1;
        }

        /* Control characters have zero width */
        if (codePoint < 0x20 || (codePoint >= 0x7F && codePoint < 0xA0)) {
            idx += charLen;
            continue;
        }

        width += isWideChar(codePoint) ? 2 : 1;
        idx += charLen;
    }

    return width;
}

/* Truncate string to fit within maxWidth terminal columns, appending "..." if needed */
static QString truncateToVisualWidth(const QString &text, int maxWidth)
{
    if (visualWidth(text) <= maxWidth) {
        return text;
    }

    int targetWidth = maxWidth - 3; /* Reserve 3 columns for "..." */
    if (targetWidth <= 0) {
        return "...";
    }

    int width = 0;
    int idx   = 0;
    int len   = text.length();

    while (idx < len) {
        QChar ch = text[idx];

        /* Preserve ANSI CSI sequences (zero visual width) */
        if (ch == '\033' && idx + 1 < len && text[idx + 1] == '[') {
            idx += 2;
            while (idx < len) {
                ushort code = text[idx].unicode();
                ++idx;
                if (code >= 0x40 && code <= 0x7E) {
                    break;
                }
            }
            continue;
        }

        if (ch == '\033') {
            idx += 2;
            continue;
        }

        uint codePoint;
        int  charLen;
        if (ch.isHighSurrogate() && idx + 1 < len && text[idx + 1].isLowSurrogate()) {
            codePoint = QChar::surrogateToUcs4(ch, text[idx + 1]);
            charLen   = 2;
        } else {
            codePoint = ch.unicode();
            charLen   = 1;
        }

        if (codePoint < 0x20 || (codePoint >= 0x7F && codePoint < 0xA0)) {
            idx += charLen;
            continue;
        }

        int charWidth = isWideChar(codePoint) ? 2 : 1;
        if (width + charWidth > targetWidth) {
            break;
        }

        width += charWidth;
        idx += charLen;
    }

    return text.left(idx) + "...";
}

QAgentStatusLine::QAgentStatusLine(QObject *parent)
    : QObject(parent)
    , spinnerTimer(new QTimer(this))
{
    connect(spinnerTimer, &QTimer::timeout, this, &QAgentStatusLine::onTimer);
}

QAgentStatusLine::~QAgentStatusLine()
{
    if (active) {
        stop();
    }
}

void QAgentStatusLine::start(const QString &status)
{
    if (active) {
        return;
    }

    currentStatus = status;
    spinnerIndex  = 0;
    dotIndex      = 0;
    toolCallCount = 0;
    inputTokens   = 0;
    outputTokens  = 0;
    thinkingLevel.clear();
    active = true;
    stepElapsedTimer.start();
    totalElapsedTimer.start();
    render();
    spinnerTimer->start(100); /* 100ms refresh rate */
}

void QAgentStatusLine::update(const QString &status)
{
    if (!active) {
        return;
    }

    currentStatus = status;
    render();
}

void QAgentStatusLine::toolCalled(const QString &toolName, const QString &detail)
{
    if (!active) {
        return;
    }

    toolCallCount++;
    stepElapsedTimer.restart(); /* Reset timer on new tool call */

    /* Output tool call to history (scrolling output) */
    QTextStream out(stdout);

    /* Clear TODO list + status line before printing tool info */
    if (displayedTodoLineCount > 0) {
        out << QString("\033[%1A").arg(displayedTodoLineCount); /* Move up to TODO start */
    }
    out << "\r\033[J"; /* Clear from cursor to end of screen */

    /* Flush partial content permanently (tool call is a natural break) */
    if (!currentPartialLine.isEmpty()) {
        out << currentPartialLine << "\n";
        currentPartialLine.clear();
    }
    if (!pendingContent.isEmpty()) {
        out << pendingContent;
        if (!pendingContent.endsWith('\n')) {
            out << "\n";
        }
        pendingContent.clear();
    }

    /* Simple format: [Tool] name: detail */
    if (detail.isEmpty()) {
        out << "[Tool] " << toolName << Qt::endl;
    } else {
        QString line      = QString("[Tool] %1: %2").arg(toolName, detail);
        int     termWidth = getTerminalWidth();
        line              = truncateToVisualWidth(line, termWidth - 1);
        out << line << Qt::endl;
    }

    /* Reset displayed count - render() will redraw TODO list */
    displayedTodoLineCount = 0;

    currentStatus = QString("Running %1...").arg(toolName);
    render();
}

void QAgentStatusLine::resetProgress()
{
    if (!active) {
        return;
    }

    stepElapsedTimer.restart();
}

void QAgentStatusLine::stop()
{
    if (!active) {
        return;
    }

    spinnerTimer->stop();
    active = false;

    /* Flush partial content + pending content permanently */
    if (!currentPartialLine.isEmpty() || !pendingContent.isEmpty()) {
        QTextStream out(stdout);
        if (displayedTodoLineCount > 0) {
            out << QString("\033[%1A").arg(displayedTodoLineCount);
        }
        out << "\r\033[J";
        out << currentPartialLine << pendingContent << Qt::flush;
        currentPartialLine.clear();
        pendingContent.clear();
        displayedTodoLineCount = 0;
    }

    clearLine();

    /* Clear TODO, queue, and input state */
    todoItems.clear();
    queuedRequests.clear();
    activeTodoId           = -1;
    displayedTodoLineCount = 0;
    inputLineText.clear();
}

bool QAgentStatusLine::isActive() const
{
    return active;
}

void QAgentStatusLine::onTimer()
{
    spinnerIndex = (spinnerIndex + 1) % static_cast<int>(spinnerFrames.size());
    dotIndex     = (dotIndex + 1) % static_cast<int>(dotFrames.size());
    render();
}

int QAgentStatusLine::getTerminalWidth() const
{
#ifdef Q_OS_WIN
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
        return csbi.srWindow.Right - csbi.srWindow.Left + 1;
    }
    return 80;
#else
    struct winsize termSize;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &termSize) == 0 && termSize.ws_col > 0) {
        return termSize.ws_col;
    }
    return 80;
#endif
}

void QAgentStatusLine::render()
{
    QString spinner = spinnerFrames[spinnerIndex];
    QString dots    = dotFrames[dotIndex];

    /* Calculate step elapsed time (internal use for slow detection) */
    qint64 stepMs      = stepElapsedTimer.elapsed();
    qint64 stepSeconds = stepMs / 1000;

    /* Calculate total elapsed time */
    qint64 totalMs      = totalElapsedTimer.elapsed();
    qint64 totalSeconds = totalMs / 1000;

    /* Format compact time string */
    QString timeStr = formatDuration(totalSeconds);

    /* Format token usage: compact format like "1.2k/3.4k" */
    QString tokenStr;
    if (inputTokens > 0 || outputTokens > 0) {
        tokenStr = QString("%1/%2").arg(formatNumber(inputTokens), formatNumber(outputTokens));
    }

    /* Tool call count indicator: [N tools] */
    QString toolInfo;
    if (toolCallCount > 0) {
        toolInfo = QString("[%1 tools]").arg(toolCallCount);
    }

    /* Add warning for long step wait times (based on step timer, not total) */
    QString warning;
    if (stepSeconds >= 120) {
        warning = " [Slow!]";
    } else if (stepSeconds >= 60) {
        warning = " [Slow]";
    }

    /* Build status line: spinner status... (tokens time) [N tools] [warning] */
    /* Format: \ Working... (1.2k/3.4k 1:54) [21 tools] */
    QString statusLine;
    if (tokenStr.isEmpty()) {
        statusLine = QString("%1 %2%3 (%4)").arg(spinner, currentStatus, dots, timeStr);
    } else {
        statusLine = QString("%1 %2%3 (%4 %5)").arg(spinner, currentStatus, dots, tokenStr, timeStr);
    }

    if (!toolInfo.isEmpty()) {
        statusLine += " " + toolInfo;
    }
    if (!thinkingLevel.isEmpty()) {
        statusLine += QString(" [T:%1]").arg(thinkingLevel);
    }
    statusLine += warning;

    /* Get terminal width and truncate if necessary */
    int termWidth = getTerminalWidth();
    statusLine    = truncateToVisualWidth(statusLine, termWidth - 1);

    QTextStream out(stdout);

    /* Calculate lines to clear: TODO list + status line */
    int todoLineCount = qMin(todoItems.size(), 5); /* Limit to 5 items */

    /* Move cursor up and clear if we have TODO lines displayed */
    if (displayedTodoLineCount > 0) {
        out << QString("\033[%1A").arg(displayedTodoLineCount); /* Move up */
    }
    out << "\r\033[J"; /* Clear from cursor to end of screen */

    /* Process pending content: split complete lines to permanent scrollback */
    if (!pendingContent.isEmpty()) {
        QString allContent = currentPartialLine + pendingContent;
        pendingContent.clear();

        int lastNewline = allContent.lastIndexOf('\n');
        if (lastNewline >= 0) {
            out << allContent.left(lastNewline + 1); /* Permanent scrollback */
            currentPartialLine = allContent.mid(lastNewline + 1);
        } else {
            currentPartialLine = allContent; /* No complete lines yet */
        }
    }

    /* Output partial content line (ephemeral - redrawn each render) */
    if (!currentPartialLine.isEmpty()) {
        out << "\033[?7l"; /* Disable auto-wrap: safety net for long lines */
        out << truncateToVisualWidth(currentPartialLine, termWidth - 1);
        out << "\033[?7h"; /* Re-enable auto-wrap */
    }

    /* Separator between partial content and TODO items */
    if (!currentPartialLine.isEmpty() && todoLineCount > 0) {
        out << "\n";
    }

    /* Render TODO list (limit to 5 most recent items) */
    int startIdx = qMax(0, todoItems.size() - 5);
    for (int i = startIdx; i < todoItems.size(); ++i) {
        const TodoItem &item = todoItems[i];
        QString         checkbox;

        if (item.status == "done") {
            checkbox = "[x]";
        } else if (item.id == activeTodoId) {
            /* Blink effect: alternate [*] and [ ] with uniform 500ms/500ms timing */
            checkbox = (spinnerIndex % 10 < 5) ? "[*]" : "[ ]";
        } else {
            checkbox = "[ ]";
        }

        QString line = QString("%1 %2 (%3)").arg(checkbox, item.title, item.priority);

        /* Truncate if too long (visual width for CJK) */
        line = truncateToVisualWidth(line, termWidth - 1);

        out << line << "\n";
    }

    /* Render queued requests (between TODO and status bar) */
    int queueLineCount = qMin(static_cast<int>(queuedRequests.size()), 3);
    int queueStartIdx  = qMax(0, static_cast<int>(queuedRequests.size()) - 3);
    for (int i = queueStartIdx; i < queuedRequests.size(); ++i) {
        QString line = "\033[2m> " + queuedRequests[i] + "\033[0m";
        line         = truncateToVisualWidth(line, termWidth - 1);
        out << line << "\n";
    }

    /* Update displayed line count: truncation + DECAWM guarantees no wrapping */
    int hasSeparator       = (!currentPartialLine.isEmpty() && todoLineCount > 0) ? 1 : 0;
    int hasInputLine       = !inputLineText.isEmpty() ? 1 : 0;
    displayedTodoLineCount = hasSeparator + todoLineCount + queueLineCount + hasInputLine;

    /* Output status line */
    out << statusLine;

    /* Output input line below status bar if user is typing */
    if (!inputLineText.isEmpty()) {
        QString inputDisplay = "> " + inputLineText;
        inputDisplay         = truncateToVisualWidth(inputDisplay, termWidth - 1);
        out << "\n" << inputDisplay;
    }

    out << Qt::flush;
}

void QAgentStatusLine::clearLine()
{
    QTextStream out(stdout);

    /* Move cursor up to clear TODO lines if any */
    if (displayedTodoLineCount > 0) {
        out << QString("\033[%1A").arg(displayedTodoLineCount);
    }
    out << "\r\033[J" << Qt::flush; /* Clear from cursor to end of screen */

    /* Reset displayed line count and partial content */
    displayedTodoLineCount = 0;
    currentPartialLine.clear();
}

void QAgentStatusLine::printContent(const QString &content)
{
    if (active) {
        stepElapsedTimer.restart();
        pendingContent += content;
        /* Flushed by render() on next timer tick (max 100ms delay) */
    } else {
        QTextStream out(stdout);
        out << content << Qt::flush;
    }
}

void QAgentStatusLine::setInputLine(const QString &text)
{
    inputLineText = text;
    if (active) {
        render();
    }
}

void QAgentStatusLine::clearInputLine()
{
    inputLineText.clear();
}

void QAgentStatusLine::addQueuedRequest(const QString &text)
{
    queuedRequests.append(text);
    if (active) {
        render();
    }
}

void QAgentStatusLine::removeQueuedRequest(const QString &text)
{
    int idx = queuedRequests.indexOf(text);
    if (idx >= 0) {
        queuedRequests.removeAt(idx);
    }
    if (active) {
        render();
    }
}

void QAgentStatusLine::clearQueuedRequests()
{
    queuedRequests.clear();
    if (active) {
        render();
    }
}

void QAgentStatusLine::updateTodoDisplay(const QString &todoResult)
{
    /* Cache the result for reference */
    if (todoResult.startsWith("Todo List:") || todoResult.startsWith("No todos found")) {
        todoListCache = todoResult;
    }

    /* No scrolling output - TODO list is displayed persistently via render() */
    /* Just trigger a render to update the display */
    if (active) {
        render();
    }
}

QString QAgentStatusLine::getTodoTitle(int todoId) const
{
    for (const auto &item : todoItems) {
        if (item.id == todoId) {
            return item.title;
        }
    }
    return {};
}

void QAgentStatusLine::updateTokens(qint64 input, qint64 output)
{
    inputTokens  = input;
    outputTokens = output;
}

void QAgentStatusLine::setThinkingLevel(const QString &level)
{
    thinkingLevel = level;
}

void QAgentStatusLine::setTodoList(const QList<TodoItem> &items)
{
    todoItems = items;
    if (active) {
        render();
    }
}

void QAgentStatusLine::setActiveTodo(int todoId)
{
    activeTodoId = todoId;

    /* Also update status in todoItems if present */
    for (int i = 0; i < todoItems.size(); ++i) {
        if (todoItems[i].id == todoId) {
            todoItems[i].status = "in_progress";
        } else if (todoItems[i].status == "in_progress") {
            /* Reset previous in_progress to pending */
            todoItems[i].status = "pending";
        }
    }

    if (active) {
        render();
    }
}

void QAgentStatusLine::clearActiveTodo()
{
    activeTodoId = -1;
    if (active) {
        render();
    }
}

void QAgentStatusLine::addTodoItem(const TodoItem &item)
{
    /* Check if item already exists (by ID) */
    for (int i = 0; i < todoItems.size(); ++i) {
        if (todoItems[i].id == item.id) {
            todoItems[i] = item; /* Update existing */
            if (active) {
                render();
            }
            return;
        }
    }
    /* Add new item */
    todoItems.append(item);
    if (active) {
        render();
    }
}

void QAgentStatusLine::updateTodoStatus(int todoId, const QString &newStatus)
{
    for (int i = 0; i < todoItems.size(); ++i) {
        if (todoItems[i].id == todoId) {
            todoItems[i].status = newStatus;
            if (newStatus == "in_progress") {
                activeTodoId = todoId;
            } else if (todoId == activeTodoId) {
                activeTodoId = -1;
            }
            if (active) {
                render();
            }
            return;
        }
    }
}

QString QAgentStatusLine::formatNumber(qint64 value)
{
    if (value < 1000) {
        return QString::number(value);
    }
    if (value < 1000000) {
        /* k: 1,000 - 999,999 */
        double k = value / 1000.0;
        if (k < 10) {
            return QString::number(k, 'f', 1) + "k";
        }
        return QString::number(static_cast<int>(k)) + "k";
    }
    if (value < 1000000000) {
        /* M: 1,000,000 - 999,999,999 */
        double m = value / 1000000.0;
        if (m < 10) {
            return QString::number(m, 'f', 1) + "M";
        }
        return QString::number(static_cast<int>(m)) + "M";
    }
    /* G: 1,000,000,000+ */
    double g = value / 1000000000.0;
    if (g < 10) {
        return QString::number(g, 'f', 1) + "G";
    }
    return QString::number(static_cast<int>(g)) + "G";
}

QString QAgentStatusLine::formatDuration(qint64 seconds)
{
    if (seconds < 60) {
        /* Less than 1 minute: just seconds */
        return QString::number(seconds) + "s";
    }
    if (seconds < 3600) {
        /* Less than 1 hour: m:ss */
        int min = static_cast<int>(seconds / 60);
        int sec = static_cast<int>(seconds % 60);
        return QString("%1:%2").arg(min).arg(sec, 2, 10, QChar('0'));
    }
    if (seconds < 86400) {
        /* Less than 1 day: h:mm:ss */
        int hrs = static_cast<int>(seconds / 3600);
        int min = static_cast<int>((seconds % 3600) / 60);
        int sec = static_cast<int>(seconds % 60);
        return QString("%1:%2:%3").arg(hrs).arg(min, 2, 10, QChar('0')).arg(sec, 2, 10, QChar('0'));
    }
    /* 1 day or more: Xd h:mm:ss */
    int days = static_cast<int>(seconds / 86400);
    int hrs  = static_cast<int>((seconds % 86400) / 3600);
    int min  = static_cast<int>((seconds % 3600) / 60);
    int sec  = static_cast<int>(seconds % 60);
    return QString("%1d%2:%3:%4")
        .arg(days)
        .arg(hrs)
        .arg(min, 2, 10, QChar('0'))
        .arg(sec, 2, 10, QChar('0'));
}

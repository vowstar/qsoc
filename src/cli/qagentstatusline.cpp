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
    active        = true;
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

    /* Simple format: [Tool] name: detail */
    if (detail.isEmpty()) {
        out << "[Tool] " << toolName << Qt::endl;
    } else {
        QString line      = QString("[Tool] %1: %2").arg(toolName, detail);
        int     termWidth = getTerminalWidth();
        if (line.length() > termWidth - 1) {
            line = line.left(termWidth - 4) + "...";
        }
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
    clearLine();

    /* Clear TODO state */
    todoItems.clear();
    activeTodoId           = -1;
    displayedTodoLineCount = 0;
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
    statusLine += warning;

    /* Get terminal width and truncate if necessary */
    int termWidth = getTerminalWidth();
    if (statusLine.length() > termWidth - 1) {
        /* Leave room for cursor, truncate with "..." */
        statusLine = statusLine.left(termWidth - 4) + "...";
    }

    QTextStream out(stdout);

    /* Calculate lines to clear: TODO list + status line */
    int todoLineCount = qMin(todoItems.size(), 5); /* Limit to 5 items */
    int totalLines    = todoLineCount + 1;

    /* Move cursor up and clear if we have TODO lines displayed */
    if (displayedTodoLineCount > 0) {
        out << QString("\033[%1A").arg(displayedTodoLineCount); /* Move up */
    }
    out << "\r\033[J"; /* Clear from cursor to end of screen */

    /* Render TODO list (limit to 5 most recent items) */
    int startIdx = qMax(0, todoItems.size() - 5);
    for (int i = startIdx; i < todoItems.size(); ++i) {
        const TodoItem &item = todoItems[i];
        QString         checkbox;

        if (item.status == "done") {
            checkbox = "[x]";
        } else if (item.id == activeTodoId) {
            /* Current item: use spinner for animation */
            checkbox = QString("[%1]").arg(spinner);
        } else {
            checkbox = "[ ]";
        }

        QString line
            = QString("%1 %2. %3 (%4)").arg(checkbox).arg(item.id).arg(item.title, item.priority);

        /* Truncate if too long */
        if (line.length() > termWidth - 1) {
            line = line.left(termWidth - 4) + "...";
        }

        out << line << "\n";
    }

    /* Update displayed line count for next render */
    displayedTodoLineCount = todoLineCount;

    /* Output status line */
    out << statusLine << Qt::flush;
}

void QAgentStatusLine::clearLine()
{
    QTextStream out(stdout);

    /* Move cursor up to clear TODO lines if any */
    if (displayedTodoLineCount > 0) {
        out << QString("\033[%1A").arg(displayedTodoLineCount);
    }
    out << "\r\033[J" << Qt::flush; /* Clear from cursor to end of screen */

    /* Reset displayed line count */
    displayedTodoLineCount = 0;
}

void QAgentStatusLine::printContent(const QString &content)
{
    QTextStream out(stdout);

    if (active) {
        /* Reset timer - content output is progress */
        stepElapsedTimer.restart();

        /* Clear TODO list + status line before printing content */
        if (displayedTodoLineCount > 0) {
            out << QString("\033[%1A").arg(displayedTodoLineCount);
        }
        out << "\r\033[J";

        /* Output content */
        out << content << Qt::flush;

        /* Reset displayed count - render() will redraw TODO list */
        displayedTodoLineCount = 0;

        /* Redraw status line */
        render();
    } else {
        /* Not active, just output content */
        out << content << Qt::flush;
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

void QAgentStatusLine::updateTokens(qint64 input, qint64 output)
{
    inputTokens  = input;
    outputTokens = output;
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
